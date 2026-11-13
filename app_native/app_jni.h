// Calls into the app's Java side (android_main.cpp), from the SDL thread: the interface's thread.
#pragma once

// A static void method of GameActivity without arguments: "showSaveImport", "showSaveExport", "showGameFolder",
// "showShareLog", "showLicenses", "showBuilder", "showDriverImport", "showCheatImport", "showModImport",
// "showModFolderImport", "showExtraImport", "showShortcut", "restartApp".
void app_call_activity(const char* method);
