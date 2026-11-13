#include "gpu/native/hd_textures.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <fmt/format.h>
#include <lodepng.h>

#include "common/utf8.h"
#include "gpu/texture/decode.h"
#include "gpu/texture/hash.h"
#include "hdpack/image.h"
#include "hdpack/pack.h"
#include "mh4n/log.h"

namespace mh4n::gpu::native {
namespace {

constexpr const char* kLog = "GPU.HdTextures";
namespace fs = std::filesystem;

// A PNG's size from its header (the signature, then IHDR: width and height big-endian at 16 and 20)
bool PngSize(const std::string& path, u32& width, u32& height) {
    std::ifstream in(utf8::PathFromUtf8(path), std::ios::binary);
    unsigned char h[24];
    if (!in.read(reinterpret_cast<char*>(h), sizeof h) || std::memcmp(h, "\x89PNG", 4) != 0 || std::memcmp(h + 12, "IHDR", 4) != 0) {
        return false;
    }
    width = (u32{h[16]} << 24) | (u32{h[17]} << 16) | (u32{h[18]} << 8) | h[19];
    height = (u32{h[20]} << 24) | (u32{h[21]} << 16) | (u32{h[22]} << 8) | h[23];
    return width > 0 && height > 0 && width <= hdpack::kMaxDimension && height <= hdpack::kMaxDimension;
}

u64 AnswerKey(u64 content, u32 width, u32 height, texture::Format format) {
    return content ^ (u64{width} << 48) ^ (u64{height} << 32) ^ (u64{static_cast<u32>(format)} << 24);
}

}  // namespace

void HdTextures::Load(const std::string& root) {
    if (const char* d = std::getenv("MH4N_HD_DUMP"); d != nullptr && *d != '\0') {
        dump_dir_ = d;
        std::error_code ec;
        fs::create_directories(utf8::PathFromUtf8(dump_dir_), ec);
        MH4N_LOG(Info, kLog, "MH4N_HD_DUMP: each texture drawn is written to {} once", dump_dir_);
    }
    if (const char* off = std::getenv("MH4N_HD_TEXTURES"); off != nullptr && *off == '0') {
        MH4N_LOG(Info, kLog, "MH4N_HD_TEXTURES=0: no texture pack is read");
        return;
    }
    std::error_code ec;
    if (root.empty() || !fs::is_directory(utf8::PathFromUtf8(root), ec)) {
        if (!root.empty()) MH4N_LOG(Info, kLog, "no texture packs ({} is not a folder)", root);
        return;
    }
    // each folder under the root is a pack (Azahar's load/textures/<title id>/, or the pack's own folder), and the root
    // itself when it holds texture files or a pack.json
    std::vector<fs::path> dirs;
    for (const auto& e : fs::directory_iterator(utf8::PathFromUtf8(root), fs::directory_options::skip_permission_denied, ec)) {
        std::error_code de;
        if (e.is_directory(de)) dirs.push_back(e.path());
    }
    std::sort(dirs.begin(), dirs.end());
    bool root_pack = fs::exists(utf8::PathFromUtf8(root) / "pack.json", ec);
    for (const auto& e : fs::directory_iterator(utf8::PathFromUtf8(root), fs::directory_options::skip_permission_denied, ec)) {
        std::error_code de;
        root_pack |= e.is_regular_file(de) && utf8::PathToUtf8(e.path().filename()).rfind("tex1_", 0) == 0;
    }
    if (root_pack) dirs.assign(1, utf8::PathFromUtf8(root));
    u64 total_files = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (const fs::path& dir : dirs) {
        std::string error;
        const std::string dir_text = utf8::PathToUtf8(dir);
        std::optional<hdpack::Pack> pack = hdpack::ScanPack(dir_text, &error);
        if (!pack) {
            MH4N_LOG(Warning, kLog, "texture pack {}: {}", dir_text, error);
            continue;
        }
        const int mode = pack->options.use_new_hash ? 1 : 0;
        u32 png = 0, other = 0, normal = 0, taken = 0, size_failed = 0;
        for (const auto& [hash, material] : pack->materials) {
            if (material.file[static_cast<int>(hdpack::MapType::Normal)] >= 0) ++normal;
            const int index = material.file[static_cast<int>(hdpack::MapType::Color)];
            if (index < 0) continue;
            const hdpack::PackFile& pf = pack->files[static_cast<std::size_t>(index)];
            if (pf.format != hdpack::FileFormat::Png) {
                ++other;
                continue;
            }
            ++png;
            if (by_hash_[mode].count(hash) != 0) continue;  // (an earlier pack has it: the first wins, as in a load order)
            File f;
            f.path = utf8::PathToUtf8(dir / utf8::PathFromUtf8(pf.path));
            f.flip = !pack->options.flip_png_files;
            if (!PngSize(f.path, f.width, f.height)) {
                ++size_failed;
                continue;
            }
            by_hash_[mode][hash] = static_cast<u32>(files_.size());
            files_.push_back(std::move(f));
            ++taken;
        }
        total_files += taken;
        MH4N_LOG(Info, kLog,
                 "texture pack {}: {} textures drawn from it ({} PNG colour maps; left out: {} DDS or KTX, {} normal maps, {} "
                 "unreadable PNG headers; {} names ignored, {} files without a hash); {} hash, files {}",
                 dir_text, taken, png, other, normal, size_failed, pack->stats.ignored_names, pack->stats.files_without_hash,
                 mode ? "the new (use_new_hash)" : "the old", pack->options.flip_png_files ? "flipped as Azahar does" : "as they are");
    }
    if (total_files) {
        MH4N_LOG(Info, kLog, "HD textures: {} in all, the packs read in {} ms; each file is read when the game first draws its "
                             "texture, and takes GPU memory as it is (4 bytes a pixel: a 1024x1024 texture 4 MB)", total_files,
                 std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    }
}

void HdTextures::CheckOrientation(const u8* data, std::size_t size, u32 width, u32 height, texture::Format format,
                                  const std::vector<u8>* rgba) {
    // (a decoded format: the hash's decode is RGBA8 too, its rows bottom-up)
    if (oriented_ || rgba == nullptr || texture::HashBytesPerTexel(format) != 4 || format == texture::Format::RGBA8) return;
    std::vector<u8> bottom_up;
    if (!texture::DecodeForHash(data, size, width, height, format, bottom_up) || bottom_up.size() != rgba->size()) return;
    oriented_ = true;
    const std::size_t row = static_cast<std::size_t>(width) * 4;
    bool upright = true;
    for (u32 y = 0; y < height && upright; ++y) {
        upright = std::memcmp(rgba->data() + y * row, bottom_up.data() + (height - 1 - y) * row, row) == 0;
    }
    MH4N_LOG(Info, kLog, "orientation check ({}x{} format {}): the decoded pixels are {}", width, height, static_cast<u32>(format),
             upright ? "the hash's rows turned over (upright, as Azahar's dumps)" : "NOT the hash's rows turned over");
}

const HdTextures::Replacement* HdTextures::Find(u64 content, const u8* data, std::size_t size, u32 width, u32 height,
                                                texture::Format format) {
    if (by_hash_[0].empty() && by_hash_[1].empty()) return nullptr;
    const u64 key = AnswerKey(content, width, height, format);
    if (auto it = answers_.find(key); it != answers_.end()) {
        return it->second.found && !files_[it->second.r.file].refused ? &it->second.r : nullptr;
    }
    Answer& a = answers_[key];
    ++lookups_;
    const std::optional<texture::CustomHashes> h = texture::ComputeCustomHashes(data, size, width, height, format);
    if (!h) return nullptr;
    const u64 hashes[2] = {h->old_hash, h->new_hash};
    for (int mode = 0; mode < 2 && !a.found; ++mode) {
        if (auto it = by_hash_[mode].find(hashes[mode]); it != by_hash_[mode].end()) {
            const File& f = files_[it->second];
            a.found = true;
            a.r = Replacement{it->second, f.width, f.height};
            if (++replaced_ <= 30 || replaced_ % 100 == 0) {
                MH4N_LOG(Info, kLog, "{} {}x{} format {} drawn from {} ({}x{}, {:.1f} MB; {} textures from packs so far)",
                         texture::CustomTextureStem(width, height, hashes[mode], format), width, height,
                         static_cast<u32>(format), f.path, f.width, f.height, f.width * f.height * 4 / 1048576.0, replaced_);
            }
        }
    }
    return a.found ? &a.r : nullptr;
}

bool HdTextures::Pixels(const Replacement& r, std::vector<u8>& rgba) {
    const File& f = files_[r.file];
    std::ifstream in(utf8::PathFromUtf8(f.path), std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamsize n = in.tellg();
    std::vector<u8> bytes(static_cast<std::size_t>(std::max<std::streamsize>(n, 0)));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), n)) return false;
    hdpack::Image image;
    std::string error;
    if (!hdpack::DecodePng(bytes.data(), bytes.size(), f.flip, image, error) || image.width != f.width || image.height != f.height) {
        MH4N_LOG(Warning, kLog, "{}: {}", f.path, error.empty() ? "its size changed" : error);
        return false;
    }
    rgba = std::move(image.levels[0]);
    return true;
}

