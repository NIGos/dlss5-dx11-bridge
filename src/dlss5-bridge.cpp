// dlss5-bridge - ReShade add-on.
//
// Lets a DLSS 5 neural-rendering add-on that only hooks D3D12 run inside a game
// that renders with D3D11.
//
// The game's NVSDK_NGX_D3D11 CreateFeature and EvaluateFeature are hooked and
// always forwarded untouched, so the game keeps working even if everything here
// fails. The bridge then mirrors the same DLSS contract onto a second NGX
// session running on its own D3D12 device -- and that D3D12 evaluate is the
// call a DLSS 5 add-on detours and inserts itself into. Nothing about the other
// add-on is modified; it simply receives genuine D3D12 NGX calls.
//
// Nothing on disk is patched. MinHook installs in-memory detours at NGX
// entry points and provides relocated trampolines for forwarding original calls.
//
// Behaviour is driven by dlss5-bridge.cfg, re-read while the game runs, so
// settings can be changed without restarting. dlss5-bridge.log records the
// contract that was read, which resource-sharing direction the driver accepted,
// and the result of every NGX call.
//
// Tested on Baldur's Gate 3. Nothing here is specific to it: the NGX entry
// points are located by export name in whatever module exports them, and every
// size and offset is taken from the game's own parameter block.
//
// Build:
//   cl /nologo /LD /EHsc /O2 /MT /std:c++17 /Ireshade dlss5-bridge.cpp version.res \
//      /link /OUT:dlss5-bridge.addon64 kernel32.lib user32.lib advapi32.lib
//
// The line above used to omit /std:c++17, /Ireshade, version.res and advapi32.lib,
// and did not compile: the vendored ReShade headers use nested namespace
// definitions, and RegGetValueW is in advapi32. Nobody noticed because nobody
// builds from the comment -- which is exactly why it is worth it being right.
//
// The test binary is the same file with a different entry point:
//   cl /nologo /EHsc /MT /std:c++17 /Ireshade /Fe:probe.exe probe.cpp \
//      /link kernel32.lib user32.lib advapi32.lib version.lib
// See probe.cpp for what it covers and, more importantly, what it does not.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <MinHook.h>
#include "module-lifetime.h"
#include "ngx-module-scan.h"
// SHA-256 for identifying a neighbour build whose version resource does not.
#include <bcrypt.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
// Header-only, ships with the Windows SDK, adds no lib to the link line. It is
// here for one call: the optical flow self-check reads back an R16G16_FLOAT
// texture and has to decode halves, subnormals included -- a UV delta below one
// pixel at 3840 wide is subnormal, which is exactly the regime this whole path
// exists for, so a hand-rolled decoder would be wrong in the only case that
// matters.
#include <DirectXPackedVector.h>
#include <cassert>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include "bounded-report.h"
#include "present-config-gate.h"
#pragma comment(lib, "version.lib")

// Kept in step with version.rc, which is where ReShade's overlay reads it from.
#define BRIDGE_VERSION "1.4.13-pre8-vk-fgrelay"

extern "C" __declspec(dllexport) const char *NAME =
    "DLSS 5 Bridge " BRIDGE_VERSION;
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Lets D3D12-only DLSS 5 add-ons run in a D3D11 game. Intercepts the game's "
    "NVSDK_NGX_D3D11 evaluate, forwards it untouched, and mirrors the same "
    "contract onto a second NGX session on its own D3D12 device -- which is "
    "where a DLSS 5 neural-rendering add-on can insert itself. "
    "Settings in dlss5-bridge.cfg, re-read while the game runs.";

// ---------------------------------------------------------------------------
// NGX declarations
//
// Deliberately mirrors the declaration order of NVIDIA's nvsdk_ngx.h. MSVC
// emits same-name virtual overloads in reverse declaration order, which puts
// Get(const char*, ID3D12Resource**) on vtable slot 0x48 -- the slot the
// shipping NGX consumers call. Keeping the order identical means the compiler
// reproduces NVIDIA's layout and no offset has to be hardcoded here.
// ---------------------------------------------------------------------------

struct ID3D12Resource;  // opaque, never dereferenced

typedef int NVSDK_NGX_Result;
static const NVSDK_NGX_Result NGX_SUCCESS = 1;

struct NVSDK_NGX_Handle { unsigned int Id; };

struct NVSDK_NGX_Parameter
{
    virtual void Set(const char *InName, unsigned long long InValue) = 0;
    virtual void Set(const char *InName, float InValue) = 0;
    virtual void Set(const char *InName, double InValue) = 0;
    virtual void Set(const char *InName, unsigned int InValue) = 0;
    virtual void Set(const char *InName, int InValue) = 0;
    virtual void Set(const char *InName, ID3D11Resource *InValue) = 0;
    virtual void Set(const char *InName, ID3D12Resource *InValue) = 0;
    virtual void Set(const char *InName, void *InValue) = 0;

    virtual NVSDK_NGX_Result Get(const char *InName, unsigned long long *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, float *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, double *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, unsigned int *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, int *OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, ID3D11Resource **OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, ID3D12Resource **OutValue) const = 0;
    virtual NVSDK_NGX_Result Get(const char *InName, void **OutValue) const = 0;

    virtual void Reset() = 0;
};

typedef NVSDK_NGX_Result (*PFN_Evaluate)(ID3D11DeviceContext *, const NVSDK_NGX_Handle *,
                                         const NVSDK_NGX_Parameter *, void *);
typedef NVSDK_NGX_Result (*PFN_Create)(ID3D11DeviceContext *, int,
                                       NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

static CRITICAL_SECTION g_log_cs;
static char             g_log_path[MAX_PATH];
static HMODULE          g_self;
#include "present-adapter-config.h"

// Anything that means "your setup is wrong" also goes into ReShade's own log,
// where its overlay shows it. People reliably post ReShade.log instead of this
// add-on's, so the messages that matter should be in both.
typedef void (*PFN_ReShadeLogMessage)(HMODULE, int, const char *);
static PFN_ReShadeLogMessage g_reshade_log;
static HMODULE               g_reshade_module;

// Annotated so the compiler counts the conversions against the arguments. It is
// unannotated varargs that let a config line ship with sixteen conversions and
// fifteen values, printing a stack slot as ofa_perf in every log this project
// has collected -- found by reading, because nothing warned.
static void Log(_Printf_format_string_ const char *fmt, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);

    // A per-frame message that nobody stopped can fill a disk on someone else's
    // machine. Bound the file so the worst case is a truncated log, not that.
    static long   written = 0;
    static bool   capped  = false;
    const  long   kCap    = 8 * 1024 * 1024;

    EnterCriticalSection(&g_log_cs);
    if (!capped)
    {
        FILE *f = nullptr;
        if (fopen_s(&f, g_log_path, "a") == 0 && f != nullptr)
        {
            written += fprintf(f, "%02u:%02u:%02u.%03u  %s\n", st.wHour, st.wMinute,
                               st.wSecond, st.wMilliseconds, line);
            if (written > kCap)
            {
                fprintf(f, "\n--- log capped at 8 MB. Something is repeating every "
                           "frame; the lines above still say what. ---\n");
                capped = true;
            }
            fclose(f);
        }
    }
    LeaveCriticalSection(&g_log_cs);
}

// Same as Log, but the message is also raised in ReShade so the user sees it in
// the overlay without having to find a file. Reserved for conditions that stop
// the bridge working -- routine progress stays in this add-on's own log.
static void Warn(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);

    Log("%s", line);

    if (g_reshade_log != nullptr)
    {
        char tagged[1100];
        _snprintf_s(tagged, sizeof(tagged), _TRUNCATE, "[DLSS 5 Bridge] %s", line);
        g_reshade_log(g_reshade_module, 1 /* error */, tagged);
    }
}

// ---------------------------------------------------------------------------
// Crash reporting
//
// Reports so far have all ended the same way: a log that simply stops, with no
// way to tell whether the game died there or the file was just truncated when
// it was copied. These three pieces answer that without having to ask.
//
// The breadcrumb names what was last being attempted, the filter records where
// a fatal exception actually landed and in whose code, and the next run says
// whether the previous one ended cleanly.
// ---------------------------------------------------------------------------

static const char *volatile g_where = "starting up";

static void Breadcrumb(const char *what) { g_where = what; }

static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter;

// A module that has been unloaded cannot be named after the fact: the address is
// free memory and every lookup returns nothing. This crash filter printed exactly
// that -- "memory that is no longer mapped" -- for a fault at process teardown,
// which says the code was unloaded but not whose it was.
//
// So the module list is photographed while it still exists, and the photograph is
// what the filter consults when the live lookup fails. Taken at registration and
// again whenever an effect runtime is destroyed, which is the edge every teardown
// fault so far has been behind.
struct ModuleShot { uintptr_t base; uintptr_t end; wchar_t name[64]; };
static ModuleShot g_shot[256];
static LONG       g_shot_n;

static void ModuleSnapshot()
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32 == nullptr) return;
    auto enum_mods = reinterpret_cast<BOOL (WINAPI *)(HANDLE, HMODULE *, DWORD, LPDWORD)>(
        GetProcAddress(k32, "K32EnumProcessModules"));
    auto mod_info = reinterpret_cast<BOOL (WINAPI *)(HANDLE, HMODULE, void *, DWORD)>(
        GetProcAddress(k32, "K32GetModuleInformation"));
    if (enum_mods == nullptr || mod_info == nullptr) return;

    HMODULE mods[256] = {};
    DWORD   need = 0;
    if (!enum_mods(GetCurrentProcess(), mods, sizeof(mods), &need)) return;

    struct { LPVOID base; DWORD size; LPVOID entry; } mi = {};
    const DWORD count = need / sizeof(HMODULE) < 256 ? need / sizeof(HMODULE) : 256;
    LONG n = 0;
    for (DWORD i = 0; i < count; ++i)
    {
        if (!mod_info(GetCurrentProcess(), mods[i], &mi, sizeof(mi))) continue;
        wchar_t path[MAX_PATH] = L"";
        if (GetModuleFileNameW(mods[i], path, MAX_PATH) == 0) continue;
        const wchar_t *leaf = wcsrchr(path, L'\\');
        leaf = leaf != nullptr ? leaf + 1 : path;
        g_shot[n].base = reinterpret_cast<uintptr_t>(mi.base);
        g_shot[n].end  = g_shot[n].base + mi.size;
        wcsncpy_s(g_shot[n].name, leaf, _TRUNCATE);
        ++n;
    }
    InterlockedExchange(&g_shot_n, n);
}

// Names the module an address belonged to WHEN THE PHOTOGRAPH WAS TAKEN. The
// offset is the useful half: it survives the unload and points at one function.
static bool ModuleSnapshotNameAt(const void *addr, wchar_t *out, size_t cch)
{
    const uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    const LONG n = InterlockedCompareExchange(&g_shot_n, 0, 0);
    for (LONG i = 0; i < n; ++i)
        if (a >= g_shot[i].base && a < g_shot[i].end)
        {
            swprintf_s(out, cch, L"%s+0x%llX (unloaded before the fault; named from the "
                                 L"module list photographed at teardown)",
                       g_shot[i].name,
                       static_cast<unsigned long long>(a - g_shot[i].base));
            return true;
        }
    return false;
}

// Defined with the hook table below; a module unloaded after being hooked is
// no longer a module, but its base is still recorded there.
static void NameHookedLayerAt(const void *base, wchar_t *out, size_t cch);

// Defined with the detour reader below; a read that faults must not fault here.
static bool SafeReadPtr(const void *const *slot, const void **out);

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS *ep)
{
    const void *addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;

    // Whose code faulted matters more than the address itself: it separates a
    // bug in here from one in the game, the driver, or another add-on.
    wchar_t owner[MAX_PATH] = L"unknown";
    HMODULE mod = nullptr;
    if (addr != nullptr &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(addr), &mod) && mod != nullptr)
        GetModuleFileNameW(mod, owner, MAX_PATH);

    // "unknown" on its own is not an answer, and it is the answer that arrives
    // whenever the faulting code has been unloaded or was mapped without being
    // registered as a module -- both of which say something specific.
    if (mod == nullptr && addr != nullptr)
    {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi))
        {
            if (mbi.State == MEM_FREE)
            {
                if (!ModuleSnapshotNameAt(addr, owner, MAX_PATH))
                    wcscpy_s(owner, L"memory that is no longer mapped -- the code that "
                                    L"faulted has been unloaded, and it was not in the "
                                    L"module list photographed at teardown either");
            }
            else
            {
                // A module hooked earlier can be unloaded while its base is still
                // recorded here. GetModuleHandleEx finds nothing; the base does.
                NameHookedLayerAt(mbi.AllocationBase, owner, MAX_PATH);

                if (wcscmp(owner, L"unknown") == 0)
                    if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll"))
                        if (auto mapped = reinterpret_cast<DWORD (WINAPI *)(HANDLE, LPVOID, LPWSTR, DWORD)>(
                                GetProcAddress(k32, "K32GetMappedFileNameW")))
                            if (mapped(GetCurrentProcess(), const_cast<void *>(addr), owner, MAX_PATH) == 0)
                                wcscpy_s(owner, L"unknown");
            }
        }
    }

    Log("");
    // A marker the next run looks for. Recording a crash when one actually
    // happens is reliable; inferring one from a missing clean-exit marker is
    // not, because plenty of games terminate without ever running DLL detach.
    Log("### CRASH RECORDED ###");
    // The version, inside the block. The next launch reprints this block under its
    // own banner, so without this line a 1.3.0 fault is read against 1.4.0's
    // offsets by whoever triages it.
    Log("  recorded by dlss5-bridge %s (built %s %s)", BRIDGE_VERSION, __DATE__, __TIME__);
    Log("  exception 0x%08X at %p", code, addr);
    Log("  in: %ls", owner);

    // For an access violation the record says what kind. Reading freed memory and
    // executing it are different bugs with the same code, and the second one --
    // a call into a module that has been unloaded -- is the one that looks like a
    // mystery until this line names it.
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
    {
        const ULONG_PTR kind = ep->ExceptionRecord->ExceptionInformation[0];
        Log("  access violation: %s at %p",
            kind == 0 ? "read" : kind == 1 ? "write" : kind == 8 ? "executed code" : "unknown access",
            reinterpret_cast<void *>(ep->ExceptionRecord->ExceptionInformation[1]));
    }

    // Which thread. A fault on a thread whose entry point is in another module is
    // that module's thread, whatever the add-on happened to be doing at the time.
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
        if (auto qti = reinterpret_cast<LONG (WINAPI *)(HANDLE, int, PVOID, ULONG, PULONG)>(
                GetProcAddress(nt, "NtQueryInformationThread")))
        {
            void *start = nullptr;
            if (qti(GetCurrentThread(), 9 /* ThreadQuerySetWin32StartAddress */,
                    &start, sizeof(start), nullptr) == 0 && start != nullptr)
            {
                wchar_t who[MAX_PATH] = L"";
                HMODULE sm = nullptr;
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       static_cast<LPCWSTR>(start), &sm) && sm != nullptr)
                    GetModuleFileNameW(sm, who, MAX_PATH);
                else if (!ModuleSnapshotNameAt(start, who, MAX_PATH))
                    wcscpy_s(who, L"unknown");
                Log("  faulting thread %lu started at %p, in %ls", GetCurrentThreadId(), start, who);
            }
        }
    Log("  this add-on was last doing: %s", g_where);
    Log("  %s", mod == g_self ? "That address is inside this add-on, so this one is on me."
                              : "That address is not in this add-on.");

    // Who called it. "Faulted in X" names the victim; the return addresses name
    // the path, and when the fault is a call into an unloaded module the caller
    // is the whole question. This filter runs on the faulting thread, so its own
    // stack is that thread's stack.
    void *frames[32] = {};
    const USHORT got = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
    if (got > 0)
    {
        Log("  called from (thread %lu):", GetCurrentThreadId());
        // When the fault is a call into an unloaded module the stack walk stops
        // there: unwinding needs the unwind tables of the faulting function and
        // those went with it. The return address is still on the stack, though,
        // and at the first instruction of the callee it is exactly at RSP.
        if (ep->ContextRecord != nullptr && code == EXCEPTION_ACCESS_VIOLATION &&
            ep->ExceptionRecord->NumberParameters >= 1 &&
            ep->ExceptionRecord->ExceptionInformation[0] == 8)
        {
            const void *ret = nullptr;
            if (SafeReadPtr(reinterpret_cast<const void *const *>(ep->ContextRecord->Rsp), &ret) &&
                ret != nullptr)
            {
                wchar_t who[MAX_PATH] = L"";
                HMODULE rm = nullptr;
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       static_cast<LPCWSTR>(ret), &rm) && rm != nullptr)
                {
                    wchar_t path[MAX_PATH] = L"";
                    GetModuleFileNameW(rm, path, MAX_PATH);
                    const wchar_t *leaf = wcsrchr(path, L'\\');
                    swprintf_s(who, L"%s+0x%llX", leaf != nullptr ? leaf + 1 : path,
                               static_cast<unsigned long long>(
                                   reinterpret_cast<uintptr_t>(ret) - reinterpret_cast<uintptr_t>(rm)));
                }
                else if (!ModuleSnapshotNameAt(ret, who, MAX_PATH))
                    swprintf_s(who, L"%p (no module)", ret);
                Log("    the call came from %ls", who);
            }
        }
        for (USHORT i = 0; i < got; ++i)
        {
            wchar_t who[MAX_PATH] = L"";
            HMODULE fm = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCWSTR>(frames[i]), &fm) && fm != nullptr)
            {
                wchar_t path[MAX_PATH] = L"";
                GetModuleFileNameW(fm, path, MAX_PATH);
                const wchar_t *leaf = wcsrchr(path, L'\\');
                swprintf_s(who, L"%s+0x%llX", leaf != nullptr ? leaf + 1 : path,
                           static_cast<unsigned long long>(
                               reinterpret_cast<uintptr_t>(frames[i]) -
                               reinterpret_cast<uintptr_t>(fm)));
            }
            else if (!ModuleSnapshotNameAt(frames[i], who, MAX_PATH))
                swprintf_s(who, L"%p (no module)", frames[i]);
            Log("    %2u  %ls", i, who);
        }
    }
    Log("#####################################################");

    // Always chain: games install their own crash handlers and this must not
    // replace them.
    return g_prev_filter != nullptr ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}


// ---------------------------------------------------------------------------
// Inline hook
//
// 14-byte absolute jump, no trampoline: the original bytes are restored around
// the forwarded call and the patch is written back afterwards. That avoids
// needing a length disassembler to relocate the prologue.
//
// Every write re-acquires and then restores page protection rather than leaving
// the page writable. The D3D11 and D3D12 NGX entry points sit within a few
// hundred bytes of each other, so another add-on hooking the D3D12 side (RenoDX
// uses Detours there) shares this page and will reset its protection when it is
// done. Assuming the page stays writable would fault the moment that happens.
// ---------------------------------------------------------------------------

struct Hook
{
    void *target   = nullptr;
    void *original = nullptr;
    bool  active   = false;
};

// Several modules can export the NGX D3D11 API at the same time: a game or mod
// that links NGX, the driver's loader beneath it, and the feature snippet
// beneath that. Which one the caller actually enters is not knowable from the
// outside -- Baldur's Gate 3 calls its own exports, Skyrim's upscaler links NGX
// statically and calls straight into nvngx_dlss.dll, never touching the loader.
// Earlier builds tried to pick the right layer by name and got it wrong twice.
// Hook every layer instead and let the outermost call win; see g_nest below.
//
// Twelve because the same snippet can be mapped more than once: Baldur's Gate 3
// alone reaches six, with nvngx_dlss.dll appearing at two different bases.
static const int kMaxLayers = 12;

struct Layer
{
    HMODULE mod;
    Hook    eval;
    Hook    eval_c;
    Hook    create;

    // The Vulkan NGX API, hooked in the same layer slot and by the same
    // mechanism. CreateFeature1 is a third class rather than a variant of
    // CreateFeature: it is a distinct export at a distinct address and it
    // prepends a VkDevice, so a game reaching DLSS through it while only
    // CreateFeature is patched would be unobserved.
    Hook    vk_eval;
    Hook    vk_eval_c;
    Hook    vk_create;
    Hook    vk_create1;
    bool pending_retirement = false;
};

// Loader notifications only publish into these atomics. They never acquire
// bridge/MinHook locks, log or manipulate hooks; scheduling owns a DLL reference.
static PVOID volatile g_layer_modules[kMaxLayers] = {};
static volatile LONG g_layer_unloaded[kMaxLayers] = {};

// Whether the Vulkan mirror is switched on, read once at attach out of
// dlss5-bridge.cfg. It decides whether four foreign entry points get
// fourteen bytes written into them, so it is a launch-time decision and not a
// per-frame one. Kept out of BridgeCfg deliberately: bridge.inc is the file the
// nine shipped D3D11 titles run on, and its config report line stays byte for
// byte what those titles' logs already carry.
static int              g_vk_mirror;

static Layer            g_layer[kMaxLayers];
static volatile LONG    g_layer_count;

// Modules already examined and rejected. Without this the scan re-decides them
// on every library load and says so each time -- file writes under the loader
// lock, dozens of them during a startup, in a process the loader lock is
// serialising. Escape from Tarkov and The Elder Scrolls Online both showed the
// same line forty times before anything else happened.
// Set when the host executable's own NGX exports were left alone, so the idle
// diagnosis can name it rather than leaving the reader to guess.
static HMODULE          g_deferred_exe;
static bool             g_force_exe;

static HMODULE          g_rejected[64];
static volatile LONG    g_rejected_count;

static bool AlreadyRejected(HMODULE m)
{
    for (LONG i = 0; i < g_rejected_count; ++i)
        if (g_rejected[i] == m) return true;
    return false;
}

// The host executable is skipped first and hooked only if nothing else carries
// the calls. Undoing one rejection is what makes that reversible.
static void ForgetRejected(HMODULE m)
{
    for (LONG i = 0; i < g_rejected_count; ++i)
        if (g_rejected[i] == m)
        {
            g_rejected[i] = g_rejected[g_rejected_count - 1];
            g_rejected_count = g_rejected_count - 1;
            return;
        }
}

static void RememberRejected(HMODULE m)
{
    if (g_rejected_count < static_cast<LONG>(_countof(g_rejected)))
        g_rejected[g_rejected_count++] = m;
}
static CRITICAL_SECTION g_hook_cs;

// One NGX call at a time in this process, across both backends.
//
// The mirror's worker runs a D3D12 NGX evaluate while the game's render thread
// runs its own Vulkan NGX evaluate, and until now nothing kept the two apart:
// the worker takes no lock, and the render thread's g_hook_cs is released before
// VkMirrorFrame wakes it. Two threads inside one NGX, on Red Dead Redemption 2,
// fault -- and the evidence that it is a race rather than a bad contract is that
// it is not reproducible in shape. Measured 2026-08-31, same build, same game,
// same values: once after 89 delivered frames inside nvapi64_impl.dll, once
// after 145 inside nvoglv64.dll, with every per-frame parameter dumped at the
// fault and every one of them in range, and the private device reporting S_OK.
//
// Deliberately NOT g_hook_cs. That one is taken from the loader notification
// while the loader lock is held, and adding a second waiter on it is the
// deadlock the note in TryInstallHooks was written about.
static CRITICAL_SECTION g_ngx_cs;
static bool             g_ngx_cs_ready;

