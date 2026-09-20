#include "logger.h"

#include <KnownFolders.h>
#include <ShlObj.h>

#include <cstdio>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

// Logger. Log file lives in %LOCALAPPDATA%\MELEVR (2026-07-16: moved OUT of the game's Win64 folder at
// By request - a MELEVR_Log.txt sitting next to the exe looks unpolished and exposes
// internals). AppData\Local is hidden by default in Explorer, writable without admin, OUTSIDE the game
// tree (so a Steam verify can't wipe it and it never clutters the game folder). Falls back to the
// dll's own folder if LOCALAPPDATA is somehow unset. (Earlier history: started in Documents\MELEVR, moved
// next to the dll 2026-07-11, now hidden in AppData.) "FlatMono.log" was a long-obsolete fossil name.

namespace
{
constexpr wchar_t kLogFileName[] = L"MELEVR_Log.txt";

std::mutex& LogMutex()
{
    static std::mutex mutex;
    return mutex;
}

void DebugLine(const std::string& line) noexcept
{
    try
    {
        OutputDebugStringA(("[MELEVR] " + line + "\r\n").c_str());
    }
    catch (...)
    {
        OutputDebugStringA("[MELEVR] Failed while writing debug output.\r\n");
    }
}

std::string Timestamp()
{
    SYSTEMTIME time = {};
    GetLocalTime(&time);

    char buffer[64] = {};
    sprintf_s(
        buffer,
        "%04u-%02u-%02u %02u:%02u:%02u.%03u",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond,
        time.wMilliseconds);

    return buffer;
}

std::wstring JoinPath(const std::wstring& left, const wchar_t* right)
{
    if (left.empty())
    {
        return right;
    }

    std::wstring result = left;
    if (result.back() != L'\\' && result.back() != L'/')
    {
        result += L'\\';
    }

    result += right;
    return result;
}

std::wstring GetEnvironmentPath(const wchar_t* variableName)
{
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(variableName, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= static_cast<DWORD>(std::size(buffer)))
    {
        return {};
    }

    return std::wstring(buffer, length);
}

// The folder THIS dll (dxgi.dll) lives in - the game's Win64 folder, next to MELEVR.ini. Same
// GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS trick vr_config.cpp uses to find MELEVR.ini; kept independent
// (not shared code) since logger.cpp has to work standalone before config exists.
std::wstring GetOwnModuleFolderPath()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&GetOwnModuleFolderPath), &self);
    wchar_t path[MAX_PATH] = {0};
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0) return L".";
    std::wstring p(path, n);
    const size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p.resize(slash);   // strip the filename, no trailing slash (JoinPath adds one)
    return p;
}

std::wstring GetLogFolderPath()
{
    // %LOCALAPPDATA%\MELEVR - hidden from users, admin-free, survives a Steam verify. Self-contained
    // (inline CreateDirectory, no dependency on EnsureDirectoryExists which is defined below). Any failure
    // to resolve or create it falls back to the dll's own folder = the pre-2026-07-16 behavior.
    wchar_t local[MAX_PATH] = {0};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
    {
        std::wstring folder = std::wstring(local, n) + L"\\MELEVR";
        CreateDirectoryW(folder.c_str(), nullptr);   // harmless if it already exists
        const DWORD attr = GetFileAttributesW(folder.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0)
            return folder;
    }
    return GetOwnModuleFolderPath();   // fallback: next to the dll (original location)
}

std::wstring GetLogFilePath()
{
    return JoinPath(GetLogFolderPath(), kLogFileName);
}

bool EnsureDirectoryExists(const std::wstring& path)
{
    if (CreateDirectoryW(path.c_str(), nullptr))
    {
        return true;
    }

    const DWORD error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS)
    {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    DebugLine("CreateDirectoryW failed for log folder. Win32 error: " + std::to_string(error));
    return false;
}

std::wstring GetModulePath(HMODULE module)
{
    std::vector<wchar_t> buffer(MAX_PATH);

    for (;;)
    {
        const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
        {
            return L"<unknown>";
        }

        if (static_cast<size_t>(length) < buffer.size() - 1)
        {
            return std::wstring(buffer.data(), length);
        }

        buffer.resize(buffer.size() * 2);
    }
}

std::string GetProcessName()
{
    const std::wstring processPath = GetModulePath(nullptr);
    const size_t slash = processPath.find_last_of(L"\\/");
    const std::wstring processName = slash == std::wstring::npos ? processPath : processPath.substr(slash + 1);
    return MELEVR::Logger::WideToUtf8(processName);
}

