#include "dxgi_proxy.h"

#include <iterator>
#include <mutex>
#include <string>

#include "d3d_capture.h"
#include "logger.h"
#include "MinHook.h"

// Clean-rebuild dxgi proxy. Loads the REAL system dxgi.dll and forwards every export to
// it so the game renders exactly as normal. Milestone-1 step 0a installs NO hooks - there
// is deliberately no PresentProbe / factory-hook call here. The game runs untouched.

namespace
{
HMODULE g_realDxgi = nullptr;
HRESULT g_loadResult = E_FAIL;
std::once_flag g_loadOnce;
std::once_flag g_factoryHookOnce;

using CreateDXGIFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
using CreateDXGIFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);

CreateDXGIFactoryFn g_realCreateDXGIFactory = nullptr;
CreateDXGIFactoryFn g_realCreateDXGIFactory1 = nullptr;
CreateDXGIFactory2Fn g_realCreateDXGIFactory2 = nullptr;

std::string HexPointer(const void* p)
{
    char buf[32] = {};
    sprintf_s(buf, "0x%p", p);
    return std::string(buf);
}

std::wstring BuildSystemDxgiPath()
{
    wchar_t systemDirectory[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(systemDirectory, static_cast<UINT>(std::size(systemDirectory)));
    if (length == 0 || length >= static_cast<UINT>(std::size(systemDirectory)))
    {
        return L"C:\\Windows\\System32\\dxgi.dll";
    }

    std::wstring path(systemDirectory);
    if (!path.empty() && path.back() != L'\\')
    {
        path += L'\\';
    }

    path += L"dxgi.dll";
    return path;
}

void LoadRealDxgiOnce() noexcept
{
    try
    {
        const std::wstring path = BuildSystemDxgiPath();
        g_realDxgi = LoadLibraryW(path.c_str());
        if (g_realDxgi == nullptr)
        {
            const DWORD error = GetLastError();
            g_loadResult = HRESULT_FROM_WIN32(error);
            MELEVR::Logger::LogWindowsError("LoadLibraryW failed for real dxgi.dll", error);
            return;
        }

        g_loadResult = S_OK;
        MELEVR::Logger::LogLine("loaded real dxgi.dll: " + MELEVR::Logger::WideToUtf8(path));
    }
    catch (...)
    {
        g_loadResult = E_FAIL;
        OutputDebugStringA("[MELEVR] Unexpected exception while loading real dxgi.dll.\r\n");
    }
}

template <typename FunctionType>
FunctionType ResolveTypedExport(const char* exportName) noexcept
{
    return reinterpret_cast<FunctionType>(MELEVR::DxgiProxy::ResolveExport(exportName));
}

void TryCaptureFactory(const char* name, HRESULT result, REFIID riid, void** factory) noexcept
{
    if (SUCCEEDED(result) && factory != nullptr && *factory != nullptr)
    {
        MELEVR::Logger::LogLine(std::string("[DXGIHOOK] ") + name + " captured factory " +
                                HexPointer(*factory));
        MELEVR::D3DCapture::TryInstallFactoryHooks(*factory, riid);
    }
}

HRESULT WINAPI RealCreateDXGIFactoryHook(REFIID riid, void** factory)
{
    const HRESULT result = g_realCreateDXGIFactory != nullptr ? g_realCreateDXGIFactory(riid, factory) : E_FAIL;
    TryCaptureFactory("CreateDXGIFactory", result, riid, factory);
    return result;
}

HRESULT WINAPI RealCreateDXGIFactory1Hook(REFIID riid, void** factory)
{
    const HRESULT result = g_realCreateDXGIFactory1 != nullptr ? g_realCreateDXGIFactory1(riid, factory) : E_FAIL;
    TryCaptureFactory("CreateDXGIFactory1", result, riid, factory);
    return result;
}

HRESULT WINAPI RealCreateDXGIFactory2Hook(UINT flags, REFIID riid, void** factory)
{
    const HRESULT result = g_realCreateDXGIFactory2 != nullptr ? g_realCreateDXGIFactory2(flags, riid, factory) : E_FAIL;
    TryCaptureFactory("CreateDXGIFactory2", result, riid, factory);
    return result;
}

void HookFactoryExport(const char* name, void* hook, void** original) noexcept
{
    FARPROC target = GetProcAddress(g_realDxgi, name);
    if (target == nullptr)
    {
        MELEVR::Logger::LogLine(std::string("[DXGIHOOK] ") + name + " export not found.");
        return;
    }

    MH_STATUS status = MH_CreateHook(reinterpret_cast<void*>(target), hook, original);
    if (status != MH_OK && status != MH_ERROR_ALREADY_CREATED)
    {
        MELEVR::Logger::LogLine(std::string("[DXGIHOOK] MH_CreateHook failed for ") + name +
                                " status=" + std::to_string(static_cast<int>(status)));
        return;
    }

    status = MH_EnableHook(reinterpret_cast<void*>(target));
    if (status != MH_OK && status != MH_ERROR_ENABLED)
    {
        MELEVR::Logger::LogLine(std::string("[DXGIHOOK] MH_EnableHook failed for ") + name +
                                " status=" + std::to_string(static_cast<int>(status)));
        return;
    }

    MELEVR::Logger::LogLine(std::string("[DXGIHOOK] real dxgi ") + name + " hook installed.");
}
}

