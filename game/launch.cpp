// launch.cpp -- starting fotgame with no command line. See launch.h.

#include "launch.h"

#include <SDL.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#else
#include <unistd.h>
#endif

namespace game {
namespace {

namespace fs = std::filesystem;

const char* const kTitle = "Fallout Tactics CE";

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

#ifdef _WIN32
std::wstring Widen(const std::string& s, UINT codePage) {
    const int n = MultiByteToWideChar(codePage, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(codePage, 0, s.c_str(), -1, &w[0], n);
    w.resize(static_cast<size_t>(n - 1));
    return w;
}

std::string Narrow(const std::wstring& w, UINT codePage) {
    const int n = WideCharToMultiByte(codePage, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(codePage, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    s.resize(static_cast<size_t>(n - 1));
    return s;
}
#endif

// SDL and the dialogs speak UTF-8. The engine opens files with narrow fopen,
// which on Windows reads the ANSI code page, so paths are kept in that.
std::string NativeFromUtf8(const std::string& utf8) {
#ifdef _WIN32
    return Narrow(Widen(utf8, CP_UTF8), CP_ACP);
#else
    return utf8;
#endif
}

std::string Utf8FromNative(const std::string& native) {
#ifdef _WIN32
    return Narrow(Widen(native, CP_ACP), CP_UTF8);
#else
    return native;
#endif
}

// Takes an SDL-allocated UTF-8 path, frees it, and drops the trailing slash.
std::string SdlPath(char* p) {
    if (p == nullptr) return std::string();
    std::string s = NativeFromUtf8(p);
    SDL_free(p);
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}

bool HasArchives(const fs::path& dir) {
    try {
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code fec;
            if (it->is_regular_file(fec) && Lower(it->path().extension().string()) == ".bos") {
                return true;
            }
        }
    } catch (const std::exception&) {
        // A name the narrow code page can't hold. Not the game's folder.
    }
    return false;
}

// `dir` is the core folder itself, or the game folder that holds it.
bool FindCore(const std::string& dir, std::string* core) {
    if (dir.empty()) return false;
    try {
        const fs::path p(dir);
        if (HasArchives(p)) {
            *core = p.string();
            return true;
        }
        std::error_code ec;
        for (fs::directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code fec;
            if (it->is_directory(fec) && Lower(it->path().filename().string()) == "core" &&
                HasArchives(it->path())) {
                *core = it->path().string();
                return true;
            }
        }
    } catch (const std::exception&) {
    }
    return false;
}

// A Mac app's SDL_GetBasePath() is its Contents/Resources folder. This is the
// folder the app itself sits in, or "" for anything that isn't an app.
std::string AppParent(const std::string& base) {
    static const char kTail[] = ".app/Contents/Resources";
    const size_t n = sizeof kTail - 1;
    if (base.size() < n || base.compare(base.size() - n, n, kTail) != 0) return std::string();
    return fs::path(base).parent_path().parent_path().parent_path().string();
}

std::string ReadFirstLine(const std::string& path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    return line;
}

// Asks whether to choose the game's folder now. False means quit, or that no
// dialog could be shown (the text then goes to stderr).
bool AskToChoose(const std::string& text) {
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit"},
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Choose folder\xE2\x80\xA6"},
    };
    SDL_MessageBoxData data;
    std::memset(&data, 0, sizeof data);
    data.flags = SDL_MESSAGEBOX_INFORMATION;
    data.title = kTitle;
    data.message = text.c_str();
    data.numbuttons = static_cast<int>(SDL_arraysize(buttons));
    data.buttons = buttons;
    int hit = 0;
    if (SDL_ShowMessageBox(&data, &hit) != 0) {
        std::fprintf(stderr, "%s\n", text.c_str());
        return false;
    }
    return hit == 1;
}

bool StderrIsTerminal() {
#ifdef _WIN32
    return GetConsoleWindow() != nullptr;
#else
    return isatty(STDERR_FILENO) != 0;
#endif
}

}  // namespace

bool IsPlayerLaunch(int argc, char** argv) {
#ifdef __EMSCRIPTEN__
    // The browser shell always passes the options it wants.
    (void)argc;
    (void)argv;
    return false;
#else
    if (argc <= 1) return true;
    return argc == 2 && std::strncmp(argv[1], "-psn_", 5) == 0;
#endif
}

bool ResolvePlayerSetup(PlayerSetup* out) {
    const std::string pref = SdlPath(SDL_GetPrefPath("RerunReactor", "Fallout Tactics CE"));
    const std::string base = SdlPath(SDL_GetBasePath());
    const std::string remembered =
        pref.empty() ? std::string() : (fs::path(pref) / "game-folder.txt").string();

    // Where to look, in order: next to the program; next to the Mac app; the
    // folder chosen last time; the current directory.
    const std::string places[] = {
        base,
        AppParent(base),
        remembered.empty() ? std::string() : ReadFirstLine(remembered),
        ".",
    };
    std::string core;
    bool found = false;
    for (const std::string& place : places) {
        if (FindCore(place, &core)) {
            found = true;
            break;
        }
    }

    if (!found) {
#ifdef __APPLE__
        // NSOpenPanel needs the application object SDL's video layer sets up.
        SDL_InitSubSystem(SDL_INIT_VIDEO);
#endif
        std::string text =
            "Fallout Tactics CE plays your own copy of Fallout Tactics: Brotherhood of "
            "Steel 1.27.\n\nChoose the game's folder: the one with the core folder in it.";
        for (;;) {
            if (!AskToChoose(text)) return false;
            std::string picked;
            const PickResult r = PickFolder("Choose your Fallout Tactics folder", &picked);
            if (r == PickResult::Unavailable) {
                ShowLaunchError(
                    "There's no folder picker on this system.\n\n"
                    "Put fotgame next to your Fallout Tactics core folder, or start it with\n"
                    "fotgame --campaign --assets <path to core>");
                return false;
            }
            if (r == PickResult::Cancelled) continue;
            if (FindCore(picked, &core)) break;
            text = "That folder doesn't hold Fallout Tactics' .bos archives:\n" +
                   Utf8FromNative(picked) +
                   "\n\nChoose the Fallout Tactics folder, or the core folder inside it.";
        }
        if (!remembered.empty()) {
            std::ofstream rem(remembered, std::ios::trunc);
            rem << core << '\n';
        }
    }

    std::error_code ec;
    const fs::path abs = fs::absolute(core, ec);
    if (!ec) core = abs.string();

    // Saves and the log live in the per-user folder SDL picks for the platform.
    // Without one, they go under the game's user folder, still apart from the
    // original's saves.
    const fs::path data =
        pref.empty() ? fs::path(core) / "user" / "fallout-tactics-ce" : fs::path(pref);
    out->core = core;
    out->saveDir = (data / "saves").string();
    out->logPath = (data / "fotgame.log").string();
    return true;
}

void RedirectOutputToLog(const std::string& path) {
    if (StderrIsTerminal()) return;
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    if (std::FILE* f = std::fopen(path.c_str(), "w")) std::fclose(f);
    // Both in append mode, so neither stream writes over the other.
    if (std::freopen(path.c_str(), "a", stdout) == nullptr) return;
    if (std::freopen(path.c_str(), "a", stderr) == nullptr) return;
}

std::string LogTail(const std::string& path, int lines) {
    std::ifstream in(path, std::ios::binary);
    std::deque<std::string> tail;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        tail.push_back(line);
        if (tail.size() > static_cast<size_t>(lines)) tail.pop_front();
    }
    std::string text;
    for (const std::string& l : tail) text += l + "\n";
    return Utf8FromNative(text);
}

void ShowLaunchError(const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, kTitle, message.c_str(), nullptr);
}

