#ifndef WINRINGS_SHELL_EXPORTS
#define WINRINGS_SHELL_EXPORTS
#endif
#include <Windows.h>

#include "../shared/WinRingsApi.h"
#include "../shared/WinRingsIdentity.h"
#include "ShellController.h"
#include "XamlTaskbarCtrl.h"

namespace
{
    HMODULE g_shellModule = nullptr;

    BOOL WINAPI ApiRequestShutdown(HWND controlWindow)
    {
        return winrings::ShellController::RequestShutdownFromApi(controlWindow) ? TRUE : FALSE;
    }

    const WinRingsApi g_api = {
        static_cast<UINT32>(sizeof(WinRingsApi)),
        WINRINGS_ABI_VERSION,
        WINRINGS_PRODUCT_VERSION,
        0u,
        &ApiRequestShutdown
    };

    DWORD WINAPI BootstrapWorkerProc(void* param)
    {
        winrings::ShellController::BootstrapInExplorer(static_cast<HMODULE>(param));
        return 0;
    }
} // namespace

extern "C" const WinRingsApi* WINAPI WinRings_GetApi(UINT32 requestedAbi)
{
    if (requestedAbi != WINRINGS_ABI_VERSION)
        return nullptr;
    return &g_api;
}

extern "C" int WINAPI WinRings_RunPreview(void)
{
    return winrings::ShellController::RunStandalonePreview(g_shellModule);
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** result)
{
    return winrings::XamlTaskbarCtrl::GetClassObject(clsid, iid, result);
}

STDAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_shellModule = module;
        HANDLE thread = CreateThread(nullptr, 0, BootstrapWorkerProc, module, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }
    return TRUE;
}