namespace MELEVR::DxgiProxy
{
bool EnsureRealDxgiLoaded() noexcept
{
    try
    {
        std::call_once(g_loadOnce, LoadRealDxgiOnce);
        return SUCCEEDED(g_loadResult) && g_realDxgi != nullptr;
    }
    catch (...)
    {
        OutputDebugStringA("[MELEVR] Unexpected exception in EnsureRealDxgiLoaded.\r\n");
        return false;
    }
}

void InstallRealFactoryHooks() noexcept
{
    try
    {
        std::call_once(g_factoryHookOnce, []()
        {
            if (!EnsureRealDxgiLoaded())
            {
                Logger::LogLine("[DXGIHOOK] skipped: real dxgi.dll is not loaded.");
                return;
            }

            MH_STATUS init = MH_Initialize();
            if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
            {
                Logger::LogLine("[DXGIHOOK] MH_Initialize failed status=" +
                                std::to_string(static_cast<int>(init)));
                return;
            }

            HookFactoryExport("CreateDXGIFactory",
                              reinterpret_cast<void*>(&RealCreateDXGIFactoryHook),
                              reinterpret_cast<void**>(&g_realCreateDXGIFactory));
            HookFactoryExport("CreateDXGIFactory1",
                              reinterpret_cast<void*>(&RealCreateDXGIFactory1Hook),
                              reinterpret_cast<void**>(&g_realCreateDXGIFactory1));
            HookFactoryExport("CreateDXGIFactory2",
                              reinterpret_cast<void*>(&RealCreateDXGIFactory2Hook),
                              reinterpret_cast<void**>(&g_realCreateDXGIFactory2));
        });
    }
    catch (...)
    {
        OutputDebugStringA("[MELEVR] Unexpected exception while installing real DXGI hooks.\r\n");
    }
}

FARPROC ResolveExport(const char* exportName) noexcept
{
    if (exportName == nullptr || exportName[0] == '\0')
    {
        Logger::LogLine("ResolveExport called with an empty export name.");
        return nullptr;
    }

    if (!EnsureRealDxgiLoaded())
    {
        Logger::LogLine(std::string("Cannot resolve dxgi export because real dxgi.dll is not loaded: ") + exportName);
        return nullptr;
    }

    FARPROC proc = GetProcAddress(g_realDxgi, exportName);
    if (proc == nullptr)
    {
        const DWORD error = GetLastError();
        Logger::LogWindowsError((std::string("Missing real dxgi.dll export: ") + exportName).c_str(), error);
    }

    return proc;
}

HRESULT MissingExportResult(const char* exportName) noexcept
{
    Logger::LogLine(std::string("Returning failure for missing dxgi export: ") + (exportName != nullptr ? exportName : "<null>"));
    return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
}
}

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** factory)
{
    using Function = HRESULT(WINAPI*)(REFIID, void**);
    const auto realFunction = ResolveTypedExport<Function>("CreateDXGIFactory");
    if (realFunction == nullptr)
    {
        return MELEVR::DxgiProxy::MissingExportResult("CreateDXGIFactory");
    }

    const HRESULT result = realFunction(riid, factory);
    if (SUCCEEDED(result) && factory != nullptr && *factory != nullptr)
    {
        MELEVR::D3DCapture::TryInstallFactoryHooks(*factory, riid);
    }
    return result;
}

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** factory)
{
    using Function = HRESULT(WINAPI*)(REFIID, void**);
    const auto realFunction = ResolveTypedExport<Function>("CreateDXGIFactory1");
    if (realFunction == nullptr)
    {
        return MELEVR::DxgiProxy::MissingExportResult("CreateDXGIFactory1");
    }

    const HRESULT result = realFunction(riid, factory);
    if (SUCCEEDED(result) && factory != nullptr && *factory != nullptr)
    {
        MELEVR::D3DCapture::TryInstallFactoryHooks(*factory, riid);
    }
    return result;
}

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** factory)
{
    using Function = HRESULT(WINAPI*)(UINT, REFIID, void**);
    const auto realFunction = ResolveTypedExport<Function>("CreateDXGIFactory2");
    if (realFunction == nullptr)
    {
        return MELEVR::DxgiProxy::MissingExportResult("CreateDXGIFactory2");
    }

    const HRESULT result = realFunction(flags, riid, factory);
    if (SUCCEEDED(result) && factory != nullptr && *factory != nullptr)
    {
        MELEVR::D3DCapture::TryInstallFactoryHooks(*factory, riid);
    }
    return result;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DXGIDeclareAdapterRemovalSupport()
{
    using Function = HRESULT(WINAPI*)();
    const auto realFunction = ResolveTypedExport<Function>("DXGIDeclareAdapterRemovalSupport");
    if (realFunction == nullptr)
    {
        return MELEVR::DxgiProxy::MissingExportResult("DXGIDeclareAdapterRemovalSupport");
    }

    return realFunction();
}

extern "C" __declspec(dllexport) HRESULT WINAPI DXGIGetDebugInterface(REFIID riid, void** debugInterface)
{
    using Function = HRESULT(WINAPI*)(REFIID, void**);
    const auto realFunction = ResolveTypedExport<Function>("DXGIGetDebugInterface");
    if (realFunction == nullptr)
    {
        return MELEVR::DxgiProxy::MissingExportResult("DXGIGetDebugInterface");
    }

    return realFunction(riid, debugInterface);
}

extern "C" __declspec(dllexport) HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, REFIID riid, void** debugInterface)
{
    using Function = HRESULT(WINAPI*)(UINT, REFIID, void**);
    const auto realFunction = ResolveTypedExport<Function>("DXGIGetDebugInterface1");
    if (realFunction == nullptr)
    {
        return MELEVR::DxgiProxy::MissingExportResult("DXGIGetDebugInterface1");
    }

    return realFunction(flags, riid, debugInterface);
}