void AttachParentConsole() {
#ifdef _WIN32
    const HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h != nullptr && h != INVALID_HANDLE_VALUE) return;  // already redirected
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;      // double-clicked
    if (std::freopen("CONOUT$", "w", stdout) == nullptr) return;
    if (std::freopen("CONOUT$", "w", stderr) == nullptr) return;
#endif
}

#if defined(_WIN32)

PickResult PickFolder(const std::string& title, std::string* out) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    PickResult result = PickResult::Unavailable;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IFileOpenDialog, reinterpret_cast<void**>(&dialog)))) {
        DWORD options = 0;
        if (SUCCEEDED(dialog->GetOptions(&options))) {
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                               FOS_PATHMUSTEXIST);
        }
        const std::wstring wtitle = Widen(title, CP_UTF8);
        dialog->SetTitle(wtitle.c_str());
        result = PickResult::Cancelled;
        if (SUCCEEDED(dialog->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    *out = Narrow(path, CP_ACP);
                    if (!out->empty()) result = PickResult::Picked;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return result;
}

#elif defined(__EMSCRIPTEN__)

PickResult PickFolder(const std::string&, std::string*) {
    return PickResult::Unavailable;
}

#elif !defined(__APPLE__)  // macOS: folder_picker_mac.mm

namespace {

std::string ShellQuote(const std::string& s) {
    std::string q = "'";
    for (char c : s) {
        if (c == '\'') {
            q += "'\\''";
        } else {
            q += c;
        }
    }
    return q + "'";
}

bool HaveCommand(const char* name) {
    const std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

}  // namespace

PickResult PickFolder(const std::string& title, std::string* out) {
    std::string cmd;
    if (HaveCommand("zenity")) {
        cmd = "zenity --file-selection --directory --title=" + ShellQuote(title) + " 2>/dev/null";
    } else if (HaveCommand("kdialog")) {
        cmd = "kdialog --getexistingdirectory \"$HOME\" --title " + ShellQuote(title) +
              " 2>/dev/null";
    } else {
        return PickResult::Unavailable;
    }
    std::FILE* p = popen(cmd.c_str(), "r");
    if (p == nullptr) return PickResult::Unavailable;
    std::string path;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, p) != nullptr) path += buf;
    const int status = pclose(p);
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
    if (status != 0 || path.empty()) return PickResult::Cancelled;
    *out = path;
    return PickResult::Picked;
}

#endif

}  // namespace game
