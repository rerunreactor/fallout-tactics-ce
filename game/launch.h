// launch.h -- starting fotgame with no command line: a double-click on
// Windows or Linux, or the Mac app opened from Finder.
//
// With any argument, fotgame behaves exactly as before. With none, it finds
// the game, starts the campaign, keeps its saves apart from the original's,
// and reports problems in a dialog rather than on a console nobody can see.

#pragma once

#include <string>

namespace game {

struct PlayerSetup {
    std::string core;     // the game's core folder, holding the .bos archives
    std::string saveDir;  // our saves, apart from the original's core/user/save
    std::string logPath;  // where stdout and stderr go when there's no terminal
};

enum class PickResult { Picked, Cancelled, Unavailable };

// True when there is nothing on the command line. Older macOS versions add a
// -psn_ argument when Finder opens an app; that counts as nothing too.
bool IsPlayerLaunch(int argc, char** argv);

// Finds the game: next to the program (or next to the Mac app), in the folder
// the player chose last time, or in the current directory. Failing that, it
// asks for the folder and remembers the answer. Returns false if the player
// gave up; any reason has already been shown.
bool ResolvePlayerSetup(PlayerSetup* out);

// When stderr isn't a terminal, empties `path` and sends stdout and stderr
// there, so there is something to read after a failed run.
void RedirectOutputToLog(const std::string& path);

// The last `lines` lines of the log, for the error dialog.
std::string LogTail(const std::string& path, int lines);

// An error dialog, with the same text on stderr.
void ShowLaunchError(const std::string& message);

// Windows builds fotgame as a GUI program, which starts without a console.
// When it was started from one and its output isn't already redirected, this
// reattaches it, so --help and the logs still print there.
void AttachParentConsole();

// The platform's folder picker: IFileOpenDialog on Windows, NSOpenPanel on
// macOS (folder_picker_mac.mm), zenity or kdialog on Linux.
PickResult PickFolder(const std::string& title, std::string* out);

}  // namespace game
