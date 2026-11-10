// Platform.h — the few things that differ between macOS, Windows and Linux: where files live,
// listing and creating folders, and opening files by UTF-8 path.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace lc {
namespace platform {

extern const char kPathSep;   // '/' or '\\', for paths shown to the user

std::string join(const std::string& a, const std::string& b);

// The bundle's Contents/Resources folder, found from this module's own location:
//   macOS   LensCharacter.ofx.bundle/Contents/MacOS/LensCharacter.ofx
//   Windows LensCharacter.ofx.bundle\Contents\Win64\LensCharacter.ofx
//   Linux   LensCharacter.ofx.bundle/Contents/Linux-x86-64/LensCharacter.ofx
// Empty if it cannot be determined.
std::string bundleResourcesDir();

// Per-user data folder for LensCharacter (not created here):
//   macOS   ~/Library/Application Support/LensCharacter
//   Windows %APPDATA%\LensCharacter
//   Linux   $XDG_DATA_HOME/LensCharacter or ~/.local/share/LensCharacter
std::string userDataDir();

// Where the plugin appends its diagnostics. Creates the parent folder if needed.
//   macOS   ~/Library/Logs/LensCharacter.log
//   Windows %LOCALAPPDATA%\LensCharacter\LensCharacter.log
//   Linux   <userDataDir>/LensCharacter.log
std::string logFilePath();

// Full paths of the files in `dir` ending in `ext` (e.g. ".json"), sorted. Empty if none.
std::vector<std::string> listFiles(const std::string& dir, const std::string& ext);

// Creates `path` and any missing parents. Returns true if it exists afterwards.
bool makeDirs(const std::string& path);

// fopen with a UTF-8 path (on Windows the narrow fopen would read it in the ANSI code page).
FILE* openFile(const std::string& path, const char* mode);

bool readFile(const std::string& path, std::string& out);
bool writeFile(const std::string& path, const std::string& data);

} // namespace platform
} // namespace lc