// Depth of NGX calls this thread is currently inside. Only the outermost is the
// caller's; anything nested is NGX talking to itself, with parameters it has
// already rewritten for its own use. Reading those as if they were the game's
// is what made 1.0.5 necessary.
static __declspec(thread) int g_nest;

struct NestGuard
{
    NestGuard()  { ++g_nest; }
    ~NestGuard() { --g_nest; }
};

static volatile LONG g_in_flight_trampoline_calls = 0;

struct TrampolineCallGuard
{
    TrampolineCallGuard()  { InterlockedIncrement(&g_in_flight_trampoline_calls); }
    ~TrampolineCallGuard() { InterlockedDecrement(&g_in_flight_trampoline_calls); }
};

static bool WaitForInFlightTrampolineCalls(DWORD maxWaitMs = 2000)
{
    DWORD waited = 0;
    while (InterlockedCompareExchange(&g_in_flight_trampoline_calls, 0, 0) > 0 && waited < maxWaitMs)
    {
        Sleep(1);
        waited += 1;
    }
    return InterlockedCompareExchange(&g_in_flight_trampoline_calls, 0, 0) == 0;
}

static const void *DetourTargetOf(const BYTE *p);
static bool        JumpsInsideOwnModule(const BYTE *p);

static bool HookInstall(Hook &h, void *target, void *detour)
{
    if (!target || !detour) return false;

    // An export that is a 5-byte jmp into its own module is one entry of a
    // thunk table, and the entries beside it are the module's own internal
    // calls. The hook goes where the thunk goes: an ordinary function with an
    // ordinary prologue.
    BYTE *at = static_cast<BYTE *>(target);
    for (int hop = 0; hop < 4 && at[0] == 0xE9 && JumpsInsideOwnModule(at); ++hop)
    {
        const void *next = DetourTargetOf(at);
        if (next == nullptr) break;
        Log("    %p is a thunk table entry; the hook goes to its destination %p so the "
            "patch does not overwrite the entries beside it.", at, next);
        at = static_cast<BYTE *>(const_cast<void *>(next));
    }
    h.target = at;

    if (!PinHookModule(reinterpret_cast<const void *>(&HookInstall))) return false;
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return false;
    MH_STATUS st = MH_CreateHook(at, detour, &h.original);
    if (st != MH_OK)
    {
        Log("    MH_CreateHook to %p failed with status %d", at, st);
        h.active = false;
        h.original = nullptr;
        return false;
    }

    st = MH_EnableHook(at);
    if (st != MH_OK)
    {
        for (int retry = 0; retry < 3 && st != MH_OK; ++retry)
        {
            Sleep(10);
            st = MH_EnableHook(at);
        }
    }
    if (st != MH_OK)
    {
        Log("    MH_EnableHook to %p failed with status %d (win32 error %lu)", at, st, GetLastError());
        MH_RemoveHook(at);
        h.active = false;
        h.original = nullptr;
        return false;
    }

    h.active = true;
    return true;
}

[[maybe_unused]] static void HookRemove(Hook &h)
{
    if (!h.active || !h.target) return;
    MH_DisableHook(h.target);
    h.active = false;
    if (!WaitForInFlightTrampolineCalls(2000))
    {
        Log("    [hook] trampoline wait timed out for %p (%ld in flight); "
            "leaving trampoline allocated to prevent use-after-free",
            h.target, InterlockedCompareExchange(&g_in_flight_trampoline_calls, 0, 0));
        return;
    }
    MH_RemoveHook(h.target);
    h.original = nullptr;
    h.target = nullptr;
}

static bool HookRetire(Hook &h)
{
    if (!h.target) return true;
    h.active = false;
    const MH_STATUS status = MH_RetireHook(h.target);
    if (status != MH_OK && status != MH_ERROR_NOT_CREATED) return false;
    h.original = nullptr;
    h.target = nullptr;
    return true;
}

static bool RetireLayerHooks(Layer &layer)
{
    Hook *hooks[] = {&layer.eval, &layer.eval_c, &layer.create, &layer.vk_eval,
                    &layer.vk_eval_c, &layer.vk_create, &layer.vk_create1};
    bool retired = true;
    for (Hook *hook : hooks) retired = HookRetire(*hook) && retired;
    return retired;
}

// ---------------------------------------------------------------------------
// Parameter dumping
// ---------------------------------------------------------------------------

static const char *FormatName(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:    return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
    case DXGI_FORMAT_R11G11B10_FLOAT:       return "R11G11B10_FLOAT";
    case DXGI_FORMAT_R10G10B10A2_UNORM:     return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return "R10G10B10A2_TYPELESS";
    case DXGI_FORMAT_R8G8B8A8_UNORM:        return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return "R8G8B8A8_TYPELESS";
    case DXGI_FORMAT_R32G32B32A32_FLOAT:    return "R32G32B32A32_FLOAT";
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return "R32G32B32A32_TYPELESS";
    case DXGI_FORMAT_R16_FLOAT:             return "R16_FLOAT";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM:        return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return "B8G8R8A8_TYPELESS";
    case DXGI_FORMAT_R16G16_FLOAT:          return "R16G16_FLOAT";
    case DXGI_FORMAT_R16G16_TYPELESS:       return "R16G16_TYPELESS";
    case DXGI_FORMAT_R32_FLOAT:             return "R32_FLOAT";
    // The Vulkan mirror's depth slot when the game's depth aspect arrives packed.
    // Without this line the one message that reports failing to create it says
    // "unnamed", which is a refusal that cannot name what it refused.
    case DXGI_FORMAT_R32_UINT:              return "R32_UINT";
    case DXGI_FORMAT_R32_TYPELESS:          return "R32_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT:             return "D32_FLOAT";
    case DXGI_FORMAT_R24G8_TYPELESS:        return "R24G8_TYPELESS";
    case DXGI_FORMAT_R32G8X24_TYPELESS:     return "R32G8X24_TYPELESS";
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return "R24_UNORM_X8_TYPELESS";
    case DXGI_FORMAT_R16_UNORM:             return "R16_UNORM";
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:  return "D32_FLOAT_S8X24_UINT";
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return "R32_FLOAT_X8X24_TYPELESS";
    case DXGI_FORMAT_R16_TYPELESS:          return "R16_TYPELESS";
    case DXGI_FORMAT_D16_UNORM:             return "D16_UNORM";
    case DXGI_FORMAT_D24_UNORM_S8_UINT:     return "D24_UNORM_S8_UINT";
    case DXGI_FORMAT_R8_UNORM:              return "R8_UNORM";
    default:                                return "unnamed";
    }
}

// A typeless texture carries no interpretation of its bits, so nothing can build
// a shader resource view or a typed UAV over it. The bridge used to give its
// shared copies whatever format the game used, typeless included, and a DLSS 5
// add-on then refused the result: "DLSS output format 1 is not a supported typed
// codec format". Escape from Tarkov hands DLSS an R32G32B32A32_TYPELESS colour
// buffer and an R16G16_TYPELESS motion vector buffer, so the bridge delivered
// frames perfectly and the neural pass never ran on any of them.
//
// The copy is given the natural typed format of the same typeless family.
// CopyResource checks the family, not the exact format, so it stays legal in
// both directions, and the far side gets something it can actually view.
static DXGI_FORMAT TypedFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R32G32B32_TYPELESS:    return DXGI_FORMAT_R32G32B32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:       return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:     return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS:       return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS:          return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R8G8_TYPELESS:         return DXGI_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R16_TYPELESS:          return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R8_TYPELESS:           return DXGI_FORMAT_R8_UNORM;
    default:                                return f;
    }
}

// The decisive field for a zero-copy bridge. Anything other than "none" means
// the texture can be opened on a second device without an intermediate copy.
static const char *ShareText(UINT misc)
{
    if (misc & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)    return "  <== SHARED_NTHANDLE";
    if (misc & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)  return "  <== SHARED_KEYEDMUTEX";
    if (misc & D3D11_RESOURCE_MISC_SHARED)             return "  <== SHARED";
    return "  (not shared)";
}

// Defined in bridge.inc, included further down.
static const char *NgxResultName(NVSDK_NGX_Result r);

static void DumpTexture(const NVSDK_NGX_Parameter *p, const char *key)
{
    ID3D11Resource *res = nullptr;
    NVSDK_NGX_Result r = p->Get(key, &res);
    if (r != NGX_SUCCESS || res == nullptr)
    {
        // Printed by name and in hex. As a signed decimal this read
        // -1160773616 in every log ever collected, which is 0xBAD00010,
        // UnsupportedParameter -- and nobody recognised it as a result code.
        // A recognised key the game left empty and a key this runtime has never
        // heard of are different answers, and "absent (Success)" read as neither.
        if (r == NGX_SUCCESS)
            Log("    %-34s  not set by the game", key);
        else
            Log("    %-34s  no such key here (%s, 0x%08X)", key, NgxResultName(r), r);
        return;
    }

    ID3D11Texture2D *tex = nullptr;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex))) &&
        tex != nullptr)
    {
        D3D11_TEXTURE2D_DESC d;
        tex->GetDesc(&d);
        Log("    %-22s  %p  %ux%u  fmt=%u %s  mips=%u arr=%u samp=%u usage=%u "
            "bind=0x%X cpu=0x%X misc=0x%X%s",
            key, static_cast<void *>(res), d.Width, d.Height, d.Format, FormatName(d.Format),
            d.MipLevels, d.ArraySize, d.SampleDesc.Count, d.Usage, d.BindFlags,
            d.CPUAccessFlags, d.MiscFlags, ShareText(d.MiscFlags));
        tex->Release();
    }
    else
    {
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        res->GetType(&dim);
        Log("    %-22s  %p  not a Texture2D (dimension=%d)", key, static_cast<void *>(res), dim);
    }
}

static void DumpUInt(const NVSDK_NGX_Parameter *p, const char *key)
{
    unsigned int v = 0;
    if (p->Get(key, &v) == NGX_SUCCESS)
        Log("    %-40s = %u", key, v);
}

static void DumpInt(const NVSDK_NGX_Parameter *p, const char *key)
{
    int v = 0;
    if (p->Get(key, &v) == NGX_SUCCESS)
        Log("    %-40s = %d", key, v);
}

static void DumpFloat(const NVSDK_NGX_Parameter *p, const char *key)
{
    float v = 0.0f;
    if (p->Get(key, &v) == NGX_SUCCESS)
        Log("    %-40s = %.6f", key, static_cast<double>(v));
}

// A loaded module that exports the NGX entry points but is neither the driver's
// loader nor an nvngx_* snippet. Empty when there is none.
static wchar_t g_ngx_proxy[MAX_PATH];

// The running NVIDIA driver as 56094 for 560.94, or 0 on a non-NVIDIA adapter.
// Read at the first DLSS call, used again when the capability table is dumped.
static unsigned g_nv_driver;

// The adapter and driver were only recorded when the D3D12 session opened, so a
// game that dies before that -- The Elder Scrolls Online does -- left a report
// with no machine in it at all. The game's own D3D11 device knows the adapter,
// and by the first DLSS call it certainly exists.
static void LogAdapterOnce(ID3D11Device *dev)
{
    static bool done = false;
    if (done || dev == nullptr) return;
    done = true;

    IDXGIDevice  *dxgi = nullptr;
    IDXGIAdapter *ad   = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice),
                                   reinterpret_cast<void **>(&dxgi))) || dxgi == nullptr)
        return;
    dxgi->GetAdapter(&ad);
    dxgi->Release();
    if (ad == nullptr) return;

    DXGI_ADAPTER_DESC d = {};
    ad->GetDesc(&d);
    Log("  adapter: %ls  vram=%llu MB  vendor 0x%04X device 0x%04X subsys 0x%08X rev %u",
        d.Description,
        static_cast<unsigned long long>(d.DedicatedVideoMemory / (1024 * 1024)),
        d.VendorId, d.DeviceId, d.SubSysId, d.Revision);

    LARGE_INTEGER umd = {};
    if (SUCCEEDED(ad->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
    {
        const unsigned f3 = static_cast<unsigned>((umd.LowPart >> 16) & 0xFFFF);
        const unsigned f4 = static_cast<unsigned>(umd.LowPart & 0xFFFF);
        Log("  driver: %u.%u.%u.%u",
            static_cast<unsigned>((umd.HighPart >> 16) & 0xFFFF),
            static_cast<unsigned>(umd.HighPart & 0xFFFF), f3, f4);

        // NVIDIA's own version is the last digit of the third field followed by
        // the fourth: 32.0.15.6094 is 560.94 and 32.0.16.1656 is 616.56, both
        // confirmed against the version ReShade prints in the same two logs.
        // Reports arrive with the Windows number, discussions use NVIDIA's, and
        // nothing here translated between them.
        if (d.VendorId == 0x10DE)
        {
            const unsigned nv = (f3 % 10u) * 10000u + f4;
            g_nv_driver = nv;
            Log("  NVIDIA driver %u.%02u", nv / 100u, nv % 100u);

            // Bumped when a release is verified on a newer driver. Older is not
            // a fault by itself -- it is the first thing worth ruling out when
            // an NGX feature will not create, and it is the one difference a
            // reader cannot see without knowing what this was tested against.
            const unsigned kVerifiedOn = 61656u;   // 616.56
            if (nv < kVerifiedOn)
                Log("  this driver is older than the %u.%02u this add-on is verified on. "
                    "DLSS itself works far back, but neural rendering does not: Final "
                    "Fantasy XIV on 560.94 could not create the feature and updating the "
                    "driver fixed it. Update before investigating anything else.",
                    kVerifiedOn / 100u, kVerifiedOn % 100u);
        }
    }
    ad->Release();
}

static void DumpContext(ID3D11DeviceContext *ctx)
{
    if (ctx == nullptr) { Log("    context                 = null"); return; }

    D3D11_DEVICE_CONTEXT_TYPE t = ctx->GetType();
    Log("    context                 = %p  type=%s", static_cast<void *>(ctx),
        t == D3D11_DEVICE_CONTEXT_IMMEDIATE ? "IMMEDIATE" : "DEFERRED  <== blocks a simple bridge");

    ID3D11Device *dev = nullptr;
    ctx->GetDevice(&dev);
    if (dev == nullptr) return;

    Log("    device                  = %p  feature_level=0x%04X",
        static_cast<void *>(dev), dev->GetFeatureLevel());

    // ID3D11Device5 is required to create a fence that can be shared with a
    // D3D12 queue, which any copy-based bridge would need.
    ID3D11Device5 *dev5 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void **>(&dev5))) &&
        dev5 != nullptr)
    {
        Log("    ID3D11Device5           = yes (shared fences available)");
        dev5->Release();
    }
    else
    {
        Log("    ID3D11Device5           = NO (no shared fence support)");
    }

    dev->Release();
}

static void DumpEvaluate(ID3D11DeviceContext *ctx, const NVSDK_NGX_Handle *handle,
                         const NVSDK_NGX_Parameter *p, long n)
{
    Log("--- EvaluateFeature #%ld  handle=%p params=%p  thread %lu ---", n,
        static_cast<const void *>(handle), static_cast<const void *>(p),
        GetCurrentThreadId());

    DumpContext(ctx);
    if (p == nullptr) { Log("    params is null"); return; }

    Log("  resources:");
    DumpTexture(p, "Color");
    DumpTexture(p, "Output");
    DumpTexture(p, "Depth");
    DumpTexture(p, "MotionVectors");
    DumpTexture(p, "ExposureTexture");
    DumpTexture(p, "TransparencyMask");

    // "BiasCurrentColorMask" is not an NGX key. The header spells it
    // DLSS.Input.Bias.Current.Color.Mask, so every "absent" this line has
    // reported since it was written was a probe of a name nothing sets: whether
    // any title supplies a bias mask is still unknown. The reactive mask is what
    // a game marks its unstable pixels with -- particles, muzzle flashes,
    // water -- and a DLSS that never receives it accumulates history over them.
    DumpTexture(p, "DLSS.Input.Bias.Current.Color.Mask");

    // The transparency layer arrived in a later SDK than the mask above and is
    // probed for the same reason: to find out whether anything sets it before
    // deciding whether it is worth mirroring.
    DumpTexture(p, "DLSS.TransparencyLayer");
    DumpTexture(p, "DLSS.TransparencyLayerOpacity");
    DumpTexture(p, "DLSS.TransparencyLayerMvecs");

    // Set by Ray Reconstruction and by nothing else. If one of these is present
    // the evaluate is a denoise, not an upscale, and the bridge stands aside for
    // it -- so a log that shows them also shows why nothing was mirrored.
    DumpTexture(p, "NormalRoughness");
    DumpTexture(p, "DiffuseAlbedo");
    DumpTexture(p, "SpecularAlbedo");
    DumpTexture(p, "SpecularHitDistance");

    Log("  scalars:");
    DumpUInt(p, "Width");
    DumpUInt(p, "Height");
    DumpUInt(p, "OutWidth");
    DumpUInt(p, "OutHeight");
    DumpUInt(p, "DLSS.Render.Subrect.Dimensions.Width");
    DumpUInt(p, "DLSS.Render.Subrect.Dimensions.Height");
    DumpUInt(p, "DLSS.Input.Color.Subrect.Base.X");
    DumpUInt(p, "DLSS.Input.Color.Subrect.Base.Y");
    DumpUInt(p, "DLSS.Input.Depth.Subrect.Base.X");
    DumpUInt(p, "DLSS.Input.Depth.Subrect.Base.Y");
    DumpUInt(p, "DLSS.Input.MV.Subrect.Base.X");
    DumpUInt(p, "DLSS.Input.MV.Subrect.Base.Y");
    DumpUInt(p, "DLSS.Output.Subrect.Base.X");
    DumpUInt(p, "DLSS.Output.Subrect.Base.Y");
    DumpFloat(p, "MV.Scale.X");
    DumpFloat(p, "MV.Scale.Y");
    DumpFloat(p, "Jitter.Offset.X");
    DumpFloat(p, "Jitter.Offset.Y");
    DumpFloat(p, "Sharpness");
    DumpFloat(p, "DLSS.Pre.Exposure");
    DumpFloat(p, "DLSS.Exposure.Scale");
    DumpInt(p, "Reset");
    DumpInt(p, "DLSS.Feature.Create.Flags");
    DumpInt(p, "PerfQualityValue");
}

// ---------------------------------------------------------------------------
// D3D12 NGX feasibility test
//
// Runs once. Asks the single question that decides whether a D3D11 -> D3D12 NGX
// bridge is possible at all: can a second NGX session be initialised on a D3D12
// device inside a process where NGX is already live on D3D11, and can a DLSS
// feature be created on it?
//
// If this fails, no bridge architecture -- copy-based or d3d11on12 -- can work,
// because both end at the same NVSDK_NGX_D3D12_CreateFeature call.
//
// Side benefit: that call is the one RenoDX's DLSS 5 add-on detours, so when the
// add-on is present its own log lines reveal whether it engages.
// ---------------------------------------------------------------------------

typedef HRESULT (WINAPI *PFN_D3D12CreateDevice_)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);

// Argument order taken from the disassembly of _nvngx.dll, not from memory.
// In Init_Ext the fourth argument is read as a 32-bit value (mov ebp,r9d) and
// the fifth as a qword, so the version precedes the FeatureCommonInfo pointer.
// Init_ProjectID follows the same pattern in its stack arguments.
typedef NVSDK_NGX_Result (*PFN_Init_ProjectID)(const char *, int, const char *, const wchar_t *,
                                               ID3D12Device *, int, const void *);
typedef NVSDK_NGX_Result (*PFN_Init_Ext)(unsigned long long, const wchar_t *, ID3D12Device *,
                                         int, const void *);
typedef NVSDK_NGX_Result (*PFN_GetCapabilityParameters)(NVSDK_NGX_Parameter **);
typedef NVSDK_NGX_Result (*PFN_AllocateParameters)(NVSDK_NGX_Parameter **);
typedef NVSDK_NGX_Result (*PFN_D3D12CreateFeature)(ID3D12GraphicsCommandList *, int,
                                                   NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
typedef NVSDK_NGX_Result (*PFN_D3D12EvaluateFeature)(ID3D12GraphicsCommandList *,
                                                     const NVSDK_NGX_Handle *,
                                                     const NVSDK_NGX_Parameter *, void *);
typedef NVSDK_NGX_Result (*PFN_D3D12ReleaseFeature)(NVSDK_NGX_Handle *);

#include "bridge.h"

static volatile LONG g_probe_done = 0;

typedef BOOL (WINAPI *PFN_EnumProcessModules)(HANDLE, HMODULE *, DWORD, LPDWORD);

// A game that ships DLSS has already loaded the driver's NGX loader long before
// this add-on looks for it -- the game's own NVSDK_NGX_D3D11_Init pulls it in.
// A game that ships no DLSS never does, and the synthetic contract is for
// exactly those games: Skyrim Special Edition on 2026-08-30 armed the synthetic
// path, built the contract, opened the D3D12 session and then found nothing
// exporting NVSDK_NGX_D3D12_Init_Ext, because nobody had loaded NGX.
//
// The driver publishes where it lives, for this purpose. NGXCore\FullPath is the
// directory the loader and its snippets sit in, and it is what NGX's own
// bootstrap reads. Loading it from anywhere else would be guessing at a path
// under DriverStore that changes with every driver.
//
// Called only from the synthetic path. The mirror path must NOT do this: there,
// nothing exporting NGX means the game never called DLSS, which is a fact worth
// reporting rather than one to paper over by loading NGX on the game's behalf.
static bool EnsureNgxLoaderLoaded()
{
    static bool tried;
    static bool ok;
    if (tried) return ok;
    tried = true;

    wchar_t dir[MAX_PATH] = {};
    DWORD   cb = sizeof(dir);
    const LSTATUS r = RegGetValueW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore",
        L"FullPath", RRF_RT_REG_SZ, nullptr, dir, &cb);
    if (r != ERROR_SUCCESS || dir[0] == 0)
    {
        Log("[bridge] NGXCore\\FullPath is not in the registry (%ld), so the driver's NGX "
            "loader cannot be found. On an NVIDIA driver that supports DLSS it is always "
            "there; its absence means either no NVIDIA driver or one too old.", r);
        return false;
    }

    wchar_t path[MAX_PATH] = {};
    _snwprintf_s(path, _TRUNCATE, L"%ls\\_nvngx.dll", dir);
    const HMODULE m = LoadLibraryW(path);
    if (m == nullptr)
    {
        Log("[bridge] the driver's NGX loader is registered at %ls but did not load (%lu).",
            path, GetLastError());
        return false;
    }
    Log("[bridge] the game loaded no NGX of its own, so the driver's loader was loaded "
        "directly from %ls. This happens only on the synthetic path: a game with its own "
        "DLSS has already brought NGX in.", path);
    ok = true;
    return true;
}

