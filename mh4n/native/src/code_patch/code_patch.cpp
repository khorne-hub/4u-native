// Derived from Azahar src/core/file_sys/ncch_container.cpp and src/core/loader/ncch.cpp at
// 5f3306d13cab40bbd5cf38f2cf3ae8b6e6eedca6 (GPL-3.0-or-later: ncch_container.cpp says "GPLv3 or any later
// version"; ncch.cpp says "GPLv2 or any later version"): the mods folder's id (GetModId), the places and
// order of the code replacement (LoadOverrideExeFSSection), of the code patches (ApplyCodePatch) and of
// the exheader override, and the bss pages appended before the patch (ncch.cpp).
// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project (ncch_container.cpp)
// Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project (ncch.cpp)
#include "code_patch.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <initializer_list>
#include <fstream>
#include <iterator>

#include "layeredfs/patch.h"

namespace mh4n::code_patch {

namespace {

bool ReadWhole(const std::string& path, std::vector<u8>* out, bool* opened) {
    std::ifstream file(path, std::ios::binary);
    *opened = static_cast<bool>(file);
    if (!file) {
        return false;
    }
    out->assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !file.bad();
}

bool Exists(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return static_cast<bool>(file);
}

std::string Join(const std::string& dir, const std::string& rel) {
    if (dir.empty() || dir.back() == '/' || dir.back() == '\\') {
        return dir + rel;
    }
    return dir + "/" + rel;
}

}  // namespace

u64 ModId(u64 program_id) {
    constexpr u64 kUpdateMask = 0x0000000e'00000000ull;
    if ((program_id & 0x000000ff'00000000ull) == kUpdateMask) {
        return program_id & ~kUpdateMask;
    }
    return program_id;
}

std::string ModsPath(const std::string& load_dir, u64 program_id) {
    char id[17];
    std::snprintf(id, sizeof(id), "%016llX", static_cast<unsigned long long>(ModId(program_id)));
    return Join(load_dir, std::string("mods/") + id);
}

u32 BssPages(u32 bss_size) {
    return static_cast<u32>((static_cast<u64>(bss_size) + (kPageSize - 1)) / kPageSize);
}

ModFolder::ModFolder(std::string path) : path_(std::move(path)) {}

bool ModFolder::Replace(std::vector<u8>& code, std::string* applied) const {
    for (const char* rel : {"exefs/code.bin", "code.bin"}) {
        std::vector<u8> data;
        bool opened = false;
        if (ReadWhole(Join(path_, rel), &data, &opened)) {
            code = std::move(data);
            *applied = rel;
            return true;
        }
    }
    return false;
}

Outcome ModFolder::Patch(std::vector<u8>& code) const {
    Outcome outcome;
    struct Location {
        const char* rel;
        bool ips;
    };
    constexpr std::array<Location, 4> kPatches{{
        {"exefs/code.ips", true},
        {"exefs/code.bps", false},
        {"code.ips", true},
        {"code.bps", false},
    }};
    for (const Location& location : kPatches) {
        const std::string path = Join(path_, location.rel);
        if (!Exists(path)) {
            continue;
        }
        const std::string error = std::string("mods: ") + location.rel + ": patch error";
        std::vector<u8> patch;
        bool opened = false;
        if (!ReadWhole(path, &patch, &opened)) {
            return Outcome{false, error, "unreadable", outcome.applied};
        }
        using layeredfs::patch::Status;
        const Status status =
            location.ips ? layeredfs::patch::ApplyIps(patch, code) : layeredfs::patch::ApplyBps(patch, code);
        if (status != Status::Success) {
            return Outcome{false, error,
                           status == Status::ErrorPatchesInvalidTitle ? "source CRC-32 differs" : "malformed",
                           outcome.applied};
        }
        outcome.applied.push_back(location.rel);
        break;
    }
    return outcome;
}

Outcome ModFolder::ApplyToCode(std::vector<u8>& code, u32 bss_pages) const {
    Outcome outcome;

    // 1. A replacement of the code, before the bss pages.
    std::string replaced;
    if (Replace(code, &replaced)) {
        outcome.applied.push_back(replaced);
    }

    // 2. The bss pages.
    code.resize(code.size() + static_cast<std::size_t>(bss_pages) * kPageSize, 0);

    // 3. The first patch that exists.
    Outcome patched = Patch(code);
    patched.applied.insert(patched.applied.begin(), outcome.applied.begin(), outcome.applied.end());
    return patched;
}

std::optional<std::vector<u8>> ModFolder::ExheaderOverride() const {
    std::vector<u8> data;
    bool opened = false;
    if (!ReadWhole(Join(path_, "exheader.bin"), &data, &opened) || data.size() < kExheaderSize) {
        return std::nullopt;
    }
    data.resize(kExheaderSize);
    return data;
}

std::optional<std::vector<u8>> ExheaderOverrideOf(const std::vector<std::string>& folders, std::string* which) {
    for (std::size_t i = folders.size(); i-- > 0;) {
        if (auto exheader = ModFolder(folders[i]).ExheaderOverride()) {
            if (which) *which = folders[i];
            return exheader;
        }
    }
    return std::nullopt;
}

std::vector<std::pair<u32, u32>> IpsRanges(const std::vector<u8>& patch) {
    std::vector<std::pair<u32, u32>> out;
    static constexpr u8 kMagic[5] = {'P', 'A', 'T', 'C', 'H'};
    if (patch.size() < 8 || !std::equal(std::begin(kMagic), std::end(kMagic), patch.begin())) return out;
    const auto be = [&](std::size_t at, int n) {
        u32 v = 0;
        for (int i = 0; i < n; ++i) v = v << 8 | (at + i < patch.size() ? patch[at + i] : 0);
        return v;
    };
    std::size_t cursor = 5;
    const std::size_t end = patch.size() - 3;
    while (cursor < end && !(patch[cursor] == 'E' && patch[cursor + 1] == 'O' && patch[cursor + 2] == 'F')) {
        const u32 offset = be(cursor, 3), length = be(cursor + 3, 2);
        if (length == 0) {
            out.emplace_back(offset, offset + be(cursor + 5, 2));
            cursor += 8;
        } else {
            out.emplace_back(offset, offset + length);
            cursor += 5 + length;
        }
    }
    return out;
}

std::vector<CodeConflict> CodeConflicts(const std::vector<std::string>& folders) {
    using Kind = CodeConflict::Kind;
    std::vector<CodeConflict> out;
    const auto has_any = [](const std::string& folder, std::initializer_list<const char*> names) -> std::string {
        for (const char* rel : names) {
            if (Exists(Join(folder, rel))) return rel;
        }
        return {};
    };
    std::vector<std::size_t> replacing;
    for (std::size_t i = 0; i < folders.size(); ++i) {
        if (!has_any(folders[i], {"exefs/code.bin", "code.bin"}).empty()) replacing.push_back(i);
    }
    for (std::size_t k = 1; k < replacing.size(); ++k) out.push_back({Kind::Replaced, replacing[k - 1], replacing[k]});
    const bool replaced = !replacing.empty();
    const std::size_t last = replaced ? replacing.back() : 0;
    std::vector<std::pair<std::size_t, std::string>> patches;  // folder, its first patch
    for (std::size_t i = 0; i < folders.size(); ++i) {
        const std::string patch = has_any(folders[i], {"exefs/code.ips", "exefs/code.bps", "code.ips", "code.bps"});
        if (patch.empty()) continue;
        patches.emplace_back(i, patch);
        if (replaced && i != last) out.push_back({Kind::PatchOnReplacement, i, last});
    }
    for (std::size_t i = folders.size(); i-- > 0;) {
        if (Exists(Join(folders[i], "exheader.bin"))) {
            if (replaced && i != last) out.push_back({Kind::Exheader, i, last});
            break;
        }
    }
    std::vector<std::pair<std::size_t, std::vector<std::pair<u32, u32>>>> seen;  // (folder, ranges; empty for a BPS)
    std::vector<bool> is_bps;
    for (const auto& [i, rel] : patches) {
        const bool bps = rel.size() >= 4 && rel.compare(rel.size() - 4, 4, ".bps") == 0;
        if (bps) {
            if (!seen.empty()) out.push_back({Kind::Bps, i, seen.back().first});
            seen.push_back({i, {}});
            is_bps.push_back(true);
            continue;
        }
        std::vector<u8> data;
        bool opened = false;
        ReadWhole(Join(folders[i], rel), &data, &opened);
        std::vector<std::pair<u32, u32>> ranges = IpsRanges(data);
        for (std::size_t k = 0; k < seen.size(); ++k) {
            if (is_bps[k]) continue;
            bool overlap = false;
            for (const auto& [s, e] : ranges) {
                for (const auto& [os, oe] : seen[k].second) overlap = overlap || (s < oe && os < e);
            }
            if (overlap) out.push_back({Kind::Overlap, i, seen[k].first});
        }
        seen.push_back({i, std::move(ranges)});
        is_bps.push_back(false);
    }
    return out;
}

Outcome ApplyFolders(const std::vector<std::string>& folders, std::vector<u8>& code, u32 bss_pages) {
    const auto name = [](const std::string& folder) {
        std::string f = folder;
        while (!f.empty() && (f.back() == '/' || f.back() == '\\')) f.pop_back();
        const std::size_t cut = f.find_last_of("/\\");
        return cut == std::string::npos ? f : f.substr(cut + 1);
    };
    Outcome outcome;
    // 1. the replacement: the last folder's that has one (a later mod wins, as its files do)
    for (std::size_t i = folders.size(); i-- > 0;) {
        std::string replaced;
        if (ModFolder(folders[i]).Replace(code, &replaced)) {
            outcome.applied.push_back(name(folders[i]) + "/" + replaced);
            break;
        }
    }
    // 2. the bss pages
    code.resize(code.size() + static_cast<std::size_t>(bss_pages) * kPageSize, 0);
    // 3. each folder's first patch, in order
    for (const std::string& folder : folders) {
        Outcome patched = ModFolder(folder).Patch(code);
        for (const std::string& applied : patched.applied) {
            outcome.applied.push_back(name(folder) + "/" + applied);
        }
        if (!patched.ok) {
            const std::string prefix = "mods: ";
            std::string error = patched.error;
            if (error.compare(0, prefix.size(), prefix) == 0) {
                error = prefix + name(folder) + "/" + error.substr(prefix.size());
            }
            return Outcome{false, error, patched.detail, outcome.applied};
        }
    }
    return outcome;
}

}  // namespace mh4n::code_patch
