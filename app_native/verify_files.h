// Verify game files (System > Your game, 1.0; the MHGU port's same button, words and view): the player's game and its
// update read whole on a thread of their own and checked against what they must be, and a plain report of what is wrong
// and what to do. Nothing is written, changed or downloaded; nothing of the game ships with the app, only hashes:
//   - the version: the SHA-1 of the update's code (decompressed), as mh4n's tools/game_version.py tells it
//     (config/versions.json), its CIA title version, the game's and the update's region
//   - whole and unchanged: each file's length against its NCCH, the exheader's hash, the ExeFS's superblock and each of
//     its sections' SHA-256, the RomFS's superblock and its whole IVFC hash tree (every 4 KB block of the RomFS read and
//     hashed); for the USA game and update, their superblocks against the original dumps' (a copy rebuilt with hashes of
//     its own: a modified repack)
//   - the app's copy of the update's code (files/game: Dump.java) and the files it was made from (source.txt)
//   - the code library: an arm64 ELF, the id inside it, the .id beside it and the app's wanted id, the same build; the
//     game's code with the code mods that are on against the library the game runs (mh4n mods::CheckCode)
//   - the save and its extra data (guild cards, event quests, the Palico), the code mods that clash, the mods on
// The report goes to the log too ("[verify] ..." lines, every file named), for Share log.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct VerifyPaths {
    std::string ext;        // the app's own folder: game_folder.txt (the player's game folder)
    std::string game;       // the game folder the app prepared (exheader.bin, code.bin, source.txt); "" none
    std::string code;       // the code library the game runs (the app's internal copy); "" none
    std::string user;       // the user folder (load/mods)
    std::string saves;      // the folder that holds the save's sdmc/ (the user folder, or the save folder)
    std::string wanted_id;  // the code library id this app runs with (code_library_id.txt); "" no check
};

struct VerifyLine {
    enum Kind { Ok, Problem, Info, File, Todo } kind;  // File: an example under the finding before it
    std::string en, es;
};

struct VerifyState {
    enum Stage { Idle, Listing, Reading, Done, Cancelled } stage = Idle;
    uint64_t done = 0, total = 0;   // the bytes read whole (the RomFS's), of how many
    double reading_s = 0;           // seconds since the reading began
    int problems = 0;
    std::vector<VerifyLine> lines;  // (Done, Cancelled) the findings, problems first; the advice last, as Todo
};

// android_main.cpp, before the menu can be opened
void verify_files_init(const VerifyPaths& paths);
// Starts a check (a running one goes on), on a thread of its own
void verify_files_start();
// Stops the running check within a moment: its report says what was read
void verify_files_cancel();
// A copy of the state, for the menu's frame
VerifyState verify_files_state();
// (the PC test driver) the whole check on this thread
void verify_files_run_here();
