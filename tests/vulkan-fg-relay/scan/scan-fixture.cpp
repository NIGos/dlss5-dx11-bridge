#include <windows.h>
#include <cstdio>

using DetachObserver = void (WINAPI *)(void *, BOOL);

static DetachObserver g_observer = nullptr;
static void *g_context = nullptr;

extern "C" __declspec(dllexport) void WINAPI ScanFixtureObserve(
    DetachObserver observer, void *context)
{
    g_context = context;
    g_observer = observer;
}

extern "C" __declspec(dllexport) int WINAPI ScanFixtureValue()
{
    return 0x514758;
}

#if defined(SCAN_FORWARD_TARGET)
static void SignalTargetEvent(const wchar_t *kind)
{
    wchar_t name[96] = {};
    if (swprintf_s(name, L"Local\\NgxScanTarget%s_%lu", kind,
                   GetCurrentProcessId()) < 0)
        return;
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (event != nullptr) {
        SetEvent(event);
        CloseHandle(event);
    }
}
#endif

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved)
{
#if defined(SCAN_FORWARD_TARGET)
    if (reason == DLL_PROCESS_ATTACH)
        SignalTargetEvent(L"Attach");
    if (reason == DLL_PROCESS_DETACH)
        SignalTargetEvent(L"Detach");
#endif
    if (reason == DLL_PROCESS_DETACH && g_observer != nullptr)
        g_observer(g_context, reserved != nullptr);
    return TRUE;
}
