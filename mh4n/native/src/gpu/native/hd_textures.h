// HD texture packs (1.0's S26): the community's packs for the emulators (Azahar's and Citra's layout and hashes: a folder
// of tex1_<w>x<h>_<hash>_<format>.png files, with or without a pack.json) drawn in place of the game's own textures.
//
// The pack is read in place from the player's folder (the app passes it, MH4N_TEXTURE_PACKS; otherwise <--user>/load/
// textures/, Azahar's own place): each folder under it with texture files is one pack (load/textures/<title id>/ as Azahar
// has them, or the pack's own folder), read by Azahar's rules (hdpack/pack.h, ScanPack: the file names, pack.json's options
// and its "textures" map). A texture's custom hash is the one Azahar names its files by (gpu/texture/hash.h: the old hash
// of the decoded texture, or the new one of its bytes when the pack's pack.json says use_new_hash), taken once for each
// content the texture cache sees, and a texture the pack has gets an image of the pack file's size with its pixels.
//
// Only PNG colour maps are drawn (most packs are PNG; DDS and KTX files, normal maps and mip files are counted in the log
// and left out). A PNG is read and decoded when its texture is first drawn (the frame waits for it: a pack loads as the
// areas are visited) and its pixels go once the image holds them. The GPU memory the HD images take is the player's
// call: the log says it as it grows ("[mem] HD textures"), and the images count in the renderer's texture budget like
// the game's own, which lets go of the least used past it.
//
// Orientation: the texture cache's pixels (DecodeToRgba8) are the image upright, its top row first, as Azahar's dumps
// write their PNGs; flip_png_files (true without a pack.json) means the files are so, false that their rows are bottom-up.
// The first texture of a decoded format checks that against the hash's own decode (rows bottom-up) and logs it.
//
//   MH4N_TEXTURE_PACKS=<dir>   the folder of the packs (the app: the game folder's textures/)
//   MH4N_HD_TEXTURES=0         no pack is read
//   MH4N_HD_DUMP=<dir>         (debug) each texture the game draws, once, as <dir>/tex1_<w>x<h>_<old hash>_<format>.png,
//                              upright: a pack of the game's own textures, for tests
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "gpu/texture/formats.h"
#include "mh4n/types.h"

namespace mh4n::gpu::native {

class HdTextures {
public:
    struct Replacement {
        u32 file = 0;  // index into files_
        u32 width = 0, height = 0;
    };

    // Reads the packs under `root` (or none: an empty root, a folder that isn't there).
    void Load(const std::string& root);
    bool active() const { return !by_hash_[0].empty() || !by_hash_[1].empty() || !dump_dir_.empty(); }

    // The pack's texture for a texture whose content hash (the texture cache's) is `content` and whose memory is `data`;
    // nullptr when no pack has it. Answered once for each content.
    const Replacement* Find(u64 content, const u8* data, std::size_t size, u32 width, u32 height, texture::Format format);

    // Its pixels, RGBA8 upright as the texture cache's, read and decoded now. False when the file can't be read.
    bool Pixels(const Replacement& r, std::vector<u8>& rgba);
    // A file that could not be read or uploaded: its textures are the game's own from now on (logged once)
    void Refuse(const Replacement& r);

    // A texture drawn for the first time (MH4N_HD_DUMP): written as a PNG of the pack's names, with its decoded pixels.
    void Dump(u64 content, const u8* data, std::size_t size, u32 width, u32 height, texture::Format format,
              const std::vector<u8>& rgba);

    // The images made from the pack: for the log's [mem] line
    void Made(u64 bytes) { ++images_, image_bytes_ += bytes; }
    void Gone(u64 bytes) { --images_, image_bytes_ -= bytes; }
    u64 images() const { return images_; }
    u64 image_bytes() const { return image_bytes_; }
    u64 replaced() const { return replaced_; }

private:
    struct File {
        std::string path;
        bool flip = false;  // the rows are turned over as they are read (the pack's flip_png_files false)
        bool refused = false;  // (Refuse)
        u32 width = 0, height = 0;
    };
    struct Answer {
        bool found = false;
        Replacement r;
    };
    void CheckOrientation(const u8* data, std::size_t size, u32 width, u32 height, texture::Format format,
                          const std::vector<u8>* rgba);

    std::vector<File> files_;
    std::unordered_map<u64, u32> by_hash_[2];  // old hash, new hash -> file
    std::unordered_map<u64, Answer> answers_;  // by content hash (and size, format)
    std::string dump_dir_;
    std::unordered_map<u64, bool> dumped_;
    bool oriented_ = false;
    u64 images_ = 0, image_bytes_ = 0, replaced_ = 0, lookups_ = 0;
};

}  // namespace mh4n::gpu::native
