// The game's addresses in the port's native code (hooks.cpp, swkbd_bridge.cpp), USA v1.3.0's: every one is listed
// in config/port_addresses.txt. A code library made for another version of the game (EUR v1.1.0, or USA v1.0's code
// that Heavenfall's web installer builds on) carries each one's address in that version (tools/game_version.py);
// SetGameAddresses installs that table before the hooks, and each GameAddr then reads as the running version's
// address. Without a table (a USA v1.3.0 library, the Azahar build) the addresses are their own.
//
//   GameAddr SMAIN_INSTANCE{0x01057194};   // a USA v1.3.0 address, used as a u32: rd32(ctx, SMAIN_INSTANCE)
//   switch (UsaAddress(vtable)) { case 0x00e6c170: ... }   // back to USA v1.3.0's, for constant labels
#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace Port {

// (usa, here) pairs, sorted by usa; empty for USA v1.3.0
inline std::vector<std::pair<std::uint32_t, std::uint32_t>>& GameAddressTable() {
    static std::vector<std::pair<std::uint32_t, std::uint32_t>> table;
    return table;
}

// The running version's address of a USA v1.3.0 one: the table's, else the address itself.
inline std::uint32_t GameAddress(std::uint32_t usa) {
    const auto& table = GameAddressTable();
    const auto it = std::lower_bound(table.begin(), table.end(), std::make_pair(usa, std::uint32_t{0}));
    return it != table.end() && it->first == usa ? it->second : usa;
}

// The other way: the USA v1.3.0 address of one of the running version's, or the address itself.
inline std::uint32_t UsaAddress(std::uint32_t here) {
    for (const auto& [usa, h] : GameAddressTable()) {
        if (h == here) return usa;
    }
    return here;
}

class GameAddr {
public:
    // (namespace-scope objects of the port's files: each registers itself, so SetGameAddresses reaches it)
    GameAddr(std::uint32_t usa) : usa_(usa), here_(usa) { Registry().push_back(this); }
    GameAddr(const GameAddr&) = delete;
    GameAddr& operator=(const GameAddr&) = delete;
    operator std::uint32_t() const { return here_; }
    std::uint32_t Usa() const { return usa_; }
    // here = the table's address (SetGameAddresses); false when the table has none for it
    bool Translate() {
        here_ = GameAddress(usa_);
        const auto& table = GameAddressTable();
        return table.empty() || usa_ == 0 ||
               std::binary_search(table.begin(), table.end(), std::make_pair(usa_, here_));
    }
    static std::vector<GameAddr*>& Registry() {
        static std::vector<GameAddr*> all;
        return all;
    }

private:
    std::uint32_t usa_, here_;
};

// Installs the code library's table (rc_port_addr_usa / rc_port_addr_here, `count` pairs; 0 for USA v1.3.0) and
// translates every GameAddr. Returns the USA v1.3.0 addresses of the GameAddrs the table has none for (0 stays 0):
// a port address missing from config/port_addresses.txt, which keeps its USA v1.3.0 value.
inline std::vector<std::uint32_t> SetGameAddresses(const std::uint32_t* usa, const std::uint32_t* here,
                                                   std::uint32_t count) {
    auto& table = GameAddressTable();
    table.clear();
    for (std::uint32_t i = 0; i < count; ++i) table.emplace_back(usa[i], here[i]);
    std::sort(table.begin(), table.end());
    std::vector<std::uint32_t> missing;
    for (GameAddr* a : GameAddr::Registry()) {
        if (!a->Translate()) missing.push_back(a->Usa());
    }
    return missing;
}

}  // namespace Port
