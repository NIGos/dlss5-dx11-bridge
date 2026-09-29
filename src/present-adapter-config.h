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

// Pipeline=0 (default) parks the FG evaluate until the real frame's NR is done.
// Pipeline=1 is a throughput probe: the same NR runs on the private device while
// frame generation proceeds on the game's own image, and the result is discarded.
// It measures the GPU headroom a one-frame-delayed pipeline could use; the
// picture is the game's own, without NR, for the duration.
static int PresentAdapterPipeline()
{
    static const int pipeline = [] {
        wchar_t path[1024] = {};
        PresentAdapterPath(path, _countof(path), L"vk-present-adapter.ini");
        return path[0] != 0 ? GetPrivateProfileIntW(L"Adapter", L"Pipeline", 0, path) : 0;
    }();
    return PresentAdapterFgInput() ? pipeline : 0;
}

// Pipeline=2 and 3 hand the feed's synchronisation to the GPU relay in
// fg-relay.inc. 2 copies the FG inputs at the evaluate, as this add-on's own
// batch on DLSSG.CmdQueue; 3 copies them at the game's vkQueuePresentKHR, on the
// game's present queue and about 5 ms earlier. Both release the evaluate's park
// from the D3D12 side. See fg-relay.inc for the chain and its fallbacks.
static bool PresentAdapterRelay()
{
    const int p = PresentAdapterPipeline();
    return p == 2 || p == 3;
}

static bool PresentAdapterRelayAtEvaluate() { return PresentAdapterPipeline() == 2; }
static bool PresentAdapterRelayAtPresent()  { return PresentAdapterPipeline() == 3; }

// Import=1 lets the relay use the D3D12-fence <-> Vulkan-timeline-semaphore import,
// and has it add VK_KHR_external_semaphore_win32 to the game's device at creation
// (a hook on the ReShade layer's vkCreateDevice that appends the name to a copy of
// the create info, with the caller's own list retried unchanged if the driver
// refuses it). The import is taken only for a device that create accepted the name
// on: a non-null vkGetDeviceProcAddr answer is not proof the extension is enabled,
// so the KHR entry points are never called on a device that was not shown to carry
// it. Off by default.
static bool PresentAdapterRelayImport()
{
    static const bool import = [] {
        wchar_t path[1024] = {};
        PresentAdapterPath(path, _countof(path), L"vk-present-adapter.ini");
        return path[0] != 0 && GetPrivateProfileIntW(L"Adapter", L"Import", 0, path) == 1;
    }();
    return PresentAdapterRelay() && import;
}

// Throttle=N (default 1, 0 off, at most 3) holds the game at its Reflex sleep
// (slReflexSleep; the game's vkQueuePresentKHR when the game does not call it)
// until the D3D12 NR of the newest armed frame, less N-1 frames, has completed.
// Reflex paces the game against its own render queue and cannot see the NR on
// the private D3D12 queue; while the GPU is saturated the game otherwise runs
// ahead and whole frames queue up in front of the NR (2026-09-29: PCL 246 ms
// uncapped against 132 ms with a frame cap below throughput). Import=1 only.
static int PresentAdapterRelayThrottle()
{
    static const int depth = [] {
        wchar_t path[1024] = {};
        PresentAdapterPath(path, _countof(path), L"vk-present-adapter.ini");
        const int value = path[0] != 0 ? static_cast<int>(GetPrivateProfileIntW(L"Adapter", L"Throttle", 1, path)) : 1;
        return value < 0 ? 0 : (value > 3 ? 3 : value);
    }();
    return PresentAdapterRelayImport() ? depth : 0;
}

// Restart-only Trace=1 keeps rendering arguments intact; bounded records are flushed by the hook worker.
static bool PresentAdapterTrace()
{
    static const bool trace = [] {
        wchar_t path[1024] = {};
        PresentAdapterPath(path, _countof(path), L"vk-present-adapter.ini");
        return path[0] != 0 && GetPrivateProfileIntW(L"Adapter", L"Trace", 0, path) == 1;
    }();
    return PresentAdapterFgInput() && trace;
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
