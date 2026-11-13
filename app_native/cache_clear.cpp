// The shader cache, cleared at the player's request (the menu's Clear shader cache, 1.0's S30; MHGU for Android's
// runtime/cache_clear.c does the same): of mh4n's pipeline cache in <user>/cache/pipelines (caching_provider.h), the
// compiled shaders (spirv.bin) and the GPU driver's own cache (driver*.bin), what a driver update or a crash can leave
// wrong. The list of the pipelines earlier sessions drew (keys.bin) and their vertex sources (vertex.bin) stay: the next
// start compiles all of them again from it, with the plate "Building shaders" saying how far (settings_panel.cpp
// shader_plate), so the game doesn't meet them one by one. The runtime has the files open while it runs: the menu only
// leaves a mark (<user>/cache/clear_at_start), and the next start deletes them before the runtime starts
// (android_main.cpp).
#include "cache_clear.h"

#include <cstdio>
#include <filesystem>
#include <system_error>

namespace {
std::string g_user;  // (cache_clear_start's)

std::filesystem::path dir() { return std::filesystem::path(g_user) / "cache" / "pipelines"; }
std::filesystem::path mark() { return std::filesystem::path(g_user) / "cache" / "clear_at_start"; }

// The folder's files: their bytes, deleted when `del` (n: how many went)
uint64_t files(bool del, int* n) {
    uint64_t bytes = 0;
    std::error_code ec;
    if (g_user.empty() || !std::filesystem::is_directory(dir(), ec)) return 0;
    for (const auto& e : std::filesystem::directory_iterator(dir(), ec)) {
        std::error_code fe;
        if (!e.is_regular_file(fe)) continue;
        const std::string name = e.path().filename().string();
        if (name != "spirv.bin" && name.rfind("driver", 0) != 0) continue;  // (the list and its vertex sources stay)
        bytes += e.file_size(fe);
        if (del && std::filesystem::remove(e.path(), fe) && n) ++*n;
    }
    return bytes;
}
}  // namespace

void cache_clear_start(const std::string& user) {
    g_user = user;
    if (!cache_clear_pending()) return;
    int n = 0;
    const uint64_t bytes = files(true, &n);
    std::error_code ec;
    std::filesystem::remove(mark(), ec);
    std::fprintf(stderr, "[Info] App: the shader cache cleared, as asked in the menu: %d files (the SPIR-V and the driver's "
                         "cache), %.1f MB; every pipeline of the list is compiled again this time\n", n, bytes / 1048576.0);
}

uint64_t cache_clear_bytes() { return files(false, nullptr); }

bool cache_clear_pending() {
    std::error_code ec;
    return !g_user.empty() && std::filesystem::exists(mark(), ec);
}

bool cache_clear_request(bool on) {
    std::error_code ec;
    if (!on) return std::filesystem::remove(mark(), ec) || !cache_clear_pending();
    std::filesystem::create_directories(mark().parent_path(), ec);
    FILE* f = std::fopen(mark().string().c_str(), "w");
    if (!f) return false;
    std::fputs("the shader cache is deleted at the next start (the settings menu's Clear shader cache)\n", f);
    std::fclose(f);
    std::fprintf(stderr, "[Info] App: the shader cache will be cleared at the next start (%.1f MB now)\n",
                 cache_clear_bytes() / 1048576.0);
    return true;
}