static HMODULE FindNgxLoader()
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto enum_modules =
        reinterpret_cast<PFN_EnumProcessModules>(GetProcAddress(k32, "K32EnumProcessModules"));
    if (enum_modules == nullptr) return nullptr;

    HMODULE mods[1024];
    DWORD   needed = 0;
    if (!enum_modules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return nullptr;

    // Taking the first module that exports these was wrong. Prey 2017 loads a
    // third-party WINMM.dll that exports the NGX D3D12 entry points too, it is
    // enumerated before the driver's own loader because it comes from the game
    // folder, and calling its Init_Ext faulted -- while the DLSS 5 add-on had
    // meanwhile hooked the driver's copy, so the two were not even talking about
    // the same NGX. NVIDIA's loader is named _nvngx.dll wherever it lives, so
    // that name wins; a game-folder proxy under the same name still wins over an
    // unrelated wrapper, which is right, because that is the one the game reaches.
    HMODULE first = nullptr;
    g_ngx_proxy[0] = 0;
    for (int pass = 0; pass < 2; ++pass)
    {
        for (DWORD i = 0; i < needed / sizeof(HMODULE); ++i)
        {
            if (GetProcAddress(mods[i], "NVSDK_NGX_D3D12_Init_ProjectID") == nullptr ||
                GetProcAddress(mods[i], "NVSDK_NGX_D3D12_CreateFeature") == nullptr)
                continue;

            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(mods[i], path, MAX_PATH);
            const wchar_t *leaf = wcsrchr(path, (wchar_t)92);   // backslash
            leaf = leaf != nullptr ? leaf + 1 : path;

            if (pass == 0)
            {
                if (_wcsicmp(leaf, L"_nvngx.dll") != 0)
                {
                    if (first == nullptr) first = mods[i];
                    // Remembered so a later failure can name it. A module that
                    // is neither the driver's loader nor an nvngx_* snippet, yet
                    // exports the NGX entry points, is a proxy standing in for
                    // NGX -- OptiScaler installed as winmm.dll is one, and it
                    // patches code inside the driver's module as well.
                    if (g_ngx_proxy[0] == 0 && _wcsnicmp(leaf, L"nvngx", 5) != 0)
                        wcsncpy_s(g_ngx_proxy, path, _TRUNCATE);
                    continue;
                }
                Log("[bridge] NGX D3D12 entry points taken from %ls", path);
                return mods[i];
            }
            // Second pass: no driver loader is present under its own name.
            Log("[bridge] no _nvngx.dll is loaded; NGX D3D12 entry points taken from "
                "%ls instead. A module that is not NVIDIA's loader exporting these is "
                "worth noting if the session then fails.", path);
            return mods[i];
        }
        if (pass == 0 && first == nullptr) break;
    }
    return first;
}

// Every NGX entry point below is called through a guarded wrapper. These are
// undocumented exports reached with signatures recovered from disassembly; a
// wrong guess must land in the log, not take the game down. Each wrapper is its
// own function with no C++ objects so __try is legal and no unwinding is needed.
#define NGX_EXCEPTION_MARKER 0x7FFFFFFF

// Where the last guarded call faulted, and whose module owns that address.
// GetExceptionCode() alone gave every fault the same log line, so three
// competing explanations for Prey 2017 all produced identical evidence. The
// filter runs before the stack unwinds, which is the only place the address is
// available. NameFaultOwner uses the same lookup the crash reporter does.
static void *g_last_fault_at;

// The return addresses as well, and the two words an access violation carries.
// The address alone named the victim and never the caller, which on 2026-09-04
// left the one question that mattered unanswerable: an access violation inside
// D3D12Core's SetDescriptorHeaps, validating a descriptor heap that is not one,
// with three candidate callers in a neighbouring add-on and no way to tell them
// apart. The crash reporter has walked the stack in its own filter since 1.0;
// this one is the same walk in the guarded-call filter, which runs on the
// faulting thread before anything unwinds, so its stack IS that thread's.
static void *g_last_fault_frames[24];
static USHORT g_last_fault_n;
static ULONG_PTR g_last_fault_op;    // 0 read, 1 write, 8 execute
static ULONG_PTR g_last_fault_addr;  // the address the faulting access touched
static bool g_last_fault_is_av;

static int CaptureFault(EXCEPTION_POINTERS *ep)
{
    g_last_fault_at = ep != nullptr && ep->ExceptionRecord != nullptr
                    ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    g_last_fault_n  = RtlCaptureStackBackTrace(0, 24, g_last_fault_frames, nullptr);
    g_last_fault_is_av = false;
    if (ep != nullptr && ep->ExceptionRecord != nullptr &&
        ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2)
    {
        g_last_fault_is_av = true;
        g_last_fault_op    = ep->ExceptionRecord->ExceptionInformation[0];
        g_last_fault_addr  = ep->ExceptionRecord->ExceptionInformation[1];
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// Printed beside the "it faulted in <module>" line every caller already logs.
// Module and offset rather than an absolute address, for the same reason that
// line gives: an address changes every run and means nothing to whoever owns
// the module it lands in.
static void LogFaultStack(const char *tag)
{
    if (g_last_fault_is_av)
        Log("%s   the access was a %s of %p", tag,
            g_last_fault_op == 1 ? "write" : g_last_fault_op == 8 ? "execute" : "read",
            reinterpret_cast<void *>(g_last_fault_addr));
    if (g_last_fault_n == 0) return;
    Log("%s   called from (thread %lu):", tag, GetCurrentThreadId());
    for (USHORT i = 0; i < g_last_fault_n; ++i)
    {
        HMODULE mod = nullptr;
        wchar_t path[MAX_PATH] = {};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCWSTR>(g_last_fault_frames[i]), &mod) &&
            mod != nullptr && GetModuleFileNameW(mod, path, MAX_PATH) != 0)
        {
            const wchar_t *leaf = wcsrchr(path, L'\\');
            Log("%s   %2u  %ls+0x%llX", tag, i, leaf != nullptr ? leaf + 1 : path,
                static_cast<unsigned long long>(
                    static_cast<const unsigned char *>(g_last_fault_frames[i]) -
                    reinterpret_cast<const unsigned char *>(mod)));
        }
        else
        {
            Log("%s   %2u  %p, in no loaded module", tag, i, g_last_fault_frames[i]);
        }
    }
}

// Fills out with the module owning the last fault, or a description of why no
// module owns it. Returns false when there was no address to resolve.
static bool NameFaultOwner(wchar_t *out, size_t n)
{
    if (g_last_fault_at == nullptr) return false;
    HMODULE mod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(g_last_fault_at), &mod) && mod != nullptr)
    {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(mod, path, MAX_PATH);
        // The offset is what a symbol file can be matched against; the absolute
        // address changes every run and is useless to the module's author.
        _snwprintf_s(out, n, _TRUNCATE, L"%ls +0x%llX", path,
                     static_cast<unsigned long long>(
                         static_cast<const unsigned char *>(g_last_fault_at) -
                         reinterpret_cast<const unsigned char *>(mod)));
        return true;
    }

    // No owning module is itself an answer: code that has been unloaded, or a
    // page mapped without being registered as a module, both land here.
    wcsncpy_s(out, n, L"no loaded module owns that address", _TRUNCATE);
    return true;
}

static NVSDK_NGX_Result SafeInitExt(PFN_Init_Ext fn, unsigned long long app_id,
                                    const wchar_t *path, ID3D12Device *dev, int ver, DWORD *code)
{
    *code = 0;
    __try { return fn(app_id, path, dev, ver, nullptr); }
    __except (CaptureFault(GetExceptionInformation()))
    { *code = GetExceptionCode(); return NGX_EXCEPTION_MARKER; }
}

static NVSDK_NGX_Result SafeInitProjectID(PFN_Init_ProjectID fn, const char *project,
                                          const wchar_t *path, ID3D12Device *dev, int ver,
                                          DWORD *code)
{
    *code = 0;
    __try { return fn(project, 0 /* ENGINE_TYPE_CUSTOM */, "1.0", path, dev, ver, nullptr); }
    __except (CaptureFault(GetExceptionInformation()))
    { *code = GetExceptionCode(); return NGX_EXCEPTION_MARKER; }
}


static NVSDK_NGX_Result SafeAllocParams(PFN_AllocateParameters fn, NVSDK_NGX_Parameter **out, DWORD *code)
{
    *code = 0;
    __try { return fn(out); }
    __except (CaptureFault(GetExceptionInformation()))
    { *code = GetExceptionCode(); return NGX_EXCEPTION_MARKER; }
}

static NVSDK_NGX_Result SafeGetCaps(PFN_GetCapabilityParameters fn, NVSDK_NGX_Parameter **out, DWORD *code)
{
    *code = 0;
    __try { return fn(out); }
    __except (CaptureFault(GetExceptionInformation()))
    { *code = GetExceptionCode(); return NGX_EXCEPTION_MARKER; }
}

static NVSDK_NGX_Result SafeCreateFeature(PFN_D3D12CreateFeature fn, ID3D12GraphicsCommandList *list,
                                          NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **out, DWORD *code)
{
    *code = 0;
    __try { return fn(list, 1 /* SuperSampling */, p, out); }
    __except (CaptureFault(GetExceptionInformation()))
    { *code = GetExceptionCode(); return NGX_EXCEPTION_MARKER; }
}



// A detoured entry point no longer starts with its own prologue. Comparing what
// is actually at the function address against the untouched bytes shows whether
// another add-on's hook sits in front of the call this probe is about to make.
// Only PODs, so __try is legal here. A RIP-relative indirect jump reads a slot
// this side did not write, and a hook whose table has been unmapped would fault
// on a diagnostic line, which is the worst place to take a process down.
static bool SafeReadPtr(const void *const *slot, const void **out)
{
    __try { *out = *slot; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Where a jump at the entry actually goes, and who owns that address.
//
// "<== DETOURED" on its own has cost two issue threads a round trip each --
// Batman: Arkham Knight (#7) shows both D3D12 NGX entry points hooked and
// Final Fantasy XV (#11) shows D3D12CreateDevice hooked, and neither log says
// by whom. The target of an E9 is arithmetic and the module owning it is one
// call, so the question was answerable all along and simply was not asked.
// Where a jump at a function's entry actually goes. Three encodings, one place,
// because two callers print it and a third has to compare it.
static const void *DetourTargetOf(const BYTE *p)
{
    const void *target = nullptr;
    if (p[0] == 0xE9)
    {
        int rel = 0;
        memcpy(&rel, p + 1, sizeof(rel));
        target = p + 5 + rel;
    }
    else if (p[0] == 0xFF && p[1] == 0x25)
    {
        int rel = 0;
        memcpy(&rel, p + 2, sizeof(rel));
        if (!SafeReadPtr(reinterpret_cast<const void *const *>(p + 6 + rel), &target))
            return nullptr;
    }
    else if (p[0] == 0x48 && p[1] == 0xB8)
    {
        unsigned long long imm = 0;
        memcpy(&imm, p + 2, sizeof(imm));
        target = reinterpret_cast<const void *>(imm);
    }
    return target;
}

// A jump at an entry point is not necessarily a hook. A module whose export is a
// thunk jumps into its OWN code, and nvngx_dlss.dll does exactly that for
// NVSDK_NGX_VULKAN_CreateFeature1: E9 into the same module, 0xEB5 back.
//
// That was reported as "already hooked by something else" in every log, with no
// hook anywhere in the process -- measured on 2026-09-01 with one add-on loaded
// and no injector. Two issue threads have already spent a round trip on this
// line, so a false one is expensive.
static bool JumpsInsideOwnModule(const BYTE *p)
{
    const void *target = DetourTargetOf(p);
    if (target == nullptr) return false;
    HMODULE here = nullptr, there = nullptr;
    const DWORD f = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (!GetModuleHandleExW(f, reinterpret_cast<LPCWSTR>(p), &here) || here == nullptr)
        return false;
    if (!GetModuleHandleExW(f, static_cast<LPCWSTR>(target), &there) || there == nullptr)
        return false;
    return here == there;
}

static void LogDetourTarget(const BYTE *p)
{
    const void *target = DetourTargetOf(p);
    if (target == nullptr) return;

    HMODULE mod = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(target), &mod) != 0 &&
        mod != nullptr && GetModuleFileNameW(mod, path, MAX_PATH) != 0)
    {
        // As an OFFSET as well as an address: an absolute address changes every
        // launch and means nothing to whoever owns the module.
        const unsigned long long off =
            static_cast<unsigned long long>(static_cast<const BYTE *>(target) -
                                            reinterpret_cast<const BYTE *>(mod));
        Log("      the hook jumps into %ls +0x%llX", path, off);
    }
    else
    {
        Log("      the hook jumps to %p, which is in no loaded module -- so it is "
            "a trampoline somebody allocated rather than a function in a DLL.", target);
    }
}

static void LogEntryBytes(const char *label, void *fn)
{
    if (fn == nullptr) { Log("    %-34s <not exported>", label); return; }
    const BYTE *p = static_cast<const BYTE *>(fn);
    char hex[64];
    int  n = 0;
    for (int i = 0; i < 14; ++i)
        n += _snprintf_s(hex + n, sizeof(hex) - n, _TRUNCATE, "%02X ", p[i]);
    const bool detoured = (p[0] == 0xE9) || (p[0] == 0xFF && p[1] == 0x25) ||
                          (p[0] == 0x48 && p[1] == 0xB8) || (p[0] == 0xEB);
    const bool thunk = detoured && JumpsInsideOwnModule(p);
    Log("    %-34s %p  %s %s", label, fn, hex,
        thunk ? " <== a thunk into its own module, not a hook"
              : detoured ? " <== DETOURED" : "");
    if (detoured) LogDetourTarget(p);
}

// Defined in bridge.inc, which is included below this point.
static const char *NgxResultName(NVSDK_NGX_Result r);

// Defined below the include; bridge.inc calls it when the D3D12 session opens.
static void WarnIfOldCopyLoaded();

// What the game presents, as ReShade sees it, and what the bridge measured of
// its own output. Written by the effect-runtime path and the readback, read by
// the log at feature build and by the panel. A screenshot of the panel then
// says HDR or SDR, which colour space, the exposure the game supplies and how
// bright the output came back relative to the input -- the four facts the
// Odyssey report (#10) took three rounds to establish.
static volatile LONG g_present_space  = 0;     // reshade::api::color_space, 0 unknown
static volatile LONG g_present_flags  = -1;    // create flags of the last feature built
static float         g_present_expo   = 0.0f;  // the game's 1x1 exposure texture, 0 = none
static float         g_bright_in      = 0.0f;  // luma of the input colour at the last readback
static float         g_bright_out     = 0.0f;  // luma of the output at the last readback
static const char *ColorSpaceName(LONG cs)
{
    switch (cs)
    {
    case 1:  return "sRGB (gamma)";
    case 2:  return "scRGB (linear)";
    case 3:  return "HDR10 (PQ)";
    case 4:  return "HLG";
    default: return "unknown";
    }
}

