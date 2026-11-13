// Cheats and mods for the settings' Cheats and Mods pages (settings_panel.cpp), over the runtime's cheat engine
// (mh4n native/src/cheats) and mod list (native/src/mods). As the MHGU port's pages, adapted to the 3DS:
//   - cheats: Gateway codes in Azahar's format, in <user>/cheats/0004000000126300.txt. ImportActivity.java copies the
//     .txt the player picks to <user>/cheats/import.txt; content_cheats_import_result merges it into the engine's list
//     (every cheat it brings off) and saves the file. Turning one on or off applies at the game's next frame.
//   - mods: folders under <user>/load/mods (each with romfs/ and/or exefs/), ordered and turned on or off in
//     load/mods/load_order.txt; the game reads them at its start. ImportActivity.java unpacks a .zip or copies a folder
//     there. A code mod (exefs/code.ips) needs the code library made with it on the PC: content_code_check tells whether
//     the library the game runs takes the code with the mods of a list.
// The mods beside the game in the game folder (its mods/) are copied in by GameFolderActivity.syncMods at each start.
#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "mods/mod_list.h"

// android_main.cpp: the user folder (before the game starts).
void content_init(const std::string& user_dir);
// The app's own cheats (the APK's assets/cheats/comer_rapido.txt: faster eating and drinking, as MHGU's) added to the
// cheats file, off; once per user folder (cheats/app_cheats_added.txt), so the ones a player removes don't come back. `text` is the
// asset's: each [Name] block the file doesn't have yet is added. Before the game starts (its cheats load then).
void content_add_port_cheats(const std::string& text);
const std::string& content_user_dir();
// The game's title, whose cheats file and mods these are: the USA game's unless the game folder says otherwise.
constexpr unsigned long long kContentTitle = 0x0004000000126300ull;
// S51: the game's title from a game folder's exheader.bin (its program id; an update's counts as the base game's): USA
// 0004000000126300, EUR 0004000000126100; kContentTitle when it can't be read. The runtime names the mods folder and
// the cheats file by it, so the Mods and Cheats pages must too.
unsigned long long content_title_of(const std::string& game_folder);
// android_main.cpp: the game folder of this run, before the pages are used; content_title() is then its title.
void content_set_game(const std::string& game_folder);
unsigned long long content_title();

// ---- cheats ---------------------------------------------------------------------------------------------------
struct ContentCheat {
    std::string name, comments, code;
    bool on = false;
    bool touches_code = false;  // it writes into the game's code: no effect in this port
    size_t invalid_lines = 0;
};
// The cheats as the engine has them now (empty before the game has started). `ready`: the engine exists.
std::vector<ContentCheat> content_cheats(bool* ready = nullptr);
// The states of the list (in its order) to the engine (at the next frame) and to the file. False when it can't be saved.
bool content_cheats_apply(const std::vector<char>& on, std::string* error);
bool content_cheat_remove(size_t index);
// Import cheats: the phone's file picker (ImportActivity); its outcome comes once, through content_cheats_import_result.
void content_cheats_import();
// A cheat that writes into the game's code (touches_code), which this port runs recompiled: its writes made a code mod,
// load/mods/<name>/exefs/code.ips ("Cheat - <its name>"), off in the load order until the code library is made with it
// on (then the cheat's change is in the game's code). The same cheat again replaces its mod, keeping its place and
// state. False and *error: CheatEngine::CodePatch's reasons (conditional, loaded_offset, register, none,
// no_code_range, ips_eof), not_started, unwritable.
bool content_cheat_to_code_mod(size_t index, std::string* name, std::string* error);
struct ContentImport {
    bool done = false, ok = false;
    std::string name;    // the file or the mod imported
    int count = 0;       // cheats: how many it brought
    bool code = false;   // a mod: it changes the game's code
    std::string error;   // unreadable, no_cheats, no_parts, not_this_game, exists, no_space...
    std::string detail;  //   and the system's words, if any
};
ContentImport content_cheats_import_result();

// ---- mods -----------------------------------------------------------------------------------------------------
std::vector<mh4n::mods::Mod> content_mods();
bool content_mods_save(const std::vector<mh4n::mods::Mod>& list, std::string* error);
// The mods the app found in the game folder's mods only by 1.0's search (further down, the game's own folders) and added
// off (load/mods/.found_off, GameFolderActivity.syncMods): the Mods page says so until its list is saved
std::vector<std::string> content_mods_found_off();
// Deletes a mod's folder. False for one copied from the game folder (it comes back at the next start) or on an error.
bool content_mod_remove(const std::string& name);
// Add a mod: the picker for its .zip (folder false) or its folder (true).
void content_mods_import(bool folder);
ContentImport content_mods_import_result();

// What the mods change in common, as MHGU's mod scan (the Mods page's conflict check): each mod's game files (romfs/ as it
// lies, romfs_ext/ by the file a patch is for) and each pair of mods that change some of the same ones. Read on a thread
// of its own: `done` once `files` and `conflicts` are there (the shared result is not touched again after that).
struct ContentModScan {
    std::vector<std::string> names;  // the mods compared, in the list's order (Pair's indexes)
    std::vector<int> files;          // how many game files each one changes
    struct Pair {
        int a = 0, b = 0;  // a < b
        int files = 0;     // the files both change: where both are on, the one later in the load order is used
        std::string example;
    };
    std::vector<Pair> conflicts;
    std::atomic<bool> done{false};
};
std::shared_ptr<ContentModScan> content_mods_scan(const std::vector<mh4n::mods::Mod>& list);

// The code mods of `list` that are on and don't go together (mh4n mods::CodeConflicts: Heavenfall's code.bin with another
// mod's code.ips, two code replacements, an exheader.bin with another mod's code, two patches over the same bytes). Read
// from the mods' files (their names and small patches, not the code): kept for the same set of code mods and files.
std::vector<mh4n::mods::CodeConflict> content_code_conflicts(const std::vector<mh4n::mods::Mod>& list);

// Would the code library the game runs take the game's code with the code mods of `list` that are on? Worked out on a
// thread of its own: `busy` until then; the answer is kept for the same set of code mods.
struct ContentCodeCheck {
    bool busy = false, known = false, matches = false;
    std::string error;
};
ContentCodeCheck content_code_check(const std::vector<mh4n::mods::Mod>& list);
