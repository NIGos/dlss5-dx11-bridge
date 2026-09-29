#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vulkan/vulkan.h>
#include <MinHook.h>
#include <intrin.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

using GetModuleEx = BOOL (WINAPI *)(DWORD, LPCWSTR, HMODULE *);
using GetProc = FARPROC (WINAPI *)(HMODULE, LPCSTR);
using RegisterAddon = bool (WINAPI *)(HMODULE, uint32_t);
using UnregisterAddon = void (WINAPI *)(HMODULE);
static GetModuleEx original_get_module;
static GetProc original_get_proc;
static RegisterAddon original_register;
static UnregisterAddon original_unregister;
static PVOID volatile generic_module;
static volatile LONG registered, registrations, acquisitions, gate_claimed;
static HANDLE acquired, release_scan, probed, unregistered;
static bool gating, late_reference;
static HMODULE injected_reference;

static HMODULE Generic()
{
    return static_cast<HMODULE>(InterlockedCompareExchangePointer(&generic_module, nullptr, nullptr));
}

static bool FromBridge(void *address)
{
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(address, &info, sizeof(info)) == sizeof(info) &&
        info.AllocationBase == GetModuleHandleW(L"dlss5-bridge.addon64");
}

static void PrintStack()
{
    void *frames[20]{};
    const USHORT count = CaptureStackBackTrace(0, 20, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        MEMORY_BASIC_INFORMATION info{};
        wchar_t path[MAX_PATH]{};
        if (VirtualQuery(frames[i], &info, sizeof(info)) != sizeof(info)) continue;
        GetModuleFileNameW(static_cast<HMODULE>(info.AllocationBase), path, MAX_PATH);
        const wchar_t *leaf = wcsrchr(path, L'\\');
        std::printf("  stack[%u] %ls+0x%llx\n", static_cast<unsigned>(i), leaf ? leaf + 1 : path,
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) -
                                           reinterpret_cast<uintptr_t>(info.AllocationBase)));
    }
}

static bool WINAPI Register(HMODULE module, uint32_t api)
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t *leaf = wcsrchr(path, L'\\');
    const bool is_generic = _wcsicmp(leaf ? leaf + 1 : path, L"renodx-dlss5.addon64") == 0;
    const bool result = original_register(module, api);
    if (is_generic && result) {
        InterlockedExchangePointer(&generic_module, module);
        InterlockedExchange(&registered, 1);
        std::printf("GENERIC registered module=%p thread=%lu count=%ld\n", module,
            GetCurrentThreadId(), InterlockedIncrement(&registrations));
    }
    return result;
}

static void WINAPI Unregister(HMODULE module)
{
    const bool is_generic = module == Generic();
    if (is_generic) {
        std::printf("GENERIC unregister module=%p thread=%lu\n", module, GetCurrentThreadId());
        PrintStack();
    }
    original_unregister(module);
    if (is_generic) {
        InterlockedExchange(&registered, 0);
        InterlockedExchangePointer(&generic_module, nullptr);
        SetEvent(unregistered);
    }
}

static BOOL WINAPI GetModule(DWORD flags, LPCWSTR name, HMODULE *module)
{
    void *caller = _ReturnAddress();
    const BOOL result = original_get_module(flags, name, module);
    if (result && module && *module == Generic() &&
        (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) != 0 && FromBridge(caller)) {
        if (flags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT) {
            SetEvent(probed);
            if (late_reference && registrations == 2 && !injected_reference &&
                original_get_module(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, name, &injected_reference)) {
                InterlockedIncrement(&acquisitions);
                std::puts("NEGATIVE extra reference acquired after probe notification");
            }
            return result;
        }
        if (flags & GET_MODULE_HANDLE_EX_FLAG_PIN) return result;
        InterlockedIncrement(&acquisitions);
        if (gating && InterlockedCompareExchange(&gate_claimed, 1, 0) == 0) {
            std::printf("SCAN holds Generic reference thread=%lu caller=%p\n", GetCurrentThreadId(), caller);
            PrintStack();
            SetEvent(acquired);
            // Delay only the test process's scanner after its real reference acquisition.
            if (WaitForSingleObject(release_scan, 15000) != WAIT_OBJECT_0)
                std::puts("FAIL scanner gate timed out");
        }
    }
    return result;
}

static FARPROC WINAPI Proc(HMODULE module, LPCSTR name)
{
    void *caller = _ReturnAddress();
    const FARPROC result = original_get_proc(module, name);
    if (module == Generic() && reinterpret_cast<uintptr_t>(name) > 0xffff &&
        std::strcmp(name, "NVSDK_NGX_D3D11_EvaluateFeature") == 0 && FromBridge(caller))
        SetEvent(probed);
    return result;
}

static bool Hook(void *target, void *detour, void **original)
{
    const MH_STATUS result = target ? MH_CreateHook(target, detour, original) : MH_ERROR_NOT_EXECUTABLE;
    if (result != MH_OK) {
        std::printf("FAIL hook target=%p status=%d\n", target, static_cast<int>(result));
        return false;
    }
    return true;
}

static bool Instance(VkInstance &instance)
{
    const char *layer = "VK_LAYER_reshade";
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Independent addon lifetime regression";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create.pApplicationInfo = &app;
    create.enabledLayerCount = 1;
    create.ppEnabledLayerNames = &layer;
    const VkResult result = vkCreateInstance(&create, nullptr, &instance);
    std::printf("INSTANCE created=%p result=%d registered=%ld\n", instance,
        static_cast<int>(result), InterlockedCompareExchange(&registered, 0, 0));
    return result == VK_SUCCESS;
}