// Every add-on beside this one that is loaded right now, with its base. The
// inventory at attach prints a base only for add-ons ReShade had already
// loaded, and it loads them in name order, so anything after "dlss5" has none
// there. A game's own crash log names addresses inside those modules (#22),
// and without a base nobody can turn them into offsets. Printed when the
// private D3D12 device is created, by which time every add-on is in.
static void LogAddonBases()
{
    wchar_t dir[MAX_PATH] = {};
    GetModuleFileNameW(g_self, dir, MAX_PATH);
    wchar_t *sl = wcsrchr(dir, L'\\');
    if (sl == nullptr) return;
    *(sl + 1) = 0;
    wchar_t pattern[MAX_PATH];
    _snwprintf_s(pattern, _TRUNCATE, L"%ls*.addon*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    Log("[bridge] add-ons loaded at this point, with their bases:");
    do
    {
        HMODULE m = GetModuleHandleW(fd.cFileName);
        if (m != nullptr) Log("[bridge]   %ls at %p", fd.cFileName, static_cast<void *>(m));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Set by the add-on inventory at attach, read where the private D3D12 device is
// created. True when the DLSS 5 add-on beside us is a build measured to need
// ReShade's proxy around that device -- see kKnownConsumers.
static bool g_consumer_needs_proxy;

// Filled by RegisterWithReShade, printed by the banner: see the note there for
// why it cannot be logged where it is discovered.
static wchar_t g_reshade_path[MAX_PATH];
static DWORD   g_reshade_others;

static void DumpCapability(NVSDK_NGX_Parameter *caps)
{
    static const char *int_keys[] = {
        "SuperSampling.Available",
        "SuperSampling.NeedsUpdatedDriver",
        "SuperSampling.MinDriverVersionMajor",
        "SuperSampling.MinDriverVersionMinor",
        "SuperSamplingDenoising.Available",
        "SuperSamplingDenoising.NeedsUpdatedDriver",
        "SuperSamplingDenoising.MinDriverVersionMajor",
        "SuperSamplingDenoising.MinDriverVersionMinor",
    };
    bool denoising_answered = false;
    int  denoising_available = 0;
    int  min_major = 0, min_minor = 0;
    for (const char *k : int_keys)
    {
        int v = 0;
        NVSDK_NGX_Result r = caps->Get(k, &v);
        if (r == NGX_SUCCESS) Log("      %-44s = %d", k, v);
        else                  Log("      %-44s   query failed 0x%08X, %s", k, r,
                                  NgxResultName(r));

        if (_stricmp(k, "SuperSamplingDenoising.Available") == 0)
        {
            denoising_answered = (r == NGX_SUCCESS);
            denoising_available = v;
        }
        else if (r == NGX_SUCCESS &&
                 _stricmp(k, "SuperSamplingDenoising.MinDriverVersionMajor") == 0)
            min_major = v;
        else if (r == NGX_SUCCESS &&
                 _stricmp(k, "SuperSamplingDenoising.MinDriverVersionMinor") == 0)
            min_minor = v;
    }

    // This minimum belongs to SuperSamplingDenoising -- Ray Reconstruction, a
    // 2023 feature -- and NOT to the neural-rendering feature a DLSS 5 add-on
    // creates, which NGX exposes no capability key for at all. Being above this
    // number says nothing about that one, and reading it as though it did is
    // what led an analysis to clear a driver that was in fact the cause.
    // Printed as the two fields the driver reports them in. Recomposing them into
    // one number needs that field's convention, and padding the minor to the two
    // digits an NVIDIA version uses turned a reported 537.2 into 537.02.
    if (min_major > 0 && g_nv_driver > 0)
        Log("[bridge]   %d.%d is this driver's stated minimum for Ray Reconstruction, "
            "and the running driver is %u.%02u. Neural rendering is a later and "
            "separate feature with no capability key of its own, so this line does "
            "not speak for it.",
            min_major, min_minor, g_nv_driver / 100u, g_nv_driver % 100u);

    // The whole point of a DLSS 5 add-on is a neural-rendering feature on the
    // D3D12 side, and when it will not create, the report says so in its own
    // words while this log printed the reason in raw hexadecimal and left it at
    // that. Final Fantasy XIV on driver 32.0.15.6094 answers the denoising
    // capability query with UnsupportedParameter and the add-on's feature 18
    // create fails with PlatformError. State what was read; the driver version
    // is logged at the first DLSS call, a few lines above this one.
    if (!denoising_answered)
        Log("[bridge]   this driver does not carry the denoising capability key at all. "
            "Final Fantasy XIV on 560.94 answered this query the same way, its DLSS 5 "
            "add-on could not create a neural-rendering feature, and updating the "
            "driver fixed it. Update the driver before looking anywhere else.");
    else if (denoising_available == 0)
        // This key is Ray Reconstruction's, and it does not speak for DLSS 5
        // neural rendering -- feature 18 has no capability key of its own. The
        // line here used to conclude that a neural-rendering feature "will not
        // get one", which is false and was falsified in this project's own
        // session on 2026-08-31: Skyrim Special Edition reported Available = 0
        // on driver 616.56 and the DLSS 5 add-on then created feature 18 and
        // evaluated it sixty times. State the reading; draw no conclusion from it.
        Log("[bridge]   Ray Reconstruction is reported unavailable here. That says "
            "nothing about DLSS 5 neural rendering, which has no capability key of "
            "its own: this add-on has seen feature 18 created and evaluated on a "
            "driver answering exactly this way.");
}

// bridge.inc prints this before D3D12CreateDevice, and the include comes long
// before the definition.
static void LogPrologue(const char *label, const BYTE *p);

// Set around this add-on's own NGX initialisation. Hooking a module while NGX
// is loading its snippets is what took Prey 2017 down.
static volatile bool g_ngx_init_in_flight;
static volatile LONG g_scan_pending;

// Defined below the include, beside the rest of the snippet-version reading, and
// used inside it: the session opener asks whether the neural snippet beside this
// game is one the driver's own loader faults on. See the note on the definition.
static bool NrSnippetIsOlderThanLoader(char *ver_out, size_t ver_n);
static bool FileVersionString(const wchar_t *path, char *out, size_t n);

// Set once the loader's route to feature 18 has been closed in memory; the
// capability probe uses it so it does not warn about a fault that can no longer
// happen. See PatchNgxFeatureTable.
static bool g_ngx_route_closed;

#include "bridge.inc"


// ---------------------------------------------------------------------------
// Detours
// ---------------------------------------------------------------------------

// Defined with the idle report it withdraws.
static void RetractIdleNote();
static void TryInstallHooks();
static void TriggerHookScan();


// Two builds of one NGX snippet in a single process is a configuration nobody
// would notice by reading version lines one at a time. Escape from Tarkov runs
// the game's own nvngx_dlss.dll at 310.7.129.0 and a hand-placed one at
// 310.8.0.0, and it is the only reported title whose GPU is reset. That is not
// a diagnosis, but 1.0.5 already fixed one fault caused by a hand-placed
// snippet of a different build from the driver's, so the shape is worth naming
// rather than leaving in two lines four hundred apart.
struct SeenSnippet { wchar_t leaf[64]; char ver[48]; };
static SeenSnippet   g_seen[24];
static volatile LONG g_seen_count;

// The super-resolution snippet's version, for the one decision that depends on it.
//
// NGX loads the GAME FOLDER's nvngx_dlss.dll before the driver's, so a game can
// pin a very old one: Red Dead Redemption 2 ships 2.2.10.0, from 2021, and the
// Rockstar launcher re-plants it. DLAA as an NVSDK_NGX_PerfQuality_Value member
// arrived in SDK 3.1.13, and the synthetic contract is DLAA by construction --
// its render size equals its output size.
//
// A 2.x runtime has no network for that. Measured in RDR2 on 2026-09-01, it does
// not refuse the request either: the create returned success at PerfQualityValue
// 5 and delivered 1800 frames, and the picture degraded progressively into
// something the user described as an abstract painting. Accepting an enum it
// predates is worse than refusing it, and this is the only place that can tell.
//
// Returns 0 when no super-resolution snippet has been scanned yet, so a caller
// can distinguish "old" from "not known".
static int SnippetMajorVersion(char *ver_out, size_t ver_n)
{
    if (ver_out != nullptr && ver_n != 0) ver_out[0] = 0;
    for (LONG i = 0; i < g_seen_count; ++i)
    {
        if (_wcsicmp(g_seen[i].leaf, L"nvngx_dlss.dll") != 0) continue;
        if (ver_out != nullptr && ver_n != 0) strcpy_s(ver_out, ver_n, g_seen[i].ver);
        int major = 0;
        // Only the major, and only compared against 3. NVIDIA moved to
        // driver-style numbering at 310.x, so "greater than or equal to 3"
        // covers 3.1.13 and everything after it without this having to parse a
        // four-part version and rank 3.1.12 against 3.1.13 -- a distinction no
        // measurement here supports.
        if (sscanf_s(g_seen[i].ver, "%d", &major) == 1) return major;
        return 0;
    }
    return 0;
}

// The neural-rendering snippet's version, as four numbers, or all zeroes when
// none has been scanned. Unlike the super-resolution one above this has to be
// ranked rather than thresholded on a major, because the version that matters
// differs in its second and third fields.
// Defined further down, beside the rest of the file inspection; used here and by
// the loader choice below, both of which run before it in file order.
static bool FileVersionString(const wchar_t *path, char *out, size_t n);

static void NrSnippetVersion(int out[4], char *ver_out, size_t ver_n)
{
    out[0] = out[1] = out[2] = out[3] = 0;
    if (ver_out != nullptr && ver_n != 0) ver_out[0] = 0;
    for (LONG i = 0; i < g_seen_count; ++i)
    {
        if (_wcsicmp(g_seen[i].leaf, L"nvngx_dlssnr.dll") != 0) continue;
        if (ver_out != nullptr && ver_n != 0) strcpy_s(ver_out, ver_n, g_seen[i].ver);
        sscanf_s(g_seen[i].ver, "%d.%d.%d.%d", &out[0], &out[1], &out[2], &out[3]);
        return;
    }

    // Nothing scanned yet, and one caller runs before anything is. g_seen is
    // filled when NGX's own modules are enumerated, which is long after attach,
    // and the loader has to be chosen before NGX exists in the process at all --
    // so the file is read where NGX would find it: beside this add-on first,
    // then beside the executable, which is the order NGX searches.
    wchar_t base[MAX_PATH] = {};
    for (int where = 0; where < 2; ++where)
    {
        if (GetModuleFileNameW(where == 0 ? g_self : nullptr, base, MAX_PATH) == 0) continue;
        wchar_t *sl = wcsrchr(base, L'\\');
        if (sl == nullptr) continue;
        wcscpy_s(sl + 1, MAX_PATH - (sl + 1 - base), L"nvngx_dlssnr.dll");
        char ver[64];
        if (!FileVersionString(base, ver, sizeof(ver))) continue;
        if (ver_out != nullptr && ver_n != 0) strcpy_s(ver_out, ver_n, ver);
        sscanf_s(ver, "%d.%d.%d.%d", &out[0], &out[1], &out[2], &out[3]);
        return;
    }
}

// The neural snippet that the driver's loader now drives, and the pairing that
// crashes.
//
// NVIDIA 32.0.16.1664's NGX loader gained first-class support for feature 18:
// its feature-id table names "dlssnr" where 32.0.16.1656 had an empty string,
// and DLSSNR.Available and its siblings exist only in the newer file. From that
// version the LOADER routes feature 18 into nvngx_dlssnr.dll, and 310.8.0.0 --
// the newest neural snippet in circulation on 2026-09-04 -- then calls
// ID3D12GraphicsCommandList::SetDescriptorHeaps with a heap whose own
// description reads back as garbage. D3D12 faults validating its node mask,
// this add-on's guarded call catches the access violation, the neighbouring
// DLSS 5 add-on registers no unwind cleanup so its lock stays held, and the
// next lock on that thread throws: the game either terminates or stops
// presenting. Measured on an RTX 5090 with the fault stack this add-on records,
// which names the snippet and puts neither this add-on nor its neighbour
// between it and D3D12.
//
// The pairing is what is broken, not either half: the same snippet on 1656's
// loader delivers frames, and this add-on's own DLSS on 1664 is healthy with no
// neural add-on present. So the test is the pairing, and it self-heals -- a
// newer snippet than the one measured is presumed fixed and allowed through,
// because the alternative is an add-on that refuses to work after the fix lands.
static bool NrSnippetIsOlderThanLoader(char *ver_out, size_t ver_n)
{
    int v[4];
    NrSnippetVersion(v, ver_out, ver_n);
    if (v[0] == 0) return false;   // none scanned; nothing to judge
    // 310.8.0.0 and anything before it. The next field NVIDIA moves is the one
    // this compares, whichever it is.
    if (v[0] != 310) return v[0] < 310;
    if (v[1] != 8)   return v[1] < 8;
    return v[2] == 0 && v[3] == 0;
}

// Whether a file contains a byte sequence. Used on the NGX loader to ask what
// it supports rather than what it is numbered, because a version can be
// renumbered and a capability name in .rdata cannot be there by accident.
static bool FileContainsAscii(const wchar_t *path, const char *needle)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    const size_t n = strlen(needle);
    // Read in blocks with an overlap of n-1 bytes, so a match that straddles a
    // block boundary is still found. 1 MB blocks against a 1.4 MB file is two
    // reads; the whole thing runs once, at attach, on a local file.
    static const DWORD kBlock = 1u << 20;
    char *buf = static_cast<char *>(malloc(kBlock + 64));
    bool  found = false;
    if (buf != nullptr && n > 0 && n < 64)
    {
        DWORD carry = 0;
        for (;;)
        {
            DWORD got = 0;
            if (!ReadFile(h, buf + carry, kBlock, &got, nullptr) || got == 0) break;
            const DWORD have = carry + got;
            for (DWORD i = 0; i + n <= have; ++i)
                if (memcmp(buf + i, needle, n) == 0) { found = true; break; }
            if (found) break;
            carry = static_cast<DWORD>(n - 1);
            memmove(buf, buf + have - carry, carry);
        }
    }
    free(buf);
    CloseHandle(h);
    return found;
}

// Choose the NGX loader before anything else can.
//
// NVIDIA 32.0.16.1664's _nvngx.dll gained first-class support for feature 18 --
// its feature-id table names "dlssnr" where 32.0.16.1656 had an empty string,
// and DLSSNR.Available exists only in the newer file -- and from that version
// the LOADER routes feature 18 into nvngx_dlssnr.dll itself. Snippet 310.8.0.0,
// the newest in circulation on 2026-09-04, then hands D3D12 a descriptor heap
// whose description reads back as garbage: D3D12 faults inside
// SetDescriptorHeaps, this add-on's guarded call catches it, the neural add-on
// beside us registers no unwind cleanup so its lock stays held, and the game
// terminates or stops presenting. The captured fault stack names the snippet
// with neither add-on between it and D3D12.
//
// The whole difference is which loader answers. Windows resolves a bare-name
// LoadLibrary against modules already loaded BY BASE NAME, so whichever file
// called _nvngx.dll is loaded first owns that name for the process -- which is
// why copying the previous driver's copy beside a game executable fixes it, and
// why doing that by hand is only reliable if nothing loaded it first. This add-on
// attaches before the game's DLSS initialises, so it can make that choice
// itself: find a loader that does not answer DLSSNR.Available, load it by full
// path, and every later resolution of the name gets that one.
//
// Deliberately narrow. It does nothing unless all three hold: nothing has loaded
// an _nvngx.dll yet, the snippet beside the game is one measured to fault, and
// the driver store still holds a loader without the new route. A newer snippet
// makes it stand down on its own, which is the behaviour wanted when NVIDIA
// fixes this. This is the fallback -- ngx_loader=2 -- and it needs the previous
// driver to still be on disk, which a clean install or a DDU pass removes; the
// default closes the route in memory instead and needs nothing.
static wchar_t g_ngx_loader_pref[MAX_PATH];


// The default answer: close the loader's own route to feature 18 in memory.
//
// _nvngx.dll carries a 19-entry table of feature id -> snippet name in writable
// data. Entries 0..17 are byte-identical between 1656 and 1664; entry 18 is the
// only difference in the whole table -- 1656 points it at L"" and 1664 at
// L"dlssnr". NGXSecureLoadFeature loads nothing and returns 0xBAD00001 when the
// name is empty, so putting an empty string back into that one slot reproduces
// 1656's behaviour exactly, in this process only, with nothing on disk touched
// and no old driver needed. The neural add-on then drives feature 18 itself the
// way it did before the driver update, which is the path that works.
//
// The table is found by pattern, never by address: entry 1 must read "dlss",
// entry 11 "dlssg" and entry 18 "dlssnr". A driver that grows, reorders or moves
// the table simply fails to match, and this logs and does nothing.
static bool NgxNameIs(const BYTE *base, size_t image, const void *p, const wchar_t *want)
{
    const BYTE *s = static_cast<const BYTE *>(p);
    if (s < base || s >= base + image) return false;
    const size_t room = static_cast<size_t>(base + image - s) / sizeof(wchar_t);
    const size_t n    = wcslen(want);
    if (room <= n) return false;
    return wcsncmp(reinterpret_cast<const wchar_t *>(s), want, n + 1) == 0;
}

static bool PatchNgxFeatureTable(HMODULE ngx)
{
    BYTE *base = reinterpret_cast<BYTE *>(ngx);
    if (base == nullptr) return false;

    const IMAGE_DOS_HEADER *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const IMAGE_NT_HEADERS *nt =
        reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const size_t image = nt->OptionalHeader.SizeOfImage;
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);

    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        if ((sec[i].Characteristics & IMAGE_SCN_MEM_WRITE) == 0) continue;
        void **p   = reinterpret_cast<void **>(base + sec[i].VirtualAddress);
        void **end = reinterpret_cast<void **>(base + sec[i].VirtualAddress +
                                               sec[i].Misc.VirtualSize);
        for (; p + 19 <= end; ++p)
        {
            if (!NgxNameIs(base, image, p[1],  L"dlss"))   continue;
            if (!NgxNameIs(base, image, p[11], L"dlssg"))  continue;
            if (!NgxNameIs(base, image, p[18], L"dlssnr")) continue;

            DWORD old = 0;
            if (!VirtualProtect(&p[18], sizeof(void *), PAGE_READWRITE, &old))
            {
                Log("[bridge] the loader's feature table is at %p but could not be made "
                    "writable (%lu), so the driver keeps its own route to neural rendering.",
                    (void *)p, GetLastError());
                return false;
            }
            // The empty string has to live inside the loader, not in this
            // add-on: ReShade can unload an add-on while the process runs on,
            // and the loader stays mapped, so a pointer into our .rdata would
            // dangle and the next NGX init would fault reading it. The
            // terminator of the loader's own "dlssnr" is an empty string at a
            // permanent address, and NgxNameIs has just proved it is in-image.
            p[18] = static_cast<wchar_t *>(p[18]) + wcslen(L"dlssnr");
            g_ngx_route_closed = true;
            VirtualProtect(&p[18], sizeof(void *), old, &old);
            Log("[bridge] the driver's route to neural rendering is closed for this process: "
                "entry 18 of the loader's feature table at %p named \"dlssnr\" and now names "
                "nothing, which is what the previous driver had there. The DLSS 5 add-on "
                "drives that feature itself, as it did before the driver update. Nothing on "
                "disk is changed and no other feature is touched. Set ngx_loader=1 in "
                "dlss5-bridge.cfg to leave the driver's route alone.", (void *)p);
            return true;
        }
    }
    return false;
}

// Where the process would find NGX if nothing interfered. NVIDIA's own SDK
// reads this key, so it names the loader the game is going to get.
static bool DriverNgxLoaderPath(wchar_t *out, size_t cch)
{
    wchar_t dir[MAX_PATH] = {};
    DWORD   cb = sizeof(dir);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore",
                     L"FullPath", RRF_RT_REG_SZ, nullptr, dir, &cb) != ERROR_SUCCESS ||
        dir[0] == 0)
        return false;
    _snwprintf_s(out, cch, _TRUNCATE, L"%ls\\_nvngx.dll", dir);
    return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}


static void PreferWorkingNgxLoader()
{
    if (g_cfg.ngx_loader == 1) return;

    char nrver[48] = "";
    const bool snippet_old = NrSnippetIsOlderThanLoader(nrver, sizeof(nrver));
    // The snippet beside the game is the whole question. It was tempting to also
    // require a neural add-on this add-on has measured, and that was wrong: the
    // fault is between the driver's loader and the snippet, with no add-on
    // between them, so gating on one add-on's sha256 would have left every other
    // build -- including the next release of the one measured -- back on the
    // faulting route with nothing in the log to say why. Nothing is lost by
    // being broad: a process that never asks for feature 18 never reads that
    // entry, so closing it there costs nothing.
    if (g_cfg.ngx_loader != 2 && !snippet_old) return;

    if (g_cfg.ngx_loader != 2)
    {
        // The loader the process is going to use anyway, patched in place. It
        // does not matter whether something else loaded it first.
        HMODULE ngx = GetModuleHandleW(L"_nvngx.dll");
        wchar_t path[MAX_PATH] = {};
        if (ngx == nullptr && DriverNgxLoaderPath(path, _countof(path)))
            ngx = LoadLibraryW(path);
        if (ngx == nullptr)
        {
            Log("[bridge] no NGX loader is loaded and none is registered, so there is nothing "
                "to adjust; the driver will bring its own in later.");
            return;
        }
        if (!PatchNgxFeatureTable(ngx))
            Log("[bridge] this NGX loader has no \"dlssnr\" entry in its feature table, so it "
                "does not drive neural rendering itself and there is nothing to close. Set "
                "ngx_loader=2 if the session still faults inside the snippet.");
        return;
    }

    if (GetModuleHandleW(L"_nvngx.dll") != nullptr)
    {
        Log("[bridge] an NGX loader was already loaded before this add-on could choose one, so "
            "the choice is not this add-on's to make in this process.");
        return;
    }

    wchar_t dir[MAX_PATH] = {};
    if (GetSystemDirectoryW(dir, MAX_PATH) == 0) return;
    wcscat_s(dir, L"\\DriverStore\\FileRepository\\");

    wchar_t pattern[MAX_PATH];
    _snwprintf_s(pattern, _countof(pattern), _TRUNCATE, L"%lsnv_dispi.inf_*", dir);

    wchar_t best[MAX_PATH] = {};
    char    best_ver[48]   = "";
    int     seen_new = 0, seen_old = 0;

    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
            wchar_t cand[MAX_PATH];
            _snwprintf_s(cand, _countof(cand), _TRUNCATE, L"%ls%ls\\_nvngx.dll", dir, fd.cFileName);
            if (GetFileAttributesW(cand) == INVALID_FILE_ATTRIBUTES) continue;

            char ver[48] = "";
            FileVersionString(cand, ver, sizeof(ver));
            if (FileContainsAscii(cand, "DLSSNR.Available")) { ++seen_new; continue; }
            ++seen_old;
            // The newest of the ones without the new route, so the pick is as
            // close to this driver as it can be while still taking the old path.
            if (best[0] == 0 || strcmp(ver, best_ver) > 0)
            { wcscpy_s(best, cand); strcpy_s(best_ver, ver); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    if (seen_new == 0 && g_cfg.ngx_loader != 2)
    {
        // Nothing here drives the neural snippet, so nothing to avoid.
        return;
    }
    if (best[0] == 0)
    {
        Log("[bridge] the snippet beside this game is %s, which this driver's loader faults on, "
            "and the driver store holds no older loader to fall back to -- a driver cleanup "
            "removes the previous one. A newer nvngx_dlssnr.dll is the fix.",
            nrver[0] != 0 ? nrver : "an older build");
        return;
    }
    if (LoadLibraryW(best) == nullptr)
    {
        Log("[bridge] %ls would not load (error %lu), so the loader is left as the driver "
            "chooses it.", best, GetLastError());
        return;
    }
    wcscpy_s(g_ngx_loader_pref, best);
    Log("[bridge] NGX loader chosen by this add-on: %ls, version %s. The driver's own is %s and "
        "drives neural rendering itself, which the %s snippet beside this game faults on; this "
        "one leaves that route alone. Windows resolves _nvngx.dll by name against what is already "
        "loaded, so this is now the one the whole process gets. Set ngx_loader=1 in "
        "dlss5-bridge.cfg to leave the choice to the driver.",
        best, best_ver[0] != 0 ? best_ver : "unknown",
        seen_new == 1 ? "newer" : "newer still", nrver[0] != 0 ? nrver : "older");
}

static void NoteSnippetVersion(const wchar_t *leaf, const char *ver)
{
    for (LONG i = 0; i < g_seen_count; ++i)
    {
        if (_wcsicmp(g_seen[i].leaf, leaf) != 0) continue;
        if (strcmp(g_seen[i].ver, ver) == 0) return;

        Log("  *** %ls is loaded twice at different versions: %s and %s. One of "
            "them was placed by hand. NGX loads one of the two for this add-on's "
            "D3D12 session while the game uses the other, and a snippet that does "
            "not match the driver has faulted before. Removing the hand-placed "
            "copy leaves the game's own in use. ***",
            leaf, g_seen[i].ver, ver);
        return;
    }
    if (g_seen_count < static_cast<LONG>(_countof(g_seen)))
    {
        const LONG k = g_seen_count;
        wcscpy_s(g_seen[k].leaf, leaf);
        strcpy_s(g_seen[k].ver, ver);
        g_seen_count = k + 1;
    }
}

// Reads a file's VERSIONINFO. Every component in this stack is versioned and
// distributed separately -- the driver's NGX loader, the feature snippets, the
// DLSS 5 add-on -- and a report that names them without their versions cannot
// be compared against a working machine.
static bool FileVersionString(const wchar_t *path, char *out, size_t n)
{
    out[0] = '\0';
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &ignored);
    if (size == 0) return false;

    void *buf = malloc(size);
    if (buf == nullptr) return false;

    bool ok = false;
    if (GetFileVersionInfoW(path, 0, size, buf))
    {
        VS_FIXEDFILEINFO *fi = nullptr;
        UINT len = 0;
        if (VerQueryValueW(buf, L"\\", reinterpret_cast<void **>(&fi), &len) &&
            fi != nullptr && len >= sizeof(VS_FIXEDFILEINFO))
        {
            sprintf_s(out, n, "%u.%u.%u.%u",
                      HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                      HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
            ok = true;
        }
    }
    free(buf);
    return ok;
}

// Logs a file's version next to its name, or says the file carries none.
static void LogFileVersion(const wchar_t *dir, const wchar_t *leaf, const char *label)
{
    wchar_t path[MAX_PATH];
    wcscpy_s(path, dir);
    wcscat_s(path, leaf);

    char ver[64];
    if (!FileVersionString(path, ver, sizeof(ver)))
        strcpy_s(ver, "no version resource");

    // The size and the date, beside the version, because the version alone does
    // not identify a build. Measured on 2026-09-01: two copies of the same DLSS 5
    // add-on, 1,732,608 bytes in one game folder and 1,703,424 in another, both
    // declaring 0.2026.0828.0517. One of them behaved differently from the other
    // and no line in this log could tell them apart -- the whole afternoon was
    // spent looking for that difference in this add-on.
    //
    // Free: the caller already has both out of the directory enumeration, and
    // this reads them again only because the signature took a leaf rather than a
    // WIN32_FIND_DATA. Size and mtime separate two builds; a hash would separate
    // them better and cost a full read of a 165 MB neighbour.
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa))
    {
        SYSTEMTIME st = {};
        FileTimeToSystemTime(&fa.ftLastWriteTime, &st);
        Log("    %-26ls %s  version %s, %llu bytes, %04u-%02u-%02u %02u:%02u",
            leaf, label, ver,
            (static_cast<unsigned long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow,
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    }
    else
    {
        Log("    %-26ls %s  version %s", leaf, label, ver);
    }
}

// What the status panel says about this add-on's neighbours. Captured while the
// log below is written rather than re-read when the panel draws: this runs once
// at attach on the loader thread and cannot change afterwards, and the panel
// runs on ReShade's UI thread on every frame the overlay is open. Doing file
// I/O there to restate a fixed fact would be the wrong trade twice over. Written
// before ReShade has presented once and read-only from then on, which is why no
// lock guards them.
static char g_panel_addons[768];
static char g_panel_dlss5[1024];

// strcat_s calls the invalid-parameter handler on overflow, which terminates the
// process. These buffers are filled from a directory listing and a file, neither
// of which has a bound this code sets, so appending has to be the kind that
// stops rather than the kind that aborts. What does not fit is dropped: the log
// beside it already carries every line in full.
static void PanelAppend(char *buf, size_t n, const char *s)
{
    const size_t have = strlen(buf);
    if (have + 2 >= n) return;
    _snprintf_s(buf + have, n - have, _TRUNCATE, "%s\n", s);
}

// The DLSS 5 add-on keeps its own settings in ReShade.ini, and those settings
// decide whether it does anything at all. Reading them here answers from the
// log what previously needed a screenshot of its overlay: whether neural
// rendering was even enabled, whether upscaling was requested, which depth
// convention was chosen. Only keys this add-on's neighbour is known to write
// are reported; nothing else in the file is touched or logged.
static void LogReShadeConfig(const wchar_t *dir)
{
    wchar_t path[MAX_PATH];
    wcscpy_s(path, dir);
    wcscat_s(path, L"ReShade.ini");

    FILE *f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr)
    {
        Log("  ReShade.ini: not next to this add-on, so the DLSS 5 add-on's own "
            "settings could not be read.");
        return;
    }


    char section[128] = "";
    char line[512];
    bool header = false;
    while (fgets(line, sizeof(line), f) != nullptr)
    {
        char *b = line;
        while (*b == ' ' || *b == '\t') ++b;
        size_t l = strlen(b);
        while (l > 0 && (b[l - 1] == '\n' || b[l - 1] == '\r' || b[l - 1] == ' ')) b[--l] = '\0';
        if (l == 0) continue;

        if (b[0] == '[')
        {
            strcpy_s(section, b);
            continue;
        }

        // The whole of the DLSS 5 add-on's own section, rather than keys
        // beginning NR: version 3.3.5 introduced EnableHooks, which decides
        // whether it hooks anything at all, and a prefix filter would have left
        // the single most important setting out of every report. The section
        // name has to be exact -- "[RenoDX" also matches [renodx-preset1],
        // which belongs to a different add-on and is forty lines of colour
        // grading.
        if (_stricmp(section, "[RenoDX.DLSS5]") != 0) continue;
        if (strchr(b, '=') == nullptr) continue;

        if (!header)
        {
            Log("  DLSS 5 add-on settings, read from ReShade.ini %s:", section);
            header = true;
        }
        Log("    %s", b);
        PanelAppend(g_panel_dlss5, sizeof(g_panel_dlss5), b);
    }
    fclose(f);

    if (!header)
        Log("  ReShade.ini holds no DLSS 5 add-on settings. Its overlay has "
            "probably never been opened, so it is running on its defaults.");
}


// Handles created for a feature that is not DLSS super resolution. Their
// parameter blocks are not an upscaling contract -- The Elder Scrolls Online
// runs frame generation through the same D3D11 entry points -- so an evaluate
// against one of these is forwarded and otherwise left alone. Only handles known
// to be something else are rejected: an unrecognised handle still drives the
// bridge, so a game whose feature was created before the hooks went in keeps
// working exactly as before.
static const NVSDK_NGX_Handle *g_other_feature[32];
static volatile LONG           g_other_feature_count;

static bool IsOtherFeature(const NVSDK_NGX_Handle *h)
{
    for (LONG i = 0; i < g_other_feature_count; ++i)
        if (g_other_feature[i] == h) return true;
    return false;
}

