#pragma once

// Restart-only opt-in: an in-flight presentation transport must not silently
// change back into the game's DLSS mirror during a live configuration reload.
static void PresentAdapterPath(wchar_t* path, size_t count, const wchar_t* leaf)
{
    path[0] = 0;
    if (GetModuleFileNameW(g_self, path, static_cast<DWORD>(count)) == 0) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash == nullptr) { path[0] = 0; return; }
    slash[1] = 0;
    wcscat_s(path, count, leaf);
}

static bool PresentationAdapterEnabled()
{
    static const bool enabled = [] {
        wchar_t path[1024] = {};
        PresentAdapterPath(path, _countof(path), L"vk-present-adapter.ini");
        return path[0] != 0 && GetPrivateProfileIntW(L"Adapter", L"Enabled", 0, path) == 1;
    }();
    return enabled;
}

// Source=fg-input processes the real frame at the native frame-generation input;
// any other value keeps the ReShade begin-effects (post-FG) path.
static bool PresentAdapterFgInput()
{
    static const bool fg_input = [] {
        wchar_t path[1024] = {};
        PresentAdapterPath(path, _countof(path), L"vk-present-adapter.ini");
        wchar_t value[64] = {};
        if (path[0] == 0) return false;
        GetPrivateProfileStringW(L"Adapter", L"Source", L"present", value, _countof(value), path);
        return _wcsicmp(value, L"fg-input") == 0;
    }();
    return PresentationAdapterEnabled() && fg_input;
}

// AutoRun=1 starts continuous NR without a request file; a request with frames=0 still stops it.
static bool PresentAdapterAutoRun()
{
    static const bool auto_run = [] {
        wchar_t path[1024] = {};
        PresentAdapterPath(path, _countof(path), L"vk-present-adapter.ini");
        return path[0] != 0 && GetPrivateProfileIntW(L"Adapter", L"AutoRun", 0, path) == 1;
    }();
    return PresentAdapterFgInput() && auto_run;
}

// Follow=1: Generic's own hook point selects the path. Present(2) runs the FG-input
// feed continuously; Upscaled(0)/Render(1) run the native mirror as before.
static bool PresentAdapterFollow()
{
    static const bool follow = [] {
        wchar_t path[1024] = {};
        PresentAdapterPath(path, _countof(path), L"vk-present-adapter.ini");
        return path[0] != 0 && GetPrivateProfileIntW(L"Adapter", L"Follow", 0, path) == 1;
    }();
    return PresentAdapterFgInput() && follow;
}

static int PresentAdapterHookPoint()
{
    static ULONGLONG next_read;
    static int cached;
    const ULONGLONG now = GetTickCount64();
    if (now >= next_read) {
        next_read = now + 500;
        wchar_t ini[1024] = {};
        PresentAdapterPath(ini, _countof(ini), L"ReShade.ini");
        cached = GetPrivateProfileIntW(L"RenoDX.DLSS5", L"NRHookPoint", 0, ini);
    }
    return cached;
}

// Which path holds the private session; mirror and FG-input never share it live.
enum AdapterOwner { OWNER_NONE, OWNER_MIRROR, OWNER_FG };
static volatile LONG g_adapter_owner = OWNER_NONE;

static bool FgInputActive()
{
    if (!PresentAdapterFgInput()) return false;
    return !PresentAdapterFollow() || PresentAdapterHookPoint() == 2;
}
