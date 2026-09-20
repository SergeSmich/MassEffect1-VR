#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "d3d_capture.h"
#include "dxgi_proxy.h"
#include "logger.h"
#include "xr_session.h"
#include "vr_menu.h"

// MELE VR clean rebuild - Milestone 0: stable flat-mono OpenXR output.
// The DLL loads, opens the log, chains the real dxgi.dll (so the game renders untouched), then
// d3d_capture hooks Present + hands the game device to xr_session, which puts the backbuffer on a
// head-locked flat panel in both eyes. That is the WHOLE mod. Nothing else exists yet - features are
// added back one milestone at a time as fresh, isolated code.

namespace
{
std::atomic_bool g_crashLogged{false};

std::string Hex64(std::uintptr_t v)
{
    char b[32] = {};
    sprintf_s(b, "0x%llX", static_cast<unsigned long long>(v));
    return std::string(b);
}

std::string AddrLabel(void* p)
{
    const auto addr = reinterpret_cast<std::uintptr_t>(p);
    const auto exe = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (exe != 0 && addr >= exe && addr < exe + 0x4000000)
    {
        return std::string("MassEffect1.exe+") + Hex64(addr - exe);
    }

    HMODULE mod = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(p),
                           &mod) && mod != nullptr &&
        GetModuleFileNameW(mod, path, MAX_PATH) > 0)
    {
        std::wstring ws(path);
        const size_t slash = ws.find_last_of(L"\\/");
        if (slash != std::wstring::npos) ws = ws.substr(slash + 1);
        char name[MAX_PATH] = {};
        WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, name, MAX_PATH, nullptr, nullptr);
        return std::string(name) + "+" + Hex64(addr - reinterpret_cast<std::uintptr_t>(mod));
    }
    return Hex64(addr);
}

LONG CALLBACK CrashAutopsyHandler(EXCEPTION_POINTERS* info) noexcept
{
    bool expected = false;
    if (!g_crashLogged.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (info == nullptr || info->ExceptionRecord == nullptr)
    {
        MELEVR::Logger::LogLine("[CRASH] vectored exception with missing EXCEPTION_POINTERS.");
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const DWORD code = info->ExceptionRecord->ExceptionCode;
    void* addr = info->ExceptionRecord->ExceptionAddress;
    std::string line = "[CRASH] code=" + Hex64(code) +
                       " address=" + AddrLabel(addr) +
                       " thread=" + std::to_string(GetCurrentThreadId());
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
    {
        line += " avOp=" + std::to_string(static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[0]));
        line += " avAddr=" + Hex64(static_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionInformation[1]));
    }
    MELEVR::Logger::LogLine(line);

    void* frames[24] = {};
    const USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
    for (USHORT i = 0; i < n; ++i)
    {
        MELEVR::Logger::LogLine("[CRASH] stack[" + std::to_string(i) + "] " + AddrLabel(frames[i]));
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

DWORD WINAPI FlatMonoWorkerThread(LPVOID parameter) noexcept
{
    const auto currentModule = static_cast<HMODULE>(parameter);

    // Clean startup banner (2026-07-16): the old multi-line "[M0] CLEAN REBUILD / Milestone 0 /
    // Features return one at a time" block was stale internal dev narrative that shipped to users.
    // One product line + a compiled-timestamp build stamp = still fully diagnosable (the timestamp is
    // unique per build) without reading like an in-development scratch build.
    MELEVR::Logger::LogLine("=====================================================");
    MELEVR::Logger::LogLine("MELEVR1 - VR mod for Mass Effect Legendary Edition (ME1)");
    MELEVR::Logger::LogLine(std::string("[BUILDSTAMP] MELEVR1 compiled ") + __DATE__ + " " + __TIME__ +
                            " (AERFULL-P1OWN-AERVIEWSTATE)");
    MELEVR::Logger::LogLine("=====================================================");
    MELEVR::Logger::LogStartup(currentModule);
    // [ENGSMOOTH] BIOEngine.ini assert as the first real work at attach: the engine reads its config
    // early in main(), and the game re-syncs this file between sessions, so the only write that
    // sticks is one that lands ahead of that read, every launch.
    MELEVR::D3DCapture::EnsureBioEngineSmoothing();
    MELEVR::Menu::InstallInputBlock();
    MELEVR::Logger::LogLine("[MENUINPUT] early input block install requested.");
    AddVectoredExceptionHandler(1, CrashAutopsyHandler);
    MELEVR::Logger::LogLine("[CRASH] vectored crash autopsy handler installed.");

    // Before the game reads its resolution settings (it clamps to the desktop mode internally, well ahead of
    // both the swapchain and MELEVR.ini). Logging only for now.
    MELEVR::D3DCapture::InstallDisplayQueryHooks();

    if (MELEVR::DxgiProxy::EnsureRealDxgiLoaded())
    {
        MELEVR::Logger::LogLine("[INIT] real dxgi.dll loaded (proxy chained). Game renders untouched.");
        MELEVR::DxgiProxy::InstallRealFactoryHooks();
    }
    else
    {
        MELEVR::Logger::LogLine("[INIT] WARNING: real dxgi.dll did not load - the game may fail to start.");
    }

    return 0;
}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    UNREFERENCED_PARAMETER(reserved);

    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);

        // BEFORE the worker thread, and before main(). The engine reads GamerSettings.ini during its own
        // init and will beat any spawned thread (measured: the worker arrived 0.9s late and the game used
        // the previous launch's resolution). File I/O only - nothing that can deadlock the loader lock.
        MELEVR::D3DCapture::ApplyAutoResEarly();

        HANDLE thread = CreateThread(nullptr, 0, FlatMonoWorkerThread, module, 0, nullptr);
        if (thread != nullptr)
        {
            CloseHandle(thread);
        }
        else
        {
            OutputDebugStringA("[MELEVR] Failed to create FlatMono worker thread.\r\n");
        }
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        MELEVR::XrSession::RequestShutdown();
    }

    return TRUE;
}