// A Ray Reconstruction evaluate hands over the same Color, Depth, MotionVectors
// and Output as a super-resolution one, so the four resources this bridge reads
// cannot tell the two apart. Mirroring a denoise onto a super-resolution feature
// upscales input that is still noisy and then copies the result over the output
// the game had already denoised: on screen that is indistinguishable from
// denoising switched off.
//
// ForwardCreate already records every feature_id other than 1, so a denoiser
// created through these hooks is recognised by handle and never reaches here.
// This exists for the one case that table cannot cover -- a feature created
// before the hooks went in -- which is exactly the case the comment above
// g_other_feature says keeps driving the bridge.
//
// Only resource keys are probed. The matrices a denoiser also sets are float
// arrays, and asking for one through a resource accessor answers a question
// nobody asked. Both spellings are listed because a key this runtime does not
// have answers UnsupportedParameter: a wrong name costs one failed call and can
// only ever produce a false negative.
static const char *const kDenoiserKeys[] = {
    "NormalRoughness",       "DLSSD.NormalRoughness",
    "DiffuseAlbedo",         "DLSSD.DiffuseAlbedo",
    "SpecularAlbedo",        "DLSSD.SpecularAlbedo",
    "SpecularHitDistance",   "DLSSD.SpecularHitDistance",
    "SpecularMotionVectors", "DLSSD.SpecularMotionVectors",
    "GBuffer.Normals",       "GBuffer.Roughness",
};

// Returns the key that matched, or nullptr. Probed on every evaluate rather than
// cached against the handle: NGX recycles freed handle addresses, so a cache
// keyed on one needs the same invalidation ForwardCreate carries for
// g_other_feature -- and writing that table from the render thread while the
// create path writes it too is a race this does not need. Twelve calls that
// return a result code cost nothing beside a texture copy and a DLSS evaluate.
static const char *DenoiserKeyPresent(const NVSDK_NGX_Parameter *p)
{
    if (p == nullptr) return nullptr;
    for (size_t i = 0; i < _countof(kDenoiserKeys); ++i)
    {
        ID3D11Resource *res = nullptr;
        if (p->Get(kDenoiserKeys[i], &res) == NGX_SUCCESS && res != nullptr)
            return kDenoiserKeys[i];
    }
    return nullptr;
}

static volatile LONG g_eval_count   = 0;
static volatile LONG g_create_count = 0;

// EvaluateFeature and EvaluateFeature_C are distinct exports at distinct
// addresses. BG3 drives DLSS through the _C variant, so hooking only the C++
// one catches CreateFeature and nothing else. Both are hooked here.
static NVSDK_NGX_Result ForwardEvaluate(Hook &h, const char *tag, ID3D11DeviceContext *ctx,
                                        const NVSDK_NGX_Handle *handle,
                                        const NVSDK_NGX_Parameter *p, void *cb)
{
    TrampolineCallGuard call_guard;
    auto fwd = reinterpret_cast<PFN_Evaluate>(h.original ? h.original : h.target);

    // A nested call means the layer above already handled this frame and is now
    // calling down through NGX's own plumbing. Forward it and touch nothing.
    if (g_nest > 0)
    {
        if (g_ngx_cs_ready) EnterCriticalSection(&g_ngx_cs);
        NVSDK_NGX_Result inner = fwd(ctx, handle, p, cb);
        if (g_ngx_cs_ready) LeaveCriticalSection(&g_ngx_cs);
        return inner;
    }
    NestGuard nest;

    // Just Cause 3 recorded an evaluate arriving with handle=0x7 and params=0x1
    // fourteen milliseconds after a placeholder module was hooked: not pointers,
    // but whatever happened to be in the registers when a confused detour was
    // entered. 1.0.11 no longer hooks such modules, but reading a parameter
    // block through a small integer is the kind of thing that should never
    // depend on that. The bottom 64 KB of the address space can never be mapped.
    if (reinterpret_cast<uintptr_t>(p) < 0x10000 ||
        reinterpret_cast<uintptr_t>(handle) < 0x10000 ||
        reinterpret_cast<uintptr_t>(ctx) < 0x10000)
    {
        static LONG said = 0;
        if (InterlockedCompareExchange(&said, 1, 0) == 0)
            Log("[bridge] an evaluate arrived with handle=%p params=%p ctx=%p -- none of "
                "those can be real. Forwarding it untouched and ignoring it.",
                static_cast<const void *>(handle), static_cast<const void *>(p),
                static_cast<void *>(ctx));

        if (g_ngx_cs_ready) EnterCriticalSection(&g_ngx_cs);
        NVSDK_NGX_Result bogus = fwd(ctx, handle, p, cb);
        if (g_ngx_cs_ready) LeaveCriticalSection(&g_ngx_cs);
        return bogus;
    }

    if (IsOtherFeature(handle))
    {
        if (g_ngx_cs_ready) EnterCriticalSection(&g_ngx_cs);
        NVSDK_NGX_Result other = fwd(ctx, handle, p, cb);
        if (g_ngx_cs_ready) LeaveCriticalSection(&g_ngx_cs);
        return other;
    }
    // source=synth pins the synthetic contract, which means the mirror stands
    // aside even when the game has DLSS of its own. The key was documented as a
    // pin and read into the config and shown on the panel, and nothing acted on
    // this half of it: source=mirror blocked the synthetic path, source=synth
    // blocked nothing. The game's own call is forwarded untouched, and the latch
    // is not taken, which is what leaves the field for the other source.
    if (g_cfg.source == CFG_SRC_SYNTH)
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            Log("[bridge] source=synth, so the game's own DLSS is forwarded and not "
                "mirrored. Only a synthetic contract may hold this session.");
        }
        if (g_ngx_cs_ready) EnterCriticalSection(&g_ngx_cs);
        NVSDK_NGX_Result pinned = fwd(ctx, handle, p, cb);
        if (g_ngx_cs_ready) LeaveCriticalSection(&g_ngx_cs);
        return pinned;
    }

    const LONG n = InterlockedIncrement(&g_eval_count);
    if (n == 1) RetractIdleNote();

    // One-way and unconditional: the game's own DLSS wins from any state, at any
    // time, and nothing ever puts this back. The asymmetry is the whole reason.
    // "This game has DLSS" is provable only positively and only at a moment
    // nothing here can bound -- Escape from Tarkov called DLSS twenty-three
    // seconds in, and a title whose first call comes from a menu can take far
    // longer -- while "this game has no DLSS" is not provable at all. So a
    // substitute may hold the field only until the real contract arrives, and
    // then it must lose it, whatever it was in the middle of.
    // The old value is kept because it says whether anything has to be undone.
    // A substitute that was mid-frame when this arrived has already left a
    // feature standing in g_bridge, and it cannot clear that itself until its
    // own next frame -- which is one presented frame too late if the game's
    // buffers happen to match the shape it built for.
    const LONG prev_source = InterlockedExchange(&g_source, SRC_MIRROR);

    // The loader notification is allowed to give up rather than block, so a
    // module can be missed. This is the same scan from the render thread, where
    // no loader lock is held and giving up costs nothing.
    // A scan deferred out of the loader-lock callback is serviced here, where
    // nothing is held.
    if (g_scan_pending && !g_ngx_init_in_flight)
    { TriggerHookScan(); }
    if ((n % 600) == 0) TriggerHookScan();
    if (n <= 5 || (n % 1800) == 0)
    {
        Log("  (entry point: %s)", tag);
        DumpEvaluate(ctx, handle, p, n);
    }

    // The game's DLSS writes an Output that the bridge then overwrites, so when
    // the bridge is delivering, running it is pure waste. Suppressed only while
    // BridgeWillDeliver() holds; the moment anything goes wrong the call is
    // forwarded again and the game renders on its own.
    // Ray Reconstruction rather than super resolution: forward it and touch
    // nothing. Counted and dumped above first, so a title that only ever
    // denoises still reports evaluates instead of tripping the idle note.
    if (const char *dkey = DenoiserKeyPresent(p))
    {
        static LONG said_rr = 0;
        if (InterlockedCompareExchange(&said_rr, 1, 0) == 0)
            Log("[bridge] this evaluate sets \"%s\", which only the denoiser sets, so it "
                "is Ray Reconstruction and not super resolution. Its contract is not the "
                "one this bridge mirrors, and upscaling a still-noisy input over an "
                "output the game already denoised would look like denoising switched "
                "off. Forwarded untouched. The feature was created before the hooks went "
                "in, or its handle would already be recorded.", dkey);

        if (g_ngx_cs_ready) EnterCriticalSection(&g_ngx_cs);
        NVSDK_NGX_Result denoise = fwd(ctx, handle, p, cb);
        if (g_ngx_cs_ready) LeaveCriticalSection(&g_ngx_cs);
        return denoise;
    }

    Breadcrumb("forwarding the game's DLSS evaluate");
    const bool suppress = BridgeWillDeliver(handle);

    NVSDK_NGX_Result r = NGX_SUCCESS;
    if (!suppress)
    {
        if (g_ngx_cs_ready) EnterCriticalSection(&g_ngx_cs);
        r = fwd(ctx, handle, p, cb);
        if (g_ngx_cs_ready) LeaveCriticalSection(&g_ngx_cs);
    }

    if (n <= 5)
        Log("--- EvaluateFeature #%ld returned %d%s ---", n, r,
            suppress ? " (game's own DLSS suppressed)" : "");

    // Worth saying in words. If the game's own DLSS is already failing, the
    // contract was broken before this add-on touched it, and chasing the bridge
    // is chasing the wrong thing.
    if (!suppress && r != NGX_SUCCESS && n <= 5)
        Log("  the game's own DLSS call failed (0x%08X), before the bridge changed "
            "anything. Whatever is wrong is wrong upstream of it.", r);

    // The bridge runs after the game's own evaluate has been forwarded, so the
    // game's Color holds this frame's input and its Output can be replaced.
    // stage=0 is documented as fully inert and has to mean it: opening the D3D12
    // session creates a device and starts an NGX session, and on some drivers
    // that is itself the thing that crashes. An off switch that still does the
    // dangerous part is not an off switch.
    // The session open and the frame are one unit under g_bridge_cs, not two:
    // a second source that took the section between them would find
    // session_ready true and every texture still null. Uncontended whenever
    // this is the only source, which the arming latch makes the usual case.
    EnterCriticalSection(&g_bridge_cs);
    g_eval_handle = handle;
    // Inside the section, so it cannot land while a substitute is between its
    // own copies and its copy back. One-shot by construction: the exchange above
    // has already made g_source SRC_MIRROR, so the next evaluate reads
    // SRC_MIRROR here and this is skipped. In a session with one source
    // prev_source is never SRC_SYNTH and this is a compare against a local.
    if (prev_source == SRC_SYNTH)
    {
        g_bridge.frame_ready = false;
        g_bridge.need_reset  = true;
    }
    if (g_cfg.stage >= 1 && InterlockedCompareExchange(&g_probe_done, 1, 0) == 0)
    {
        ID3D11Device *dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev != nullptr) { BridgeInitSession(dev, ctx); dev->Release(); }
    }
    BridgeFrame(ctx, p);
    LeaveCriticalSection(&g_bridge_cs);

    return r;
}

static NVSDK_NGX_Result ForwardCreate(Hook &h, ID3D11DeviceContext *ctx, int feature_id,
                                      NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **out)
{
    TrampolineCallGuard call_guard;
    auto fwd = reinterpret_cast<PFN_Create>(h.original ? h.original : h.target);
    if (g_nest > 0)
    {
        if (g_ngx_cs_ready) EnterCriticalSection(&g_ngx_cs);
        NVSDK_NGX_Result inner = fwd(ctx, feature_id, p, out);
        if (g_ngx_cs_ready) LeaveCriticalSection(&g_ngx_cs);
        return inner;
    }
    NestGuard nest;

    // Same guard as the evaluate path, which had it and this did not. A detour
    // entered from something that is not a call to it arrives with whatever was
    // in the registers, and the bottom 64 KB of the address space is never
    // mapped, so an argument below it cannot be real.
    if (reinterpret_cast<uintptr_t>(p) < 0x10000 ||
        reinterpret_cast<uintptr_t>(ctx) < 0x10000)
    {
        static LONG said = 0;
        if (InterlockedCompareExchange(&said, 1, 0) == 0)
            Log("[bridge] a create arrived with params=%p ctx=%p -- neither can be real. "
                "Forwarding it untouched and ignoring it.",
                static_cast<void *>(p), static_cast<void *>(ctx));

        if (g_ngx_cs_ready) EnterCriticalSection(&g_ngx_cs);
        NVSDK_NGX_Result bogus = fwd(ctx, feature_id, p, out);
        if (g_ngx_cs_ready) LeaveCriticalSection(&g_ngx_cs);
        return bogus;
    }

    Breadcrumb("forwarding the game's DLSS create");
    const LONG n = InterlockedIncrement(&g_create_count);
    if (n == 1) RetractIdleNote();
    Log("=== CreateFeature #%ld  feature_id=%d  thread %lu ===", n, feature_id,
        GetCurrentThreadId());
    {
        ID3D11Device *d = nullptr;
        if (ctx != nullptr) ctx->GetDevice(&d);
        if (d != nullptr) { LogAdapterOnce(d); d->Release(); }
    }
    DumpContext(ctx);
    if (p != nullptr)
    {
        Log("  creation parameters:");
        DumpUInt(p, "Width");
        DumpUInt(p, "Height");
        DumpUInt(p, "OutWidth");
        DumpUInt(p, "OutHeight");
        DumpInt(p, "DLSS.Feature.Create.Flags");
        DumpInt(p, "PerfQualityValue");
        DumpInt(p, "RTXValue");
        DumpInt(p, "DLSS.Enable.Output.Subrects");
    }


    if (g_ngx_cs_ready) EnterCriticalSection(&g_ngx_cs);
    NVSDK_NGX_Result r = fwd(ctx, feature_id, p, out);
    if (g_ngx_cs_ready) LeaveCriticalSection(&g_ngx_cs);

    Log("=== CreateFeature #%ld returned %d, handle=%p ===", n, r,
        (out != nullptr && *out != nullptr) ? static_cast<void *>(*out) : nullptr);

    // 1 is DLSS super resolution, the only feature whose contract this bridge
    // knows how to mirror.
    // Nothing ever removed entries, and NGX recycles freed handle addresses. A
    // super-resolution feature created at an address this table still holds
    // would take the "not super resolution" branch on every evaluate for the
    // rest of the session -- the add-on silently doing nothing while the game
    // renders on its own DLSS. A successful super-resolution create at an
    // address is exactly the event that invalidates an older entry for it.
    if (feature_id == 1 && r == NGX_SUCCESS && out != nullptr && *out != nullptr)
        BridgeNoteCreate(*out, p);
    if (feature_id == 1 && r == NGX_SUCCESS && out != nullptr && *out != nullptr)
        for (LONG i = 0; i < g_other_feature_count; ++i)
            if (g_other_feature[i] == *out)
            {
                g_other_feature[i] = g_other_feature[--g_other_feature_count];
                Log("  handle %p was recorded as another feature and has been reused "
                    "for super resolution; the old entry is dropped.", *out);
                break;
            }

    if (feature_id != 1 && r == NGX_SUCCESS && out != nullptr && *out != nullptr &&
        g_other_feature_count < static_cast<LONG>(_countof(g_other_feature)))
    {
        g_other_feature[g_other_feature_count++] = *out;
        Log("  feature %d is not super resolution; evaluates on this handle will be "
            "forwarded and otherwise ignored.", feature_id);
    }
    return r;
}

// Defined in vkmirror.inc, which is included after synth.inc -- it needs the
// private D3D12 session and the Vulkan transport, both of which live there --
// while the detour table below has to exist before the module scan. void * for
// VkCommandBuffer and VkDevice: both are dispatchable, both pointer-sized, and
// nothing here dereferences either.
static NVSDK_NGX_Result ForwardVkEvaluate(Hook &h, const char *tag, void *cmd,
                                          const NVSDK_NGX_Handle *handle,
                                          const NVSDK_NGX_Parameter *p, void *cb);
static NVSDK_NGX_Result ForwardVkCreate(Hook &h, void *cmd, int feature_id,
                                        NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **out);
static NVSDK_NGX_Result ForwardVkCreate1(Hook &h, void *dev, void *cmd, int feature_id,
                                         NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **out);

// One detour per layer, because a detour has to know which layer's saved bytes
// to restore before forwarding, and the entry point itself carries no way to
// tell. Eight sets of three is duplication a macro can carry; the alternative is
// hand-written thunks in assembly.
#define LAYER_DETOURS(i)                                                             \
    static NVSDK_NGX_Result Detour_Evaluate_##i(                                     \
        ID3D11DeviceContext *c, const NVSDK_NGX_Handle *h,                           \
        const NVSDK_NGX_Parameter *p, void *cb)                                      \
    { return ForwardEvaluate(g_layer[i].eval,                                        \
                             "NVSDK_NGX_D3D11_EvaluateFeature", c, h, p, cb); }      \
    static NVSDK_NGX_Result Detour_Evaluate_C_##i(                                   \
        ID3D11DeviceContext *c, const NVSDK_NGX_Handle *h,                           \
        const NVSDK_NGX_Parameter *p, void *cb)                                      \
    { return ForwardEvaluate(g_layer[i].eval_c,                                      \
                             "NVSDK_NGX_D3D11_EvaluateFeature_C", c, h, p, cb); }    \
    static NVSDK_NGX_Result Detour_Create_##i(                                       \
        ID3D11DeviceContext *c, int f, NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **o) \
    { return ForwardCreate(g_layer[i].create, c, f, p, o); }                         \
    static NVSDK_NGX_Result Detour_VkEvaluate_##i(                                   \
        void *c, const NVSDK_NGX_Handle *h,                                          \
        const NVSDK_NGX_Parameter *p, void *cb)                                      \
    { return ForwardVkEvaluate(g_layer[i].vk_eval,                                   \
                               "NVSDK_NGX_VULKAN_EvaluateFeature", c, h, p, cb); }   \
    static NVSDK_NGX_Result Detour_VkEvaluate_C_##i(                                 \
        void *c, const NVSDK_NGX_Handle *h,                                          \
        const NVSDK_NGX_Parameter *p, void *cb)                                      \
    { return ForwardVkEvaluate(g_layer[i].vk_eval_c,                                 \
                               "NVSDK_NGX_VULKAN_EvaluateFeature_C", c, h, p, cb); } \
    static NVSDK_NGX_Result Detour_VkCreate_##i(                                     \
        void *c, int f, NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **o)                \
    { return ForwardVkCreate(g_layer[i].vk_create, c, f, p, o); }                    \
    static NVSDK_NGX_Result Detour_VkCreate1_##i(                                    \
        void *d, void *c, int f, NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **o)       \
    { return ForwardVkCreate1(g_layer[i].vk_create1, d, c, f, p, o); }

LAYER_DETOURS(0) LAYER_DETOURS(1) LAYER_DETOURS(2)  LAYER_DETOURS(3)
LAYER_DETOURS(4) LAYER_DETOURS(5) LAYER_DETOURS(6)  LAYER_DETOURS(7)
LAYER_DETOURS(8) LAYER_DETOURS(9) LAYER_DETOURS(10) LAYER_DETOURS(11)

struct DetourSet
{
    PFN_Evaluate eval;
    PFN_Evaluate eval_c;
    PFN_Create   create;
    // void *, because the Vulkan detours' own prototypes are not the D3D11 ones
    // and HookInstall takes a void * anyway.
    void        *vk_eval;
    void        *vk_eval_c;
    void        *vk_create;
    void        *vk_create1;
};

