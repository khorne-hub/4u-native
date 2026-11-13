// The shader cache cleared at the next start (cache_clear.cpp; the settings menu's Clear shader cache)
#pragma once
#include <cstdint>
#include <string>

void cache_clear_start(const std::string& user);  // android_main.cpp, before the runtime: clears it if asked, and logs
uint64_t cache_clear_bytes();                      // what the shader cache's files take now
bool cache_clear_pending();                        // the menu asked for it: the next start clears it
bool cache_clear_request(bool on);                 // ask (or take back) the clearing at the next start