static bool FinishProbe()
{
    const HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    const auto lock = reinterpret_cast<LONG (NTAPI *)(ULONG, ULONG *, ULONG_PTR *)>(
        GetProcAddress(nt, "LdrLockLoaderLock"));
    const auto unlock = reinterpret_cast<LONG (NTAPI *)(ULONG, ULONG_PTR)>(
        GetProcAddress(nt, "LdrUnlockLoaderLock"));
    ULONG disposition = 0;
    ULONG_PTR cookie = 0;
    if (!lock || !unlock || lock(0, &disposition, &cookie) < 0 || disposition != 1) return false;
    return unlock(0, cookie) >= 0;
}

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc != 2 || (std::strcmp(argv[1], "race") && std::strcmp(argv[1], "stable") &&
                      std::strcmp(argv[1], "control") && std::strcmp(argv[1], "late-ref"))) return 2;
    const bool expect_race = std::strcmp(argv[1], "race") == 0;
    const bool control = std::strcmp(argv[1], "control") == 0;
    late_reference = std::strcmp(argv[1], "late-ref") == 0;
    gating = !control;
    acquired = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    release_scan = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    probed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    unregistered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!acquired || !release_scan || !probed || !unregistered) return 3;
    const HMODULE reshade = LoadLibraryW(L"C:\\ProgramData\\ReShade\\ReShade64.dll");
    if (!reshade) { std::printf("FAIL ReShade load error=%lu\n", GetLastError()); return 4; }
    if (MH_Initialize() != MH_OK) return 5;
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (!Hook(reinterpret_cast<void *>(GetProcAddress(reshade, "ReShadeRegisterAddon")),
              reinterpret_cast<void *>(&Register), reinterpret_cast<void **>(&original_register)) ||
        !Hook(reinterpret_cast<void *>(GetProcAddress(reshade, "ReShadeUnregisterAddon")),
              reinterpret_cast<void *>(&Unregister), reinterpret_cast<void **>(&original_unregister)) ||
        !Hook(reinterpret_cast<void *>(GetProcAddress(kernel, "GetModuleHandleExW")),
              reinterpret_cast<void *>(&GetModule), reinterpret_cast<void **>(&original_get_module)) ||
        !Hook(reinterpret_cast<void *>(GetProcAddress(kernel, "GetProcAddress")),
              reinterpret_cast<void *>(&Proc), reinterpret_cast<void **>(&original_get_proc)) ||
        MH_EnableHook(MH_ALL_HOOKS) != MH_OK) return 6;

    VkInstance first{};
    if (!Instance(first) || !Generic() || !registered) return 7;
    DWORD observed = WAIT_OBJECT_0 + 1;
    if (!control) {
        HANDLE events[]{acquired, probed};
        observed = WaitForMultipleObjects(2, events, FALSE, 8000);
        if (observed != WAIT_OBJECT_0 && observed != WAIT_OBJECT_0 + 1) {
            SetEvent(release_scan);
            std::puts("FAIL bridge scanner did not visit Generic");
            return 8;
        }
        // The no-ref observation fires inside the loader lock, before the candidate decision.
        if (observed == WAIT_OBJECT_0 + 1 && !FinishProbe()) return 13;
    }
    std::printf("BEFORE destroy registered=%ld acquisitions=%ld gated=%d\n", registered,
        acquisitions, observed == WAIT_OBJECT_0);
    vkDestroyInstance(first, nullptr);
    const HMODULE after_destroy = GetModuleHandleW(L"renodx-dlss5.addon64");
    std::printf("AFTER destroy registered=%ld module=%p\n", registered, after_destroy);
    if (!expect_race && (registered != 0 || after_destroy != nullptr)) {
        SetEvent(release_scan);
        std::puts("FAIL Generic outlived the first instance");
        return 14;
    }
    ResetEvent(unregistered);
    ResetEvent(probed);
    VkInstance final{};
    if (!Instance(final)) { SetEvent(release_scan); return 9; }
    const LONG before_release = InterlockedCompareExchange(&registered, 0, 0);
    SetEvent(release_scan);
    if (observed == WAIT_OBJECT_0) {
        const DWORD dropped = WaitForSingleObject(unregistered, 8000);
        const bool reproduced = before_release == 1 && dropped == WAIT_OBJECT_0 && registered == 0 &&
            GetModuleHandleW(L"renodx-dlss5.addon64") == nullptr;
        std::printf("VERDICT race=%d registrations=%ld acquisitions=%ld registered=%ld\n",
            reproduced, registrations, acquisitions, registered);
        vkDestroyInstance(final, nullptr);
        return expect_race && reproduced ? 0 : 10;
    }
    if (!control && (WaitForSingleObject(probed, 8000) != WAIT_OBJECT_0 || !FinishProbe())) return 11;
    const bool stable = registered == 1 && registrations >= 2 && acquisitions == 0 &&
        GetModuleHandleW(L"renodx-dlss5.addon64") != nullptr;
    std::printf("VERDICT stable=%d registrations=%ld acquisitions=%ld registered=%ld control=%d\n",
        stable, registrations, acquisitions, registered, control);
    if (injected_reference) FreeLibrary(injected_reference);
    vkDestroyInstance(final, nullptr);
    const bool cleaned = registered == 0 && GetModuleHandleW(L"renodx-dlss5.addon64") == nullptr;
    std::printf("FINAL cleanup=%d acquisitions=%ld\n", cleaned, acquisitions);
    return !expect_race && stable && cleaned && acquisitions == 0 ? 0 : 12;
}