#define LAYER_ENTRY(i) { &Detour_Evaluate_##i, &Detour_Evaluate_C_##i, &Detour_Create_##i, \
                         reinterpret_cast<void *>(&Detour_VkEvaluate_##i),                 \
                         reinterpret_cast<void *>(&Detour_VkEvaluate_C_##i),               \
                         reinterpret_cast<void *>(&Detour_VkCreate_##i),                   \
                         reinterpret_cast<void *>(&Detour_VkCreate1_##i) }
static const DetourSet kDetour[kMaxLayers] = {
    LAYER_ENTRY(0), LAYER_ENTRY(1), LAYER_ENTRY(2),  LAYER_ENTRY(3),
    LAYER_ENTRY(4), LAYER_ENTRY(5), LAYER_ENTRY(6),  LAYER_ENTRY(7),
    LAYER_ENTRY(8), LAYER_ENTRY(9), LAYER_ENTRY(10), LAYER_ENTRY(11),
};

// ---------------------------------------------------------------------------
// Module discovery
// ---------------------------------------------------------------------------

typedef BOOL (WINAPI *PFN_EnumProcessModules)(HANDLE, HMODULE *, DWORD, LPDWORD);

// The NGX entry points may live in the driver's loader DLL or, when a game
// links the static NGX library, in the game executable itself -- which is where
// BG3 keeps them. Rather than guess, walk every loaded module and take whichever
// one exports the symbol.
// Hooks every module that exports the NGX D3D11 API and is not already hooked,
// and returns how many were added this pass. Modules appear at different times
// -- a game's own exports are there from the start, the feature snippet only
// once DLSS initialises -- so this runs again on every DLL load.
//
// Earlier builds picked one module and tried to reason about which layer the
// caller would enter. That reasoning was wrong for Trails (hooked too low) and
// wrong for Skyrim (hooked too high). The layers are indistinguishable from the
// outside, so all of them are hooked and g_nest decides at call time.
// An entry point whose first fourteen bytes are one filler byte repeated has no
// code there: 0x90 is nop and 0xCC is int3, and no real function begins with
// fourteen of either. The driver's nvngx.dll -- the redirector, not the loader
// _nvngx.dll -- exports two NGX names into such sleds, at addresses that are not
// even function-aligned. Writing a fourteen-byte jump into padding corrupts
// whatever the padding belongs to.
static bool IsFillerStub(const void *fn)
{
    if (fn == nullptr) return false;
    const BYTE *p = static_cast<const BYTE *>(fn);
    if (p[0] != 0x90 && p[0] != 0xCC) return false;
    for (int i = 1; i < 14; ++i)
        if (p[i] != p[0]) return false;
    return true;
}

static volatile bool g_shutting_down = false;

// Kept by the caller until after g_hook_cs is released: dropping the last
// reference can run arbitrary DLL_PROCESS_DETACH code and acquire other locks.
struct ScanModuleRefs
{
    NgxModuleScanner scanner;
    HMODULE modules[1024] = {};
    DWORD count = 0;
    NgxScanResult Acquire(HMODULE module)
    {
        if (count == _countof(modules)) return NgxScanResult::skipped;
        HMODULE held = nullptr;
        const NgxScanResult result = scanner.Acquire(module, g_vk_mirror != 0, held);
        if (result == NgxScanResult::acquired) modules[count++] = held;
        return result;
    }
    ~ScanModuleRefs()
    {
        for (DWORD i = 0; i < count; ++i) FreeLibrary(modules[i]);
    }
};

static int HookNewNgxModules(ScanModuleRefs &held)
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32 == nullptr) return 0;
    auto enum_modules =
        reinterpret_cast<PFN_EnumProcessModules>(GetProcAddress(k32, "K32EnumProcessModules"));
    if (enum_modules == nullptr) return 0;

    HMODULE mods[1024];
    DWORD   needed = 0;
    if (!enum_modules(GetCurrentProcess(), mods, sizeof(mods), &needed))
        return 0;

    int added = 0;
    const DWORD count = (needed < sizeof(mods) ? needed : sizeof(mods)) / sizeof(HMODULE);
    for (DWORD i = 0; i < count; ++i)
    {
        if (g_shutting_down) break;
        const NgxScanResult acquired = held.Acquire(mods[i]);
        if (acquired == NgxScanResult::retry)
        {
            InterlockedExchange(&g_scan_pending, 1);
            break;
        }
        if (acquired == NgxScanResult::unavailable)
        {
            static bool said = false;
            if (!said) { said = true; Log("NGX module discovery unavailable: loader-lock probe failed."); }
            break;
        }
        if (acquired != NgxScanResult::acquired) continue;
        bool slot_available = g_layer_count < kMaxLayers;
        for (LONG k = 0; k < g_layer_count && !slot_available; ++k)
            slot_available = g_layer[k].mod == nullptr;
        if (!slot_available)
        {
            static bool said = false;
            if (!said) { said = true; Log("  layer table full at %d; later modules "
                                          "exporting NGX are NOT hooked.", kMaxLayers); }
            break;
        }

        void *eval   = reinterpret_cast<void *>(GetProcAddress(mods[i], "NVSDK_NGX_D3D11_EvaluateFeature"));
        void *eval_c = reinterpret_cast<void *>(GetProcAddress(mods[i], "NVSDK_NGX_D3D11_EvaluateFeature_C"));
        void *create = reinterpret_cast<void *>(GetProcAddress(mods[i], "NVSDK_NGX_D3D11_CreateFeature"));

        // The Vulkan API, only when the mirror is switched on. There is no
        // NVSDK_NGX_VULKAN_EvaluateFeature_C in any driver -- there is no _C
        // export for any API, because _C lives in the SDK's static client
        // library -- but a game that links NGX statically can re-export one, so
        // it is looked for per module and simply expected to be absent.
        void *vk_eval    = nullptr;
        void *vk_eval_c  = nullptr;
        void *vk_create  = nullptr;
        void *vk_create1 = nullptr;
        if (g_vk_mirror != 0)
        {
            vk_eval    = reinterpret_cast<void *>(GetProcAddress(mods[i], "NVSDK_NGX_VULKAN_EvaluateFeature"));
            vk_eval_c  = reinterpret_cast<void *>(GetProcAddress(mods[i], "NVSDK_NGX_VULKAN_EvaluateFeature_C"));
            vk_create  = reinterpret_cast<void *>(GetProcAddress(mods[i], "NVSDK_NGX_VULKAN_CreateFeature"));
            vk_create1 = reinterpret_cast<void *>(GetProcAddress(mods[i], "NVSDK_NGX_VULKAN_CreateFeature1"));
        }

        const bool has_d11 = create != nullptr && (eval != nullptr || eval_c != nullptr);
        const bool has_vk  = (vk_create != nullptr || vk_create1 != nullptr) &&
                             (vk_eval != nullptr || vk_eval_c != nullptr);
        if (!has_d11 && !has_vk) continue;

        // Mandatory on Vulkan rather than optional: in this driver build EVERY
        // NGX export of nvngx.dll -- the redirector, not the loader -- is a 0x90
        // sled at an address that is not even function-aligned, the Vulkan names
        // included. The peer project has no such guard and patches fourteen
        // bytes of that padding.
        const bool filler = IsFillerStub(create) || IsFillerStub(eval) || IsFillerStub(eval_c) ||
                            IsFillerStub(vk_create) || IsFillerStub(vk_create1) ||
                            IsFillerStub(vk_eval) || IsFillerStub(vk_eval_c);

        bool known = AlreadyRejected(mods[i]);
        for (LONG k = 0; k < g_layer_count && !known; ++k)
        {
            if (g_layer[k].mod == mods[i])
            {
                if (g_layer[k].pending_retirement)
                {
                    if (InterlockedCompareExchange(&g_in_flight_trampoline_calls, 0, 0) == 0 &&
                        RetireLayerHooks(g_layer[k]))
                    {
                        InterlockedExchangePointer(&g_layer_modules[k], nullptr);
                        g_layer[k] = {};
                    }
                    else
                    {
                        Log("NGX layer %ld at %p reload deferred; hook retirement has not completed.", k, mods[i]);
                        known = true;
                    }
                }
                else
                {
                    known = true;
                }
            }
        }
        if (known) continue;

        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(mods[i], path, MAX_PATH);
        const wchar_t *leaf = wcsrchr(path, L'\\');
        leaf = leaf ? leaf + 1 : path;

        // FG exports the generic NGX names too, but cannot carry SR calls.
        // Patching its implementation makes Vulkan FG creation return
        // FAIL_PlatformError; leave the shared loader hooked, not this snippet.
        if (_wcsicmp(leaf, L"nvngx_dlssg.dll") == 0)
        {
            Log("  leaving %ls untouched: Frame Generation is not an SR entry point.", leaf);
            RememberRejected(mods[i]);
            continue;
        }

        // A game that links the NGX client statically exports these from its own
        // executable, and hooking them writes into the game's code section. The
        // Elder Scrolls Online does, and it is the only reported title that dies
        // with no exception, no shutdown marker and a log that simply stops --
        // which is what a process terminated by an integrity check looks like.
        // Hold it back: the driver's loader and the feature snippet are hooked as
        // their own layers and should carry the same calls. If nothing has called
        // DLSS a minute later, ReportIdle un-rejects this and comes back.
        if (mods[i] == GetModuleHandleW(nullptr) && !g_force_exe && g_cfg.skip_exe != 0)
        {
            Log("  holding off on %ls: it is the host executable rather than a "
                "library, so its NGX entry points sit in the game's own code "
                "section. The driver's loader and the feature snippet are hooked "
                "as their own layers and should carry the same calls. If nothing "
                "calls DLSS within a minute, this one is hooked after all rather "
                "than leaving the add-on doing nothing.", leaf);
            if (g_cfg.skip_exe == 1) g_deferred_exe = mods[i];
            RememberRejected(mods[i]);
            continue;
        }

        // The downloaded models under ProgramData are DLLs that export the full
        // API too, but nothing ever calls them as an entry point. They would eat
        // layer slots the real callers need.
        const wchar_t *ext = wcsrchr(leaf, L'.');
        if ((ext != nullptr && _wcsicmp(ext, L".bin") == 0) ||
            wcsstr(path, L"\\NGX\\models\\") != nullptr)
        { RememberRejected(mods[i]); continue; }

        // Two of these names arriving at one address means the module is not a
        // real implementation: the driver's nvngx_dlssg.dll points both
        // CreateFeature and EvaluateFeature at a six-byte "mov eax,1 ; ret"
        // placeholder. Patching one address twice saves the first patch as the
        // second's original bytes, so the function is redirected for good and
        // the two detours receive each other's arguments -- which is what makes
        // NGX fault while it is loading its own snippets.
        //
        // Skipping the module is right on both counts: the double patch cannot
        // happen, and a stub that answers 1 to everything was never going to
        // carry a DLSS call worth intercepting.
        if (filler)
        {
            Log("  skipping %ls: its NGX entry points are filler bytes, not code.", leaf);
            RememberRejected(mods[i]);
            continue;
        }

        if ((eval != nullptr && eval == create) ||
            (eval_c != nullptr && eval_c == create) ||
            (eval != nullptr && eval == eval_c))
        {
            Log("  skipping %ls: it exports more than one NGX entry point at %p, so "
                "it is a placeholder rather than an implementation.", leaf,
                eval != nullptr ? eval : eval_c);
            RememberRejected(mods[i]);
            continue;
        }

        // Slots used to be handed out forward only, so every unload and reload
        // of an NGX module burned one permanently and a long session could run
        // out of a table it was no longer using. ForgetUnloadedLayer nulls mod,
        // which is what makes a slot free. Reset every hook before reuse: the
        // replacement module may not export all the old module's entry points.
        LONG slot = g_layer_count;
        for (LONG k = 0; k < g_layer_count; ++k)
            if (g_layer[k].mod == nullptr) { slot = k; break; }

        Layer &L = g_layer[slot];
        L = {};
        L.mod = mods[i];
        InterlockedExchange(&g_layer_unloaded[slot], 0);
        InterlockedExchangePointer(&g_layer_modules[slot], mods[i]);

        Log("NGX layer %ld: %ls (base=%p)", slot, path, static_cast<void *>(mods[i]));
        {
            char ver[64];
            if (FileVersionString(path, ver, sizeof(ver)))
            {
                Log("  version %s", ver);
                NoteSnippetVersion(leaf, ver);
            }
        }
        Log("  NVSDK_NGX_D3D11_CreateFeature     = %p", create);
        Log("  NVSDK_NGX_D3D11_EvaluateFeature   = %p", eval);
        Log("  NVSDK_NGX_D3D11_EvaluateFeature_C = %p", eval_c);
        if (create != nullptr) LogPrologue("CreateFeature", static_cast<const BYTE *>(create));
        if (eval   != nullptr) LogPrologue("EvaluateFeature", static_cast<const BYTE *>(eval));
        if (eval_c != nullptr) LogPrologue("EvaluateFeature_C", static_cast<const BYTE *>(eval_c));

        EnterCriticalSection(&g_hook_cs);
        const bool ok_create = create != nullptr &&
            HookInstall(L.create, create, reinterpret_cast<void *>(kDetour[slot].create));
        const bool ok_eval = eval != nullptr &&
            HookInstall(L.eval, eval, reinterpret_cast<void *>(kDetour[slot].eval));
        const bool ok_eval_c = eval_c != nullptr &&
            HookInstall(L.eval_c, eval_c, reinterpret_cast<void *>(kDetour[slot].eval_c));
        LeaveCriticalSection(&g_hook_cs);

        // "absent" rather than "FAILED" for a null CreateFeature, which used to be
        // impossible -- the accept test above required it -- and is not any more:
        // a module can now be accepted for its Vulkan exports alone.
        Log("  hooked: CreateFeature=%s EvaluateFeature=%s EvaluateFeature_C=%s",
            create != nullptr ? (ok_create ? "yes" : "FAILED") : "absent",
            eval   != nullptr ? (ok_eval   ? "yes" : "FAILED") : "absent",
            eval_c != nullptr ? (ok_eval_c ? "yes" : "FAILED") : "absent");

        // Said and done only where there is something to say, so a session with
        // vk_mirror off -- which is every D3D11 session -- logs exactly what it
        // logged before this existed.
        if (has_vk)
        {
            Log("  NVSDK_NGX_VULKAN_CreateFeature     = %p", vk_create);
            Log("  NVSDK_NGX_VULKAN_CreateFeature1    = %p", vk_create1);
            Log("  NVSDK_NGX_VULKAN_EvaluateFeature   = %p", vk_eval);
            Log("  NVSDK_NGX_VULKAN_EvaluateFeature_C = %p", vk_eval_c);
            if (vk_create  != nullptr) LogPrologue("VK CreateFeature", static_cast<const BYTE *>(vk_create));
            if (vk_create1 != nullptr) LogPrologue("VK CreateFeature1", static_cast<const BYTE *>(vk_create1));
            if (vk_eval    != nullptr) LogPrologue("VK EvaluateFeature", static_cast<const BYTE *>(vk_eval));
            if (vk_eval_c  != nullptr) LogPrologue("VK EvaluateFeature_C", static_cast<const BYTE *>(vk_eval_c));

            EnterCriticalSection(&g_hook_cs);
            const bool okv_create = vk_create != nullptr &&
                HookInstall(L.vk_create, vk_create, kDetour[slot].vk_create);
            const bool okv_create1 = vk_create1 != nullptr &&
                HookInstall(L.vk_create1, vk_create1, kDetour[slot].vk_create1);
            const bool okv_eval = vk_eval != nullptr &&
                HookInstall(L.vk_eval, vk_eval, kDetour[slot].vk_eval);
            const bool okv_eval_c = vk_eval_c != nullptr &&
                HookInstall(L.vk_eval_c, vk_eval_c, kDetour[slot].vk_eval_c);
            LeaveCriticalSection(&g_hook_cs);

            Log("  hooked: VULKAN CreateFeature=%s CreateFeature1=%s EvaluateFeature=%s "
                "EvaluateFeature_C=%s",
                vk_create  != nullptr ? (okv_create  ? "yes" : "FAILED") : "absent",
                vk_create1 != nullptr ? (okv_create1 ? "yes" : "FAILED") : "absent",
                vk_eval    != nullptr ? (okv_eval    ? "yes" : "FAILED") : "absent",
                vk_eval_c  != nullptr ? (okv_eval_c  ? "yes" : "FAILED") : "absent");
        }

        if (slot == g_layer_count) InterlockedIncrement(&g_layer_count);
        ++added;
    }
    return added;
}

static void LogPrologue(const char *label, const BYTE *p)
{
    char hex[64];
    int  n = 0;
    for (int i = 0; i < 14; ++i)
        n += _snprintf_s(hex + n, sizeof(hex) - n, _TRUNCATE, "%02X ", p[i]);

    // A jump where the prologue should be means something else hooked this
    // first. Chaining onto it can work, but when it does not this is the line
    // that explains why, so it is worth saying out loud.
    const bool detoured = (p[0] == 0xE9) || (p[0] == 0xFF && p[1] == 0x25) ||
                          (p[0] == 0x48 && p[1] == 0xB8) || (p[0] == 0xEB);
    const bool thunk = detoured && JumpsInsideOwnModule(p);
    Log("  %-16s prologue: %s%s", label, hex,
        thunk ? " <== a thunk into its own module, not a hook"
              : detoured ? " <== already hooked by something else" : "");
    // The target as well as the fact. "Already hooked" with no name has cost two
    // issue threads a round trip each -- Batman: Arkham Knight (#7) and Final
    // Fantasy XV (#11) both show one and neither log says by whom.
    if (detoured) LogDetourTarget(p);
}

// A log that only describes this add-on cannot diagnose a setup problem. These
// two record what the machine and the folder actually look like, because the
// most common failure is not a bug here but a missing file next to it.
typedef LONG (WINAPI *PFN_RtlGetVersion)(OSVERSIONINFOEXW *);

static void LogEnvironment()
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    Log("  host: %ls", exe);

    // Whose D3D11 this is. A wrapper in the game folder -- ENB, a proxy -- sits
    // underneath everything else here, and is worth naming in any title rather
    // than guessed at from a filename check.
    {
        wchar_t d3d[MAX_PATH] = {};
        if (GetModuleFileNameW(GetModuleHandleW(L"d3d11.dll"), d3d, MAX_PATH) != 0)
            Log("  d3d11.dll: %ls", d3d);
    }

    // Which ReShade this add-on registered with, and whether it had a choice.
    // Two of them in one process is not exotic: a proxy DLL beside the executable
    // and the machine-wide Vulkan layer both export ReShadeRegisterAddon, and the
    // first one enumerated wins. Which one that is decides whether the effect
    // runtime this add-on sees belongs to the game's renderer or to a secondary
    // device the game made for something else.
    if (g_reshade_path[0] != 0)
    {
        Log("  reshade: %ls", g_reshade_path);
        if (g_reshade_others != 0)
            Log("    %lu other module%s in this process also export%s "
                "ReShadeRegisterAddon. Two ReShades is the shape that leaves this "
                "add-on on the wrong device; if the mirror later reports a runtime "
                "that is not the game's renderer, this line is why.",
                g_reshade_others, g_reshade_others == 1 ? "" : "s",
                g_reshade_others == 1 ? "s" : "");
    }

    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
    {
        if (auto rtl = reinterpret_cast<PFN_RtlGetVersion>(GetProcAddress(nt, "RtlGetVersion")))
        {
            OSVERSIONINFOEXW vi = {};
            vi.dwOSVersionInfoSize = sizeof(vi);
            if (rtl(&vi) == 0)
                // Windows 11 still reports major version 10; only the build
                // separates them. Reports have to be comparable at a glance, and
                // "10.0 build 19044" is not obviously a different operating
                // system from "10.0 build 26200".
                Log("  windows: %s, %lu.%lu build %lu",
                    vi.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10",
                    vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
        }
        // Wine says so through an export of its own ntdll. Under Proton the D3D12
        // runtime is vkd3d-proton, and the neural pass has a known wall there
        // that is not this add-on's; a report that carries this line skips a
        // round trip.
        typedef const char *(*PFN_wine_get_version)();
        if (auto wv = reinterpret_cast<PFN_wine_get_version>(GetProcAddress(nt, "wine_get_version")))
            Log("  wine: %s. The D3D12 runtime here is vkd3d-proton; see dlss5-bridge #22 "
                "for where the neural pass stands under Proton.", wv());
    }
}

// SHA-256 of a file, for identifying a build that its own version resource does
// not. Two copies of one DLSS 5 add-on shipped on 2026-08-28 both declare
// 0.2026.0828.0517 and are 29 KB apart, and one of them writes nothing -- so the
// version is not an identity and the content has to be.
//
// Streamed in 1 MB chunks and capped: this runs at attach, beside a 165 MB
// neural-rendering snippet that must never be read here. Only .addon64 files
// reach it.
static bool Sha256File(const wchar_t *path, char *out_hex, size_t cch)
{
    if (cch < 65) return false;
    out_hex[0] = 0;

    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz = {};
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart > (64ll << 20)) { CloseHandle(f); return false; }

    BCRYPT_ALG_HANDLE  alg = nullptr;
    BCRYPT_HASH_HANDLE h   = nullptr;
    bool ok = false;
    BYTE digest[32] = {};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0)
    {
        BYTE *buf = static_cast<BYTE *>(malloc(1 << 20));
        if (buf != nullptr)
        {
            ok = true;
            DWORD got = 0;
            while (ReadFile(f, buf, 1 << 20, &got, nullptr) && got != 0)
                if (BCryptHashData(h, buf, got, 0) != 0) { ok = false; break; }
            free(buf);
            if (ok && BCryptFinishHash(h, digest, sizeof(digest), 0) != 0) ok = false;
        }
    }
    if (h   != nullptr) BCryptDestroyHash(h);
    if (alg != nullptr) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    if (!ok) return false;

    static const char kHex[] = "0123456789ABCDEF";
    for (int i = 0; i < 32; ++i)
    { out_hex[i * 2] = kHex[digest[i] >> 4]; out_hex[i * 2 + 1] = kHex[digest[i] & 0xF]; }
    out_hex[64] = 0;
    return true;
}

// The DLSS 5 add-on builds this one has actually been run against, and what
// happened. Not an authority and not a compatibility list: it is what was
// measured on one machine, and it says so in the line it prints.
//
// It exists because the failure it names is silent from every other angle. A
// build that attaches, reports itself active in its own panel and writes nothing
// looks exactly like a working one from inside this add-on -- and cost an
// evening before the difference turned out to be two builds under one version.
// needs_proxy: the build recycles its scratch buffers off queue submissions it
// learns about from ReShade, so it has to see this add-on's private D3D12 device
// through ReShade's proxy rather than underneath it. Measured 2026-09-02 in
// ngxGym on both backends: with the proxy stripped it attached, evaluated once
// and wrote nothing -- output byte-identical to no add-on at all -- and with the
// proxy kept it installed its "native D3D12 queue submission tracker", evaluated
// every frame and changed the picture. The older build works either way.
struct KnownConsumer { const char *sha256; bool good; bool needs_proxy; const char *note; };
static const KnownConsumer kKnownConsumers[] = {
    { "245C06137AD13B1CA03AFAAD5100C1E8F0DCE8C11FE50A9272EA562F33CEA601", true, false,
      "measured 2026-09-02: neural rendering changes the picture through this bridge" },
    { "D5ADF82EB44B065F4C590AC91FE824BAB07AFEA0EB9F994BDE936710C8593952", true, true,
      "measured 2026-09-02: works only when this add-on keeps ReShade's proxy on its "
      "D3D12 device, which it now does for this build automatically. Underneath the "
      "proxy it attached, said active and wrote nothing" },
};

// "renodx-dlss5 (2).addon64" and "renodx-dlss5.addon64" are the same add-on. The
// suffix is what Windows and every browser append when a download would overwrite
// something, and it is the one rename that produces a second loaded copy of a
// thing whose author only ever shipped one.
static void StripDuplicateSuffix(const wchar_t *in, wchar_t *out, size_t cch)
{
    wcscpy_s(out, cch, in);
    wchar_t *dot = wcsrchr(out, L'.');
    wchar_t *end = dot != nullptr ? dot : out + wcslen(out);
    // " (12)" backwards: the digits, the space before the bracket, the bracket.
    if (end - out < 4 || *(end - 1) != L')') return;
    wchar_t *p = end - 2;
    if (!iswdigit(*p)) return;
    while (p > out && iswdigit(*p)) --p;
    if (*p != L'(' || p == out || *(p - 1) != L' ') return;
    const size_t keep = static_cast<size_t>(p - 1 - out);
    wcscpy_s(out + keep, cch - keep, dot != nullptr ? dot : L"");
}