std::string TrimTrailingWhitespace(std::string value)
{
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ' || value.back() == '\t'))
    {
        value.pop_back();
    }

    return value;
}

std::string FormatWindowsError(DWORD errorCode)
{
    char* messageBuffer = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&messageBuffer),
        0,
        nullptr);

    if (length == 0 || messageBuffer == nullptr)
    {
        return "Win32 error: " + std::to_string(errorCode);
    }

    std::string message(messageBuffer, length);
    LocalFree(messageBuffer);

    return "Win32 error " + std::to_string(errorCode) + ": " + TrimTrailingWhitespace(message);
}

bool AppendLineToFile(const std::wstring& filePath, const std::string& line)
{
    // Keep the log handle OPEN across calls. Opening + closing the file every line is a syscall storm that hitches
    // the frame whenever a log line is written during combat (mode transitions). The OS owns the bytes the moment WriteFile returns,
    // so the line survives a crash of this process without a per-line flush - the crash logger uses its own handle.
    // Guarded by the caller's LogMutex, so the static handle is single-threaded. Path is fixed, so open-once is safe.
    static HANDLE s_file = INVALID_HANDLE_VALUE;
    if (s_file == INVALID_HANDLE_VALUE)
    {
        s_file = CreateFileW(
            filePath.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (s_file == INVALID_HANDLE_VALUE)
        {
            DebugLine("CreateFileW failed for MELEVR_Log.txt. " + FormatWindowsError(GetLastError()));
            return false;
        }
    }

    const std::string bytes = line + "\r\n";
    DWORD bytesWritten = 0;
    const BOOL writeResult = WriteFile(s_file, bytes.data(), static_cast<DWORD>(bytes.size()), &bytesWritten, nullptr);
    if (!writeResult || bytesWritten != static_cast<DWORD>(bytes.size()))
    {
        DebugLine("WriteFile failed for MELEVR_Log.txt. " + FormatWindowsError(writeResult ? ERROR_SUCCESS : GetLastError()));
        return false;
    }

    return true;
}

// ---- crash logger: name the faulting module/address when the process hard-faults ------------------------
// SetUnhandledExceptionFilter fires on the final unhandled exception (the one that kills the process). This logs
// the faulting address, the module it lands in (dxgi.dll vs MassEffect1.exe), the RVA, and for an access
// violation the read/write + the bad address - then chain to the previous filter so WER still writes its dump.
// Crash-time write uses a CACHED wide path + a stack buffer only (no heap, no std::string) in case the heap is
// the thing that's corrupt.
wchar_t g_crashLogPath[MAX_PATH] = {};
LPTOP_LEVEL_EXCEPTION_FILTER g_prevCrashFilter = nullptr;

void CrashRawWrite(const char* text) noexcept
{
    if (g_crashLogPath[0] == L'\0' || text == nullptr) return;
    HANDLE f = CreateFileW(g_crashLogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(f, text, static_cast<DWORD>(strlen(text)), &wrote, nullptr);
    CloseHandle(f);
}

// Format the faulting module + RVA + access + bad address for an exception and append it to the crash log.
// Stack buffer only (no heap) so it survives heap corruption. `tag` distinguishes the unhandled vs first-chance path.
void LogExceptionAutopsy(EXCEPTION_POINTERS* info, const char* tag) noexcept
{
    if (info == nullptr || info->ExceptionRecord == nullptr) return;
    const EXCEPTION_RECORD* er = info->ExceptionRecord;
    void* addr = er->ExceptionAddress;
    HMODULE mod = nullptr;
    char modName[MAX_PATH] = {};
    uintptr_t base = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &mod) && mod != nullptr)
    {
        GetModuleFileNameA(mod, modName, sizeof(modName));
        base = reinterpret_cast<uintptr_t>(mod);
    }
    const bool av = er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION;
    const char* access = "n/a";
    if (av && er->NumberParameters > 0)
    {
        access = er->ExceptionInformation[0] == 1 ? "write"
               : er->ExceptionInformation[0] == 8 ? "exec" : "read";
    }
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char line[1024] = {};
    sprintf_s(line,
              "%04d-%02d-%02d %02d:%02d:%02d.%03d | [%s] code=0x%08lX addr=%p module=%s rva=0x%llX access=%s badAddr=0x%llX\r\n",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
              tag,
              er->ExceptionCode, addr, modName[0] != '\0' ? modName : "<unknown>",
              static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(addr) - base),
              access,
              (av && er->NumberParameters > 1) ? static_cast<unsigned long long>(er->ExceptionInformation[1]) : 0ull);
    CrashRawWrite(line);
}

