#pragma once
#include <windows.h>

static volatile LONG g_present_config_deferrals = 0;

// A present/FG callback must not wait for the mirror lock: the lock owner may
// itself be waiting for GPU work whose progress requires that same FG thread.
// A missed poll does not consume CfgReload's timer or modify the configuration.
template <typename Reload>
bool TryReloadPresentConfig(CRITICAL_SECTION &section, Reload &&reload)
{
    if (!TryEnterCriticalSection(&section))
    {
        InterlockedIncrement(&g_present_config_deferrals);
        return false;
    }
    struct Release
    {
        CRITICAL_SECTION &section;
        ~Release() { LeaveCriticalSection(&section); }
    } release{section};
    reload();
    return true;
}