// The pieces this bridge needs are supplied by other people and dropped in by
// hand, so listing which of them are actually present turns "it does nothing"
// into an answer.
static void LogNeighbours()
{
    wchar_t dir[MAX_PATH] = {};
    GetModuleFileNameW(g_self, dir, MAX_PATH);
    if (wchar_t *s = wcsrchr(dir, L'\\')) *(s + 1) = L'\0';

    // Only NVIDIA's own model files are named here. The DLSS 5 add-on's filename
    // belongs to its author and is not this add-on's to require: the listing of
    // every *.addon* below already shows what is present, and a red error in the
    // overlay naming one particular file is wrong for anyone using another.
    static const wchar_t *needed[] = {
        L"nvngx_dlssnr.dll",       // the neural-rendering model
        L"nvngx_dlss.dll",         // optional, a newer super-resolution model
    };

    // ReShade can be told to load add-ons from a directory that is not the game's
    // own, and Phantasy Star Online 2 is the first report where they differ. The
    // old text said "missing from the game folder" while the code had only looked
    // beside the add-on, so a file plainly sitting next to the executable was
    // announced as absent -- and the same log then loaded it from there.
    wchar_t exe_dir[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_dir, MAX_PATH);
    if (wchar_t *sl = wcsrchr(exe_dir, (wchar_t)92)) *(sl + 1) = 0;
    const bool split = _wcsicmp(exe_dir, dir) != 0;

    Log("  files next to this add-on:");
    if (split)
        Log("    the add-on is not in the game's own folder, so both are checked:");
    for (int i = 0; i < 2; ++i)
    {
        const wchar_t *n = needed[i];
        wchar_t p[MAX_PATH];
        wcscpy_s(p, dir);
        wcscat_s(p, n);
        const bool here = GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;

        bool at_exe = false;
        if (!here && split)
        {
            wchar_t q[MAX_PATH];
            wcscpy_s(q, exe_dir);
            wcscat_s(q, n);
            at_exe = GetFileAttributesW(q) != INVALID_FILE_ATTRIBUTES;
        }

        if (here)        LogFileVersion(dir, n, "present");
        else if (at_exe) LogFileVersion(exe_dir, n, "present beside the game");
        else             Log("    %-26ls MISSING from both", n);

        // The model file is NVIDIA's, and its name is fixed by NVIDIA rather than
        // by anyone's add-on, so its absence can be raised where it is seen.
        if (!here && !at_exe && i == 0)
            Warn("%ls is in neither the add-on's folder nor the game's. The DLSS 5 "
                 "add-on loads its neural-rendering model from that file.", n);
    }

    // Everything else that could be taking part, so conflicts are visible.
    wchar_t pattern[MAX_PATH];
    wcscpy_s(pattern, dir);
    wcscat_s(pattern, L"*.addon*");
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        Log("  add-ons present:");
        wchar_t dup[32][MAX_PATH];
        int     dup_n = 0;
        do
        {
            LogFileVersion(dir, fd.cFileName, "");

            // Where each add-on is mapped, so a fault address can be turned into
            // an offset by whoever owns that add-on. Prey 2017 faults inside
            // Luma-Prey.addon during NGX initialisation and the address alone
            // told its author nothing. Only loaded add-ons have a base; one that
            // ReShade has not loaded yet simply has no line.
            HMODULE m = GetModuleHandleW(fd.cFileName);
            if (m != nullptr)
                Log("        loaded at %p", static_cast<void *>(m));

            // The same two facts kept for the status panel. Which of these is
            // the DLSS 5 add-on is deliberately not decided here: its filename
            // belongs to its author, so the panel shows the list and lets the
            // reader recognise it, exactly as the log above does.
            {
                char entry[160];
                _snprintf_s(entry, sizeof(entry), _TRUNCATE, "%ls -- %s", fd.cFileName,
                            m != nullptr ? "loaded" : "present, not loaded");
                PanelAppend(g_panel_addons, sizeof(g_panel_addons), entry);
            }
            // The browser-duplicate shape, collected as it goes. A re-downloaded
            // add-on lands as "name (2).addon64" beside the one it was meant to
            // replace, and ReShade loads BOTH: two copies detouring the same NGX
            // entry points, each seeing the other's feature and matching neither.
            //
            // Found in Baldur's Gate 3 on 2026-09-01, where the folder held
            // "renodx-dlss5 (2).addon64" and Red Dead Redemption 2 held a
            // "renodx-dlss5.addon64" of a different size under the same declared
            // version. The symptom reported was a DLSS 5 add-on whose panel said
            // active while the picture did not change.
            if (dup_n < static_cast<int>(_countof(dup)))
            {
                wcscpy_s(dup[dup_n], fd.cFileName);
                ++dup_n;
            }

            // What this add-on has actually been run against. Only .addon64
            // files, and never this one: hashing ourselves says nothing, and the
            // neural-rendering snippet beside us is 165 MB.
            {
                const size_t n_leaf = wcslen(fd.cFileName);
                const bool is_addon = n_leaf > 8 &&
                    _wcsicmp(fd.cFileName + n_leaf - 8, L".addon64") == 0;
                wchar_t self_leaf[MAX_PATH] = L"";
                if (GetModuleFileNameW(g_self, self_leaf, MAX_PATH) != 0)
                    if (const wchar_t *sl = wcsrchr(self_leaf, L'\\'))
                        memmove(self_leaf, sl + 1, (wcslen(sl + 1) + 1) * sizeof(wchar_t));
                if (is_addon && _wcsicmp(fd.cFileName, self_leaf) != 0)
                {
                    wchar_t full[MAX_PATH];
                    wcscpy_s(full, dir);
                    wcscat_s(full, fd.cFileName);
                    char hex[72] = "";
                    if (Sha256File(full, hex, sizeof(hex)))
                    {
                        const KnownConsumer *k = nullptr;
                        for (size_t q = 0; q < _countof(kKnownConsumers); ++q)
                            if (_stricmp(kKnownConsumers[q].sha256, hex) == 0)
                            { k = &kKnownConsumers[q]; break; }

                        // Short for the unknown case, long for a known bad one.
                        // Every add-on in the folder passes through here, most of
                        // them nothing to do with DLSS 5, and a paragraph each
                        // would be noise that trains a reader to skip the block.
                        if (k == nullptr)
                            Log("        sha256 %s -- not on this add-on's measured "
                                "list, which is one machine's experience and not a "
                                "compatibility statement. If its panel says active "
                                "while the picture does not change, unwrap=0 in "
                                "dlss5-bridge.cfg is the first thing to try.", hex);
                        else if (k->good)
                            Log("        sha256 %s -- %s.", hex, k->note);
                        else
                            Log("        *** sha256 %s -- %s. ***", hex, k->note);
                        if (k != nullptr && k->needs_proxy) g_consumer_needs_proxy = true;

                        // The panel says it too, in three words. Somebody looking at
                        // a picture that will not change is not reading a log.
                        {
                            char tag[96];
                            _snprintf_s(tag, sizeof(tag), _TRUNCATE, "%ls -- %s",
                                        fd.cFileName,
                                        k == nullptr ? "untested build"
                                        : k->good ? "measured working"
                                                  : "MEASURED TO WRITE NOTHING");
                            PanelAppend(g_panel_addons, sizeof(g_panel_addons), tag);
                        }
                    }
                }
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);

        // Two DIFFERENT DLSS 5 add-ons is the other shape: renodx-dlss5 beside
        // renodx-dlssfix, each detouring the same NGX D3D12 entry points. Under
        // that pair the bridge's own feature create faulted inside D3D12Core
        // before anything ran (Baldur's Gate 3, D3D11, #16). Which of the two is
        // the trigger is not known; two at once is the thing to undo first.
        {
            int n_dlss5 = 0;
            for (int i = 0; i < dup_n; ++i)
                if (_wcsnicmp(dup[i], L"renodx-dlss", 11) == 0) ++n_dlss5;
            if (n_dlss5 >= 2)
                Log("  *** %d DLSS 5 add-ons are in this folder. Each detours the same NGX "
                    "D3D12 entry points, and with two loaded at once this add-on's feature "
                    "create has faulted inside D3D12Core (#16). Keep one. ***", n_dlss5);
        }

        // Names compared after a trailing " (N)" is stripped from the stem. Two
        // that collide are the same add-on twice.
        for (int i = 0; i < dup_n; ++i)
            for (int j = i + 1; j < dup_n; ++j)
            {
                wchar_t a[MAX_PATH], b[MAX_PATH];
                StripDuplicateSuffix(dup[i], a, MAX_PATH);
                StripDuplicateSuffix(dup[j], b, MAX_PATH);
                if (_wcsicmp(a, b) != 0) continue;
                Log("  *** %ls and %ls are the same add-on twice -- one is a copy that a "
                    "browser or an unpack renamed rather than replaced. ReShade loads every "
                    "add-on it finds, so BOTH are running and both are detouring the same "
                    "entry points. Delete the one you did not mean to keep. ***",
                    dup[i], dup[j]);
            }
    }

    LogReShadeConfig(dir);
}

// When the D3D11 entry points are not found, the useful question is what IS
// loaded. Reports that only say "not found" cannot be acted on; this turns the
// next one into evidence.
static void LogNgxCandidates()
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto enum_modules =
        reinterpret_cast<PFN_EnumProcessModules>(GetProcAddress(k32, "K32EnumProcessModules"));
    if (enum_modules == nullptr) return;

    HMODULE mods[1024];
    DWORD   needed = 0;
    if (!enum_modules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return;

    Log("  modules that expose any NGX or DLSS entry point:");
    bool any = false;
    for (DWORD i = 0; i < needed / sizeof(HMODULE); ++i)
    {
        const bool d3d11 = GetProcAddress(mods[i], "NVSDK_NGX_D3D11_CreateFeature") != nullptr;
        const bool d3d12 = GetProcAddress(mods[i], "NVSDK_NGX_D3D12_CreateFeature") != nullptr;
        const bool vk    = GetProcAddress(mods[i], "NVSDK_NGX_VULKAN_CreateFeature") != nullptr;

        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(mods[i], path, MAX_PATH);

        // Streamline sits above NGX, so a game using it looks different from
        // one calling NGX directly. Worth naming even without NGX exports.
        const wchar_t *leaf = wcsrchr(path, L'\\');
        leaf = leaf ? leaf + 1 : path;
        const bool interesting = (_wcsnicmp(leaf, L"sl.", 3) == 0) ||
                                 (wcsstr(leaf, L"nvngx") != nullptr);

        if (!d3d11 && !d3d12 && !vk && !interesting) continue;

        // The Elder Scrolls Online exports the NGX entry points from eso64.exe
        // itself: it links the NGX client statically rather than loading it.
        // Hooking those means writing fourteen bytes into the game's own code
        // section, and ESO is the only reported title that dies with no
        // exception, no shutdown marker and a log that simply stops -- which is
        // what a process terminated by an integrity check looks like.
        //
        // Skipping costs little. A statically linked NGX client still reaches
        // the driver's _nvngx.dll and the feature snippet, and both are hooked
        // as their own layers; ESO's log shows all three present. If a game ever
        // exposes NGX nowhere else, the idle diagnosis below says so and the
        // result is an add-on that does nothing rather than a dead game.
        Log("    %ls  ->%s%s%s%s", path, d3d11 ? " D3D11" : "", d3d12 ? " D3D12" : "",
            vk ? " VULKAN" : "",
            (!d3d11 && !d3d12 && !vk) ? " (no NGX exports)" : "");
        any = true;
    }
    if (!any)
        Log("    none. NGX is not loaded in this process, so DLSS has not been "
            "initialised -- check that DLSS is actually enabled in the game.");
}
// ---------------------------------------------------------------------------
// Finding NGX
//
// NGX is loaded well after the process starts, so the entry points are not
// there to hook at attach time. This used to poll on a background thread, which
// was wrong: an add-on can be unloaded at any moment -- Skyrim loads and drops
// them inside a 400 ms hardware-detection pass -- and a thread still executing
// inside the module when it leaves memory kills the process.
//
// The loader will say when a library arrives instead. No thread, so no window
// in which the module can vanish underneath one, and unregistering is ordered.
// ---------------------------------------------------------------------------

typedef void (CALLBACK *PFN_LdrDllNotification)(ULONG reason, const void *data, void *ctx);
typedef LONG (NTAPI *PFN_LdrRegisterDllNotification)(ULONG, PFN_LdrDllNotification, void *, void **);
typedef LONG (NTAPI *PFN_LdrUnregisterDllNotification)(void *);

static void *g_ldr_cookie;
static PFN_LdrUnregisterDllNotification g_ldr_unregister;
static volatile LONG g_hooks_installed;
static ULONGLONG     g_hook_time;
static volatile LONG g_idle_reported;
// The add-on shipped as dlss5-dx11-bridge.addon64 until 1.1.0, and renaming the
// file does not delete the old one. ReShade loads every add-on it finds, so both
// end up in the process, each writing 14-byte jumps over the same NGX entry points
// and each recording whatever it found there as the original bytes.
//
// This WARNS and does not prevent, which is the honest thing it can do. Two
// placements were tried on 2026-08-31 in Baldur's Gate 3 and neither works:
//
//   at attach   -- add-ons load in name order and "dlss5-bridge" sorts before
//                  "dlss5-dx11-bridge", so the old copy is not in the process yet.
//   at hook time -- assumed to be later, and is not: the driver's _nvngx.dll was
//                  already loaded, so layer 0 was hooked 39 ms before the old
//                  add-on attached at all.
//
// There is no point between those two where the answer is both known and still
// actionable. Called from the frame path, where every add-on has certainly loaded,
// it can at least name the problem. Both copies did hook in that test and nothing
// crashed, so this is a warning about a hazard, not a report of a failure.
static void WarnIfOldCopyLoaded()
{
    static LONG said = 0;
    if (InterlockedCompareExchange(&said, 1, 0) != 0) return;

    if (HMODULE other = GetModuleHandleA("dlss5-dx11-bridge.addon64"))
        if (other != g_self)
            Log("WARNING: dlss5-dx11-bridge.addon64 is loaded as well as this one. That "
                "is this add-on under the name it used up to and including 1.1.0, and "
                "both have hooked the same NGX entry points over each other. Both also "
                "open their own D3D12 session and both deliver frames, and the one that "
                "loaded second is the outer detour -- so the OLDER build is what reaches "
                "the screen. Delete the old file from the game folder, or disable it in "
                "ReShade's add-on list. Settings are safe either way: the cfg is read "
                "under both names.");

    // The other redundancy, and the one that ends this project: a DLSS add-on that
    // reaches NGX by itself needs no bridge at all. Named rather than acted on,
    // deliberately -- whether it covers Vulkan as well as D3D11 is not established
    // here, and standing the bridge down on a guess would take Vulkan away from
    // somebody who still wants it. The user is told, and decides.
    if (GetModuleHandleA("renodx-dlss.addon64") != nullptr)
        Log("NOTE: renodx-dlss.addon64 is loaded, and it reaches NGX on its own rather "
            "than through a bridge. Where it serves the game, this add-on has nothing "
            "to add and can be removed. It is kept loaded here because whether it "
            "covers every API this bridge does has not been checked.");
}

// Only the owned worker installs hooks, outside loader notifications and detours.
static void TryInstallHooks()
{
    if (g_shutting_down) return;
    // Not "once and done" any more. The game's own exports are present from the
    // start, but the feature snippet underneath only shows up when DLSS
    // initialises, so every load is another chance to find a layer.

    // Do not compete with a render call holding hook state. Preserve the request
    // so the worker retries even when no NGX hook has been installed yet.
    ScanModuleRefs held;
    if (!TryEnterCriticalSection(&g_hook_cs))
    {
        InterlockedExchange(&g_scan_pending, 1);
        return;
    }
    const int added = HookNewNgxModules(held);
    LeaveCriticalSection(&g_hook_cs);
    if (added == 0 || g_shutting_down) return;

    if (InterlockedCompareExchange(&g_hooks_installed, 1, 0) != 0) return;

    g_hook_time = GetTickCount64();

    // Streamline is a second way to reach DLSS, and it was thought to be out of
    // reach: it does not call the game's own NGX exports. It does, however, link
    // NVIDIA's NGX D3D11 client inside sl.common.dll and call
    // NVSDK_NGX_D3D11_EvaluateFeature on the feature snippet -- a module hooked
    // since every layer became a target, so the call does arrive here. Worth
    // recording because the call then comes from Streamline rather than from the
    // game, and the parameter block is Streamline's.
    if (GetModuleHandleW(L"sl.interposer.dll") != nullptr)
        Log("  sl.interposer.dll is loaded, so DLSS here is driven through Streamline. "
            "It reaches NGX through the feature snippet, which is hooked, so the calls "
            "below come from Streamline's own NGX client rather than from the game.");
}

// "DLSS is off, or the call goes somewhere else" leaves the reader to guess
// which, and the two need completely different answers. NGX loads nvngx_dlss.dll
// the moment DLSS actually starts, so its presence settles it: absent means DLSS
// never started and the switch is off wherever it lives; present means DLSS did
// start and is reaching NGX by a route these hooks do not sit on.
//
// The call counts separate a third case that looks the same from outside: a
// feature created and then never evaluated is not the same failure as one that
// was never created.
static void SayWhyNothingCalled()
{
    Log("  CreateFeature calls: %ld, EvaluateFeature calls: %ld",
        InterlockedCompareExchange(&g_create_count, 0, 0),
        InterlockedCompareExchange(&g_eval_count, 0, 0));

    const bool streamline = GetModuleHandleW(L"sl.interposer.dll") != nullptr;

    if (GetModuleHandleW(L"nvngx_dlss.dll") == nullptr)
    {
        Log("  nvngx_dlss.dll is not loaded, so no DLSS model has been asked for yet.");
        if (streamline)
            Log("  sl.interposer.dll is, so an upscaler runtime is initialised and idle.");
        // What is absent is a fact; why it is absent is not one this process can
        // see. It used to say "it is switched off wherever it is configured",
        // which is false for a game started without its script extender, where
        // nothing is switched off and the plugin simply never loaded.
        Log("  Something has to ask for upscaling before anything here can run: the");
        Log("  game's own setting, a mod's setting, or the mod not having loaded.");
    }
    else if (InterlockedCompareExchange(&g_create_count, 0, 0) > 0)
    {
        Log("  A DLSS feature was created and then never evaluated. That is a");
        Log("  different fault from DLSS not running: something started it and");
        Log("  stopped before the first frame.");
    }
    else if (streamline)
    {
        Log("  Streamline and a DLSS model are both loaded, so the upscaler is running");
        Log("  and nothing has asked it to upscale a frame.");
        Log("  Where an upscaler replaces the engine's own temporal antialiasing -- which");
        Log("  is how the Skyrim and Fallout mods work -- that is what a disabled TAA");
        Log("  setting looks like from here. This add-on cannot read that setting; it can");
        Log("  only say that the upscaler is idle.");
    }
    else
    {
        Log("  nvngx_dlss.dll is loaded, so NGX has initialised something. That is not");
        Log("  the same as the game rendering: a menu, a lobby or a loading screen can");
        Log("  sit here for minutes before the first DLSS call. If a level was already");
        Log("  running, then DLSS is reaching NGX somewhere these hooks do not sit --");
        Log("  a D3D12 path would do that. Streamline would not: it calls the same");
        Log("  D3D11 entry points, on the snippet, and those are hooked.");
    }
}

// The note above is written on a timer and can be wrong: Escape from Tarkov
// produced it from the menu and then called DLSS twenty-three seconds later.
// A log that corrects itself is worth more than one that states a conclusion
// and leaves it standing.
static void RetractIdleNote()
{
    if (InterlockedCompareExchange(&g_idle_reported, 0, 0) == 0) return;
    static LONG done = 0;
    if (InterlockedCompareExchange(&done, 1, 0) != 0) return;
    Log("");
    Log("Disregard the note above: DLSS has now called through these entry points.");
    Log("  It had simply not started yet when that was written.");
}

// The "nothing ever called us" diagnosis used to be written only at unload, and
// most games never run DLL detach -- so the one log that needed it said nothing.
// Report it as soon as it is true instead. There is no timer thread to hang it
// on by design, so it rides the loader notification: a game doing anything at
// all keeps loading DLLs.
static void ReportIdle()
{
    if (g_shutting_down) return;
    if (InterlockedCompareExchange(&g_hooks_installed, 0, 0) == 0 &&
        g_deferred_exe == nullptr) return;
    if (g_eval_count != 0) return;
    // Idle means nothing at all is running, not merely that the game's own DLSS
    // is not. A source that delivers frames without the game ever calling
    // EvaluateFeature leaves g_eval_count at zero for the whole session, and the
    // path below then hooks the host executable's own NGX exports -- which is
    // what this file records as killing The Elder Scrolls Online. Asking a
    // delivered-frame count rather than each source in turn keeps that correct
    // for a source nobody has written yet.
    if (g_frames_delivered != 0) return;
    if (GetTickCount64() - g_hook_time < 60000) return;
    if (InterlockedCompareExchange(&g_idle_reported, 1, 0) != 0) return;

    Log("");
    Log("60 seconds after hooking, nothing has called DLSS through these entry points.");

    // The host executable was held back because patching a game's own code
    // section is what an integrity check terminates a process for. A minute of
    // silence says the other layers are not carrying the calls, and an add-on
    // that does nothing is no better than one that takes the risk. Hook it.
    if (g_deferred_exe != nullptr)
    {
        Log("  The host executable's own NGX entry points were held back. Nothing "
            "has called DLSS through the driver's loader either, so they are being "
            "hooked now. skip_exe=0 hooks them from the start; skip_exe=2 never "
            "does, for a game that dies when they are patched.");
        g_force_exe = true;
        ForgetRejected(g_deferred_exe);
        g_deferred_exe = nullptr;
        TryInstallHooks();
        if (g_create_count > 0 || g_eval_count > 0) return;
    }
    SayWhyNothingCalled();
}

// Both the loaded and unloaded notifications carry this shape.
struct LdrDllNotificationData
{
    ULONG       Flags;
    const void *FullDllName;
    const void *BaseDllName;
    void       *DllBase;
    ULONG       SizeOfImage;
};

// A hooked module can be unloaded while this add-on still holds the address of
// a patched function and the bytes it saved from it. Calling through that
// address afterwards lands in memory that is gone -- an access violation whose
// address belongs to no module, which is exactly what Assetto Corsa reported.
// Writing the saved bytes back is no better. Drop the layer instead.
static void NameHookedLayerAt(const void *base, wchar_t *out, size_t cch)
{
    for (LONG i = 0; i < g_layer_count; ++i)
        if (g_layer[i].mod != nullptr && static_cast<const void *>(g_layer[i].mod) == base)
            _snwprintf_s(out, cch, _TRUNCATE,
                         L"NGX layer %ld, no longer a registered module", i);
}

static void TriggerHookScan();

static void ForgetUnloadedLayer(const void *base)
{
    if (!base) return;
    bool matched = false;
    for (LONG i = 0; i < kMaxLayers; ++i)
        if (InterlockedCompareExchangePointer(&g_layer_modules[i], nullptr, nullptr) == base)
        {
            InterlockedExchange(&g_layer_unloaded[i], 1);
            matched = true;
        }
    // In particular, never start another worker while our own last reference
    // is being released after a probe which installed no hooks.
    if (matched) TriggerHookScan();
}

static volatile LONG g_worker_running = 0;
static volatile bool g_watching_ngx = false;

static bool ProcessPendingRetirements()
{
    if (!TryEnterCriticalSection(&g_hook_cs)) return false;

    bool has_pending = false;
    for (LONG i = 0; i < g_layer_count; ++i)
        if (InterlockedExchange(&g_layer_unloaded[i], 0))
        {
            Layer &layer = g_layer[i];
            layer.pending_retirement = true;
            layer.eval.active = layer.eval_c.active = layer.create.active = false;
            layer.vk_eval.active = layer.vk_eval_c.active = layer.vk_create.active = layer.vk_create1.active = false;
        }
    for (LONG i = 0; i < g_layer_count; ++i)
    {
        if (g_layer[i].pending_retirement)
        {
            has_pending = true;
            break;
        }
    }
    LeaveCriticalSection(&g_hook_cs);

    if (!has_pending) return true;

    // Never wait for a render call while holding bridge or loader locks.
    if (InterlockedCompareExchange(&g_in_flight_trampoline_calls, 0, 0) != 0)
        return false;

    if (!TryEnterCriticalSection(&g_hook_cs)) return false;

    const bool drained = InterlockedCompareExchange(&g_in_flight_trampoline_calls, 0, 0) == 0;
    bool retired_all = drained;
    if (drained)
    {
        for (LONG i = 0; i < g_layer_count; ++i)
        {
            if (!g_layer[i].pending_retirement) continue;

            if (!RetireLayerHooks(g_layer[i]))
            {
                retired_all = false;
                continue;
            }

            InterlockedExchangePointer(&g_layer_modules[i], nullptr);
            g_layer[i] = {};
            Log("NGX layer %ld retirement finalized.", i);
        }
    }
    LeaveCriticalSection(&g_hook_cs);
    return retired_all;
}

static void FgtMaintenance();

