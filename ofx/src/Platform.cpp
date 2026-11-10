// Platform.cpp — see Platform.h.
#include "Platform.h"

#include <algorithm>
#include <cstdlib>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <dirent.h>
  #include <dlfcn.h>
  #include <sys/stat.h>
#endif

namespace lc {
namespace platform {

#ifdef _WIN32
const char kPathSep = '\\';

static std::wstring widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::string env(const wchar_t* name)
{
    const wchar_t* v = _wgetenv(name);
    return v ? narrow(v) : std::string();
}
#else
const char kPathSep = '/';

static std::string env(const char* name)
{
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}
#endif

static bool isSep(char c) { return c == '/' || c == '\\'; }

std::string join(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    if (isSep(a.back())) return a + b;
    return a + kPathSep + b;
}

// Strips the last path component ("a/b/c" -> "a/b"); empty when there is none
static std::string parent(const std::string& p)
{
    size_t i = p.size();
    while (i > 0 && !isSep(p[i - 1])) i--;
    if (i == 0) return std::string();
    while (i > 1 && isSep(p[i - 1])) i--;
    return p.substr(0, i);
}

static std::string modulePath()
{
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&modulePath), &module))
        return std::string();
    std::wstring buf(1024, L'\0');
    for (;;)
    {
        DWORD n = GetModuleFileNameW(module, &buf[0], (DWORD)buf.size());
        if (n == 0) return std::string();
        if (n < buf.size()) { buf.resize(n); break; }
        buf.resize(buf.size() * 2);
    }
    return narrow(buf);
#else
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(&modulePath), &info) && info.dli_fname) return info.dli_fname;
    return std::string();
#endif
}

std::string bundleResourcesDir()
{
    std::string arch = parent(modulePath());   // .../Contents/<MacOS|Win64|Linux-x86-64>
    std::string contents = parent(arch);
    return contents.empty() ? std::string() : join(contents, "Resources");
}

std::string userDataDir()
{
#if defined(_WIN32)
    std::string base = env(L"APPDATA");
    return base.empty() ? std::string() : join(base, "LensCharacter");
#elif defined(__APPLE__)
    return env("HOME") + "/Library/Application Support/LensCharacter";
#else
    std::string xdg = env("XDG_DATA_HOME");
    return (xdg.empty() ? env("HOME") + "/.local/share" : xdg) + "/LensCharacter";
#endif
}

std::string logFilePath()
{
#if defined(_WIN32)
    std::string base = env(L"LOCALAPPDATA");
    if (base.empty()) return std::string();
    std::string dir = join(base, "LensCharacter");
    makeDirs(dir);
    return join(dir, "LensCharacter.log");
#elif defined(__APPLE__)
    std::string home = env("HOME");
    return home.empty() ? std::string() : home + "/Library/Logs/LensCharacter.log";
#else
    std::string dir = userDataDir();
    makeDirs(dir);
    return dir + "/LensCharacter.log";
#endif
}

std::vector<std::string> listFiles(const std::string& dir, const std::string& ext)
{
    std::vector<std::string> out;
    auto matches = [&](const std::string& n) {
        return n.size() > ext.size() && n.compare(n.size() - ext.size(), ext.size(), ext) == 0;
    };
#ifdef _WIN32
    if (dir.empty()) return out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(widen(join(dir, "*")).c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::string n = narrow(fd.cFileName);
            if (matches(n)) out.push_back(join(dir, n));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
#else
    if (DIR* d = opendir(dir.c_str()))
    {
        while (dirent* e = readdir(d))
        {
            std::string n = e->d_name;
            if (matches(n)) out.push_back(join(dir, n));
        }
        closedir(d);
    }
#endif
    std::sort(out.begin(), out.end());
    return out;
}

static bool isDir(const std::string& path)
{
#ifdef _WIN32
    DWORD a = GetFileAttributesW(widen(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool makeDirs(const std::string& path)
{
    if (path.empty()) return false;
    if (isDir(path)) return true;
    std::string up = parent(path);
    if (!up.empty() && up != path && !isDir(up)) makeDirs(up);
#ifdef _WIN32
    CreateDirectoryW(widen(path).c_str(), nullptr);
#else
    mkdir(path.c_str(), 0755);
#endif
    return isDir(path);
}

FILE* openFile(const std::string& path, const char* mode)
{
#ifdef _WIN32
    return _wfopen(widen(path).c_str(), widen(mode).c_str());
#else
    return fopen(path.c_str(), mode);
#endif
}

bool readFile(const std::string& path, std::string& out)
{
    FILE* f = openFile(path, "rb");
    if (!f) return false;
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

bool writeFile(const std::string& path, const std::string& data)
{
    FILE* f = openFile(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = fclose(f) == 0 && ok;
    return ok;
}

} // namespace platform
} // namespace lc