LONG WINAPI MeleVrCrashFilter(EXCEPTION_POINTERS* info) noexcept
{
    LogExceptionAutopsy(info, "CRASH");
    return g_prevCrashFilter != nullptr ? g_prevCrashFilter(info) : EXCEPTION_CONTINUE_SEARCH;
}

// FIRST-CHANCE catcher: runs BEFORE any SEH/__try, so it logs the fault even when the game's own handler swallows it
// (which is why the unhandled filter above never fired on the prewarm crash). Serious codes only + rate-limited so a
// benign probed AV does not bury the fatal one; the LAST [FIRSTCHANCE] before the log ends is the kill. Logs only - it
// returns CONTINUE_SEARCH so normal handling is unchanged.
LONG WINAPI MeleVrVectoredFilter(EXCEPTION_POINTERS* info) noexcept
{
    static volatile long s_logged = 0;
    if (info == nullptr || info->ExceptionRecord == nullptr) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    const bool serious =
        code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_STACK_OVERFLOW   || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
        code == EXCEPTION_PRIV_INSTRUCTION || code == 0xC0000409UL /* STATUS_STACK_BUFFER_OVERRUN (fast-fail) */;
    if (!serious) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&s_logged) > 200) return EXCEPTION_CONTINUE_SEARCH;
    LogExceptionAutopsy(info, "FIRSTCHANCE");
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashLogger() noexcept
{
    try
    {
        const std::wstring path = GetLogFilePath();
        wcsncpy_s(g_crashLogPath, path.c_str(), _TRUNCATE);
    }
    catch (...)
    {
    }
    AddVectoredExceptionHandler(1, MeleVrVectoredFilter);  // first-chance, before the game's SEH can swallow it
    g_prevCrashFilter = SetUnhandledExceptionFilter(MeleVrCrashFilter);
}
}

namespace MELEVR::Logger
{
std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }

    const int requiredBytes = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (requiredBytes <= 0)
    {
        return "<utf8 conversion failed>";
    }

    std::string result(static_cast<size_t>(requiredBytes), '\0');
    const int writtenBytes = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), requiredBytes, nullptr, nullptr);
    if (writtenBytes <= 0)
    {
        return "<utf8 conversion failed>";
    }

    result.resize(static_cast<size_t>(writtenBytes - 1));
    return result;
}

void LogLine(const std::string& message) noexcept
{
    try
    {
        const std::string line = Timestamp() + " | " + message;

        std::lock_guard<std::mutex> lock(LogMutex());
        DebugLine(line);

        const std::wstring folderPath = GetLogFolderPath();
        if (!EnsureDirectoryExists(folderPath))
        {
            return;
        }

        AppendLineToFile(GetLogFilePath(), line);
    }
    catch (...)
    {
        OutputDebugStringA("[MELEVR] Unexpected exception while logging.\r\n");
    }
}

void LogWindowsError(const char* context, DWORD errorCode) noexcept
{
    try
    {
        LogLine(std::string(context != nullptr ? context : "<null context>") + ". " + FormatWindowsError(errorCode));
    }
    catch (...)
    {
        OutputDebugStringA("[MELEVR] Unexpected exception while logging a Win32 error.\r\n");
    }
}

std::wstring GetDataFolderPath() noexcept
{
    try
    {
        return GetLogFolderPath();
    }
    catch (...)
    {
        OutputDebugStringA("[MELEVR] Unexpected exception while resolving data folder path.\r\n");
        return L".";
    }
}

bool EnsureDataFolderExists() noexcept
{
    try
    {
        return EnsureDirectoryExists(GetLogFolderPath());
    }
    catch (...)
    {
        OutputDebugStringA("[MELEVR] Unexpected exception while creating data folder.\r\n");
        return false;
    }
}

void LogStartup(HMODULE currentModule) noexcept
{
    try
    {
        LogLine("process name: " + GetProcessName());
        LogLine("current module path: " + WideToUtf8(GetModulePath(currentModule)));
    }
    catch (...)
    {
        OutputDebugStringA("[MELEVR] Unexpected exception during startup logging.\r\n");
    }
    InstallCrashLogger();  // name the faulting module/address on the next hard crash
}
}