static DWORD WINAPI HookWorkerProc(LPVOID module)
{
    for (;;)
    {
        InterlockedExchange(&g_scan_pending, 0);
        if (!g_shutting_down)
        {
            if (ProcessPendingRetirements())
            {
                TryInstallHooks();
                FgtMaintenance();
                ReportIdle();
            }
            else
            {
                InterlockedExchange(&g_scan_pending, 1);
            }
        }
        // Back off outside all locks when discovery yields to a loader or render thread.
        if (!g_shutting_down && InterlockedCompareExchange(&g_scan_pending, 0, 0)) Sleep(10);
        InterlockedExchange(&g_worker_running, 0);
        if (!g_shutting_down && InterlockedCompareExchange(&g_scan_pending, 0, 0) &&
            InterlockedCompareExchange(&g_worker_running, 1, 0) == 0) continue;
        // Atomic release + exit: there must be no return into an unloaded DLL.
        FreeLibraryAndExitThread(static_cast<HMODULE>(module), 0);
    }
}

static void TriggerHookScan()
{
    InterlockedExchange(&g_scan_pending, 1);
    if (!g_watching_ngx || g_shutting_down) return;
    if (InterlockedCompareExchange(&g_worker_running, 1, 0) != 0) return;
    HANDLE thread = StartModuleWorker(HookWorkerProc);
    if (thread) CloseHandle(thread);
    else InterlockedExchange(&g_worker_running, 0);
}

static void CALLBACK OnDllLoaded(ULONG reason, const void *data, void *)
{
    // 1 is LDR_DLL_NOTIFICATION_REASON_LOADED, 2 is UNLOADED. This runs under
    // the loader lock, on the thread doing the load.
    // Hook installation uses MinHook which freezes threads and modifies protection,
    // so it must execute outside the loader lock to prevent deadlocks.
    if (reason == 1)
    {
        TriggerHookScan();
    }
    else if (reason == 2 && data != nullptr)
    {
        ForgetUnloadedLayer(static_cast<const LdrDllNotificationData *>(data)->DllBase);
    }
}

static void StartWatchingForNgx()
{
    g_watching_ngx = true;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto reg = nt != nullptr ? reinterpret_cast<PFN_LdrRegisterDllNotification>(
        GetProcAddress(nt, "LdrRegisterDllNotification")) : nullptr;
    g_ldr_unregister = nt != nullptr ? reinterpret_cast<PFN_LdrUnregisterDllNotification>(
        GetProcAddress(nt, "LdrUnregisterDllNotification")) : nullptr;

    if (reg != nullptr) reg(0, &OnDllLoaded, nullptr, &g_ldr_cookie);
    else Log("loader notifications unavailable; NGX will only be found if it is already loaded");

    // Signal the worker to scan as soon as DllMain exits and releases the loader lock:
    TriggerHookScan();
}

static void StopWatchingForNgx()
{
    g_watching_ngx = false;
    g_shutting_down = true;
    if (g_ldr_cookie != nullptr && g_ldr_unregister != nullptr)
    {
        g_ldr_unregister(g_ldr_cookie);
        g_ldr_cookie = nullptr;
    }
    // No join in DllMain. A live worker owns a DLL reference, and installed
    // hooks pin the module, so normal detach cannot race either of them.

}

// Said at unload, when the answer is finally known, rather than guessed at from
// a timer partway through.
static void ReportOutcome()
{
    if (InterlockedCompareExchange(&g_hooks_installed, 0, 0) == 0)
    {
        Log("");
        Log("NGX D3D11 entry points never appeared while this add-on was loaded.");
        Log("  Nothing in this process ever provided DLSS through the D3D11 NGX path.");
        Log("  A game with no DLSS of its own needs a mod that injects it; a game that");
        Log("  has it needs it switched on. Streamline reaches these functions and is");
        Log("  not the explanation; a D3D12 renderer would not, but this is a D3D11 one.");
        LogNgxCandidates();
    }
    else if (g_eval_count == 0 &&
             InterlockedCompareExchange(&g_idle_reported, 1, 0) == 0)
    {
        Log("");
        Log("The entry points were hooked but nothing ever called DLSS through them.");
        SayWhyNothingCalled();
    }
}

// After bridge.inc, because it reads the config the mirror parses and reuses the
// mirror's pixel readback and its whole frame path. Down here rather than beside
// that include because arming has to ask three things the mirror keeps at file
// scope further down -- g_create_count, g_eval_count and g_hook_time -- and
// hoisting three existing declarations into bridge.h to feed one new reader is a
// larger change to the file the nine titles depend on than moving one include.
// Still above RegisterWithReShade, which is what fills in the module handle and
// the API version synth.inc needs.
#include "synth.inc"
// After synth.inc, which owns the private D3D12 session and the Vulkan
// transport the mirror reuses whole.
#include "vkmirror.inc"


// ---------------------------------------------------------------------------
// ReShade add-on registration
//
// Replicates what reshade.hpp's register_addon does, without the SDK: locate
// the module exporting ReShadeRegisterAddon and call it. The API version is
// negotiated downwards because ReShade rejects a version newer than its own.
// ---------------------------------------------------------------------------

typedef bool (*PFN_ReShadeRegisterAddon)(HMODULE, uint32_t);
typedef void (*PFN_ReShadeUnregisterAddon)(HMODULE);

static PFN_ReShadeUnregisterAddon g_unregister;

static bool RegisterWithReShade(HMODULE self)
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32 == nullptr) return false;
    auto enum_modules =
        reinterpret_cast<PFN_EnumProcessModules>(GetProcAddress(k32, "K32EnumProcessModules"));
    if (enum_modules == nullptr) return false;

    HMODULE mods[1024];
    DWORD   needed = 0;
    if (!enum_modules(GetCurrentProcess(), mods, sizeof(mods), &needed))
        return false;

    const DWORD count = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < count; ++i)
    {
        auto reg = reinterpret_cast<PFN_ReShadeRegisterAddon>(
            GetProcAddress(mods[i], "ReShadeRegisterAddon"));
        if (reg == nullptr) continue;

        for (uint32_t version = 18; version >= 5; --version)
        {
            if (reg(self, version))
            {
                g_unregister = reinterpret_cast<PFN_ReShadeUnregisterAddon>(
                    GetProcAddress(mods[i], "ReShadeUnregisterAddon"));
                g_reshade_log = reinterpret_cast<PFN_ReShadeLogMessage>(
                    GetProcAddress(mods[i], "ReShadeLogMessage"));
                g_reshade_module = self;
                // Which ReShade, and which version it settled on. The loop
                // counts down past the point where effect_runtime's vtable is
                // the one this add-on was compiled against, so the number it
                // stopped at is a fact synth.inc has to check rather than a
                // detail of the negotiation.
                g_reshade_dll = mods[i];
                g_reshade_api = version;
                // Which ReShade, recorded rather than logged: this runs before the
                // log is truncated at attach, so a line written here does not
                // survive. The banner prints it.
                //
                // It matters more than it looks. RegisterWithReShade takes the
                // FIRST module that accepts, and a folder can hold two: a proxy
                // DLL beside the executable and the machine-wide Vulkan layer.
                // Baldur's Gate 3 with dxgi.dll in bin\ is exactly that, and the
                // one that wins decides whether the effect runtime this add-on
                // sees is the game's Vulkan one or a secondary D3D12 device.
                GetModuleFileNameW(mods[i], g_reshade_path, MAX_PATH);
                for (DWORD k = 0; k < count; ++k)
                    if (k != i && GetProcAddress(mods[k], "ReShadeRegisterAddon") != nullptr)
                        ++g_reshade_others;

                // Pin it. On D3D11 and D3D12 ReShade arrives as a proxy DLL beside
                // the executable and stays mapped for the life of the process; on
                // Vulkan it is an implicit layer, and the Vulkan loader FreeLibrary's
                // it at vkDestroyInstance. An add-on that calls a ReShade export from
                // its own detach -- renodx-dlss5 does, from +0x2EDAA -- then jumps
                // into memory that is no longer mapped, which is the 0xC0000005 at
                // Vulkan teardown that four bisects attributed here.
                //
                // Pinning holds the module to process exit. It costs one reference
                // and changes nothing on the D3D paths, where it was already true.
                HMODULE pinned = nullptr;
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN |
                                   GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                   reinterpret_cast<LPCWSTR>(reg), &pinned);
                return true;
            }
        }
    }
    return false;
}

// A standalone NGX D3D12 probe, off unless dlss5-bridge.cfg says probe=1.
//
// The bridge only opens its D3D12 session when the game calls DLSS, which makes
// some questions untestable: Prey 2017 has no DLSS of its own, so removing the
// Luma add-on to see whether Luma is what NVSDK_NGX_D3D12_Init_Ext faults inside
// also removes every DLSS call, and nothing runs at all. This does the same two
// calls on its own, on its own device, so the answer no longer depends on the
// game providing a DLSS feature.
//
// It runs once, on its own thread, after a delay long enough for the other
// add-ons to have registered whatever they register.
static DWORD NgxProbeRun()
{
    Sleep(20000);

    Log("");
    Log("################ NGX probe (probe=1) ################");
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    if (d3d12 == nullptr) { Log("[probe] d3d12.dll would not load"); return 0; }

    auto create_device = reinterpret_cast<PFN_D3D12CreateDevice_>(
        GetProcAddress(d3d12, "D3D12CreateDevice"));
    if (create_device == nullptr) { Log("[probe] no D3D12CreateDevice export"); return 0; }

    ID3D12Device *dev = nullptr;
    DWORD code = 0;
    HRESULT hr = SafeCreateDevice(create_device, nullptr, &dev, &code);
    if (code != 0 || FAILED(hr) || dev == nullptr)
    {
        wchar_t owner[MAX_PATH] = {};
        if (code != 0 && NameFaultOwner(owner, MAX_PATH))
            Log("[probe] D3D12CreateDevice raised 0x%08X at %p, inside %ls",
                code, g_last_fault_at, owner);
        else
            Log("[probe] D3D12CreateDevice failed 0x%08X (exception 0x%08X)", hr, code);
        Log("############# NGX probe done #############");
        return 0;
    }
    Log("[probe] D3D12 device created");

    HMODULE ngx = FindNgxLoader();
    if (ngx == nullptr) { Log("[probe] no NGX loader is present"); dev->Release(); return 0; }

    auto init_ext = reinterpret_cast<PFN_Init_Ext>(
        GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_Ext"));
    if (init_ext == nullptr) { Log("[probe] no NVSDK_NGX_D3D12_Init_Ext export"); dev->Release(); return 0; }

    wchar_t data_path[MAX_PATH] = {};
    GetModuleFileNameW(g_self, data_path, MAX_PATH);
    if (wchar_t *sl = wcsrchr(data_path, (wchar_t)92)) *(sl + 1) = 0;

    code = 0;
    NVSDK_NGX_Result r = SafeInitExt(init_ext, 0x1000000ULL, data_path, dev, 0x13, &code);
    if (code != 0)
    {
        wchar_t owner[MAX_PATH] = {};
        if (NameFaultOwner(owner, MAX_PATH))
            Log("[probe] Init_Ext raised 0x%08X at %p, inside %ls", code, g_last_fault_at, owner);
        else
            Log("[probe] Init_Ext raised 0x%08X", code);
        Log("[probe] the fault does not need the game to call DLSS, so it belongs to "
            "this process rather than to any DLSS contract.");
    }
    else
    {
        Log("[probe] Init_Ext -> %s (0x%08X, %s)",
            r == NGX_SUCCESS ? "ok" : "refused", r, NgxResultName(r));
    }

    dev->Release();
    Log("############# NGX probe done #############");
    return 0;
}

// Reads one key straight from the config file. Two callers now: the probe
// decides whether to run before the bridge has parsed anything, and the Vulkan
// mirror's gate is deliberately not a BridgeCfg field -- see g_vk_mirror.
static bool CfgKeyOn(const char *key_eq)
{
    // Same two names CfgPath tries, and for the same reason: this runs before
    // the hooks go in, so a user upgrading from dlss5-dx11-bridge must not lose
    // the setting that holds them back.
    char path[MAX_PATH] = {};
    GetModuleFileNameA(g_self, path, MAX_PATH);
    char *sl = strrchr(path, (char)92);   // backslash
    if (sl == nullptr) return false;
    const size_t room = MAX_PATH - (sl + 1 - path);
    strcpy_s(sl + 1, room, "dlss5-bridge.cfg");
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
        strcpy_s(sl + 1, room, "dlss5-dx11-bridge.cfg");

    FILE *f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || f == nullptr) return false;
    char line[256];
    bool on = false;
    const size_t n = strlen(key_eq);
    while (fgets(line, sizeof(line), f) != nullptr)
        if (_strnicmp(line, key_eq, n) == 0) { on = true; break; }
    fclose(f);
    return on;
}

// The same file, read the same way, but for a key whose ABSENCE has to mean
// something other than off. CfgKeyOn answers "is this exact line present", which
// makes a missing key and an explicit 0 indistinguishable -- fine for probe=1,
// wrong for anything that should default on.
//
// Prefix-matched at the start of a line like its sibling, so a comment line can
// mention a key without turning it on.
static int CfgKeyInt(const char *key, int def)
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(g_self, path, MAX_PATH);
    char *sl = strrchr(path, (char)92);   // backslash
    if (sl == nullptr) return def;
    const size_t room = MAX_PATH - (sl + 1 - path);
    strcpy_s(sl + 1, room, "dlss5-bridge.cfg");
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
        strcpy_s(sl + 1, room, "dlss5-dx11-bridge.cfg");

    FILE *f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || f == nullptr) return def;
    char line[256];
    int  out = def;
    int  hits = 0;
    const size_t n = strlen(key);
    // LAST match wins, and the loop does not break. CfgReload keeps overwriting
    // and so takes the last line for every key it reads; this one broke on the
    // first, so a file carrying the key twice -- appending a line from a forum
    // post to a file that already has it -- was read two different ways by the two
    // readers of the same file. Measured with vk_mirror=0 first and vk_mirror=1
    // last: the Vulkan half was off and nothing said so.
    while (fgets(line, sizeof(line), f) != nullptr)
    {
        if (_strnicmp(line, key, n) != 0 || line[n] != '=') continue;
        int v = 0;
        if (sscanf_s(line + n + 1, "%d", &v) == 1) { out = v; ++hits; }
    }
    fclose(f);
    if (hits > 1)
        Log("[bridge] %s appears %d times in the configuration file. The last one is "
            "used, as it is for every other key.", key, hits);
    return out;
}

static DWORD WINAPI NgxProbeThread(LPVOID module)
{
    const DWORD result = NgxProbeRun();
    FreeLibraryAndExitThread(static_cast<HMODULE>(module), result);
}

static bool ProbeRequested() { return CfgKeyOn("probe=1"); }

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = module;
        g_shutting_down = false;
        g_worker_running = 0;
        DisableThreadLibraryCalls(module);
        InitializeCriticalSection(&g_log_cs);
        InitializeCriticalSection(&g_hook_cs);
        // BOTH backends serialise on this one now. Eight NGX forwards in this file
        // and five in vkmirror.inc, all in the same order: g_ngx_cs OUTSIDE
        // g_hook_cs, never the reverse.
        //
        // It was left off the D3D11 side for four releases, while five comments
        // and a release note said both backends were covered. The stated reason
        // for not closing it was that nine shipped titles run through these
        // detours and there was no way to test D3D11 without launching one of
        // them. That reason expired: ngxGym drives real NGX on D3D11 from a
        // scriptable host in seconds, so the change could be looped rather than
        // reasoned about.
        //
        // Pathologic 3 (issue #15) crashed 0xC0000005 inside the game's own
        // NVUnityPlugin.DLL with this add-on last recorded "running the D3D12
        // evaluate", after 6509 delivered frames. That is the signature this
        // closes, not a demonstration that it was the cause -- nobody has
        // reproduced that crash under a debugger, and a clean run here is not
        // proof for a race that is won by timing.
        //
        // The ready flag exists because VKM_FORWARD can be reached from a detour
        // before this line on a process that loads an NGX module unusually early,
        // and entering an uninitialised section is worse than not serialising.
        InitializeCriticalSection(&g_ngx_cs);
        g_ngx_cs_ready = true;
        // Before RegisterWithReShade, because that is what arms the ReShade
        // event a second source enters the frame path through, and an
        // uninitialised section is not a lock that is merely unheld.
        InitializeCriticalSection(&g_bridge_cs);

        GetModuleFileNameA(module, g_log_path, MAX_PATH);
        char *slash = strrchr(g_log_path, '\\');
        if (slash != nullptr)
            strcpy_s(slash + 1, MAX_PATH - (slash + 1 - g_log_path), "dlss5-bridge.log");

        if (!RegisterWithReShade(module))
            return FALSE;  // ReShade will unload us; do not leave hooks behind

        // Opt-in and off by default: it creates a D3D12 device in every game
        // that turns it on. The thread does not start until DllMain returns.
        if (ProbeRequested())
            if (HANDLE t = StartModuleWorker(NgxProbeThread))
                CloseHandle(t);

        // Carry forward the previous run's crash report, if it left one. This
        // looks for a marker the crash handler writes, not for the absence of a
        // clean-exit marker: many games terminate the process without ever
        // running DLL detach, so absence proves nothing and reporting it would
        // claim a crash on every ordinary launch.
        char carried[2048] = {};
        bool carried_truncated = false;
        {
            FILE *old = nullptr;
            if (fopen_s(&old, g_log_path, "r") == 0 && old != nullptr)
            {
                char line[512];
                bool in_report = false;
                while (fgets(line, sizeof(line), old) != nullptr)
                {
                    if (strstr(line, "### CRASH RECORDED ###") != nullptr)
                    {
                        in_report = true;
                        carried[0] = '\0';
                        carried_truncated = false;
                        continue;
                    }
                    if (in_report && strstr(line, "####") != nullptr) break;
                    if (in_report && !AppendCrashReport(carried, line))
                        carried_truncated = true;
                }
                fclose(old);
            }

            FILE *f = nullptr;
            if (fopen_s(&f, g_log_path, "w") == 0 && f != nullptr) fclose(f);
        }

        g_prev_filter = SetUnhandledExceptionFilter(&CrashFilter);

        // First line of every log, so a report can name the build exactly,
        // followed by everything needed to diagnose a setup remotely.
        Log("dlss5-bridge %s (built %s %s) attached.", BRIDGE_VERSION, __DATE__, __TIME__);
        if (carried[0] != '\0')
        {
            Log("The previous run crashed. What it recorded at the time:");
            Log("%s", carried);
            if (carried_truncated) Log("[bridge] Previous crash report truncated safely at 2047 bytes.");
        }

        LogEnvironment();
        LogNeighbours();

        // Written now rather than when the D3D12 session opens, so the file
        // exists even in a game where nothing ever hooks -- and so stage=0 is
        // available as an off switch before launching, without deleting this.
        CfgRegenerateIfStale();
        CfgWriteDefault();
        // And read it. stage=0 is tested before the first evaluate, and until
        // now the only load happened inside the session opener it was meant to
        // prevent -- so the documented off switch still created a D3D12 device
        // and started an NGX session before anything looked at the file.
        CfgReload();
        // Which file those values came from, before anything acts on them. A
        // session that never opens a D3D12 session still gets this line.
        CfgSayWhich();
        // Before anything can reach NGX, and after the file inventory that
        // recorded which snippets are beside the game and the config that can
        // switch this off. Whoever loads a file called _nvngx.dll first owns
        // that name for the process, so this is the only moment the choice
        // exists. See the note above the function for what it is choosing
        // between and why it usually chooses nothing.
        PreferWorkingNgxLoader();
        // After the config, never before it: stage=0 has to switch this off too,
        // and the value is not known until the file has been read.
        SynthRegisterEvents();
        // Read before StartWatchingForNgx, because it decides whether the module
        // scan hooks the Vulkan entry points at all.
        // ON unless the file says otherwise, which is the opposite of how this
        // shipped. Hooking the Vulkan NGX entry points is a no-op in a process
        // that has none -- every D3D11 game -- so the only thing the old default
        // bought was a Vulkan user getting nothing and no way to find out why:
        // the key was not even written into the generated config, so there was
        // nothing to discover. It costs a Vulkan game four more patched entry
        // points per module, which is what it is for.
        g_vk_mirror = CfgKeyInt("vk_mirror", 1) != 0 ? 1 : 0;
        // Said when it is OFF, which is the case that produced no line anywhere.
        // vk_mirror is not a BridgeCfg field, so it is absent from the config echo
        // too, and VkmRegisterEvents returns before its own announcement -- so a
        // file carrying vk_mirror=0, from a version where that was the default or
        // from a support thread that suggested it, took away the whole Vulkan half
        // in silence.
        if (g_vk_mirror == 0)
            Log("[bridge] vk_mirror=0 in the configuration file, so the Vulkan NGX entry "
                "points are not hooked and a Vulkan game's own DLSS cannot be mirrored. "
                "The substitute contract still runs on Vulkan, park and all -- it is "
                "registered above this line and does not read this key. A DirectX game "
                "has no entry points to hook either way. Remove the line or set "
                "vk_mirror=1 to turn the mirror back on.");
        VkmRegisterEvents();
        // Not behind SynthEnabled, unlike the events above. The panel's whole
        // job is to say which situation this session is in, and "synthesis is
        // switched off and the mirror is running" is one of the situations it
        // has to be able to say. It registers a draw callback and nothing else:
        // no device, no session, no per-frame work unless the user opens the
        // overlay.
        PanelRegister();

        g_hook_time = GetTickCount64();
        StartWatchingForNgx();
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        // Stop being told about new libraries before anything else, so no
        // callback can arrive while the rest of this is tearing down.
        // Before anything else that can fault. This filter is process-wide and
        // points into this module; leaving it installed after an unload sends
        // the next unhandled exception anywhere in the process -- game, driver,
        // another add-on -- into memory that is gone.
        if (g_prev_filter != nullptr)
        {
            SetUnhandledExceptionFilter(g_prev_filter);
            g_prev_filter = nullptr;
        }

        // A non-null third argument means the PROCESS is terminating, not that
        // somebody called FreeLibrary. The loader has already begun unloading other
        // modules, so calling into NGX, D3D12, Vulkan or the driver from here reaches
        // code that is gone -- which is exactly what a Vulkan session recorded:
        // 0xC0000005 "in: memory that is no longer mapped -- the code that faulted
        // has been unloaded", with this add-on last doing "running the D3D12
        // evaluate". Nothing below is needed on process exit; the operating system
        // reclaims every handle, every allocation and every hook byte in this
        // process image when the process goes.
        //
        // The FreeLibrary case still does all of it, and must: the note below about
        // a module unloading with its jumps still in place is about exactly that
        // case, and it is not hypothetical.
        if (reserved != nullptr) return TRUE;

        StopWatchingForNgx();
        ReportOutcome();

        // NGX hooks pin the bridge, but a probe or synthetic-only session can
        // still detach normally. Release any parked transport before unregistering.
        VkmShutdown();
        // The substitute contract's Vulkan transport parks the same way and for
        // the same stakes, on ReShade's own command buffer rather than the game's.
        SynthVkParkStop();

        // Any installed hook pins this module before publication. Normal detach
        // therefore has no MinHook state to tear down; never acquire its shared
        // mutex or suspend threads under the loader lock.
        if (g_unregister != nullptr) g_unregister(g_self);

        // Kept as a human-readable end-of-session marker only. Nothing reads it:
        // inferring a crash from its absence is what 1.0.9 removed, because most
        // games never run detach at all.
        Log("shut down cleanly.");
    }
    return TRUE;
}