void HdTextures::Refuse(const Replacement& r) {
    File& f = files_[r.file];
    if (f.refused) return;
    f.refused = true;
    MH4N_LOG(Warning, kLog, "{} ({}x{}): could not be read or uploaded; its texture is the game's own from now on", f.path,
             f.width, f.height);
}

void HdTextures::Dump(u64 content, const u8* data, std::size_t size, u32 width, u32 height, texture::Format format,
                      const std::vector<u8>& rgba) {
    CheckOrientation(data, size, width, height, format, &rgba);
    if (dump_dir_.empty()) return;
    const u64 key = AnswerKey(content, width, height, format);
    if (dumped_.count(key) != 0) return;
    dumped_[key] = true;
    const std::optional<texture::CustomHashes> h = texture::ComputeCustomHashes(data, size, width, height, format);
    if (!h || rgba.size() != static_cast<std::size_t>(width) * height * 4) return;
    const std::string path =
        utf8::PathToUtf8(utf8::PathFromUtf8(dump_dir_) / (texture::CustomTextureStem(width, height, h->old_hash, format) + ".png"));
    if (lodepng::encode(path, rgba, width, height) != 0) {
        MH4N_LOG(Warning, kLog, "MH4N_HD_DUMP: {} not written", path);
    }
}

}  // namespace mh4n::gpu::native
