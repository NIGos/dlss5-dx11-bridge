#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../../../src/ngx-module-scan.h"

namespace {
constexpr DWORD kWaitMs = 5000;
constexpr DWORD kSuiteTimeoutMs = 60000;
constexpr double kRetryLimitMs = 250.0;
constexpr unsigned kCycles = 20;
constexpr int kFixtureValue = 0x514758;

using DetachObserver = void (WINAPI *)(void *, BOOL);
using ObserveFn = void (WINAPI *)(DetachObserver, void *);
using ValueFn = int (WINAPI *)();
using LockFn = LONG (NTAPI *)(ULONG, ULONG *, ULONG_PTR *);
using UnlockFn = LONG (NTAPI *)(ULONG, ULONG_PTR);

CRITICAL_SECTION g_scanCs;
volatile LONG g_inScan = 0;
unsigned g_checks = 0;
const char *g_case = "setup";
std::wstring g_directory;

[[noreturn]] void Fail(const char *message)
{
    std::fprintf(stderr, "FAIL [%s] %s (checks=%u, last-error=%lu)\n",
                 g_case, message, g_checks, GetLastError());
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 1);
    std::abort();
}

void Check(bool condition, const char *message)
{
    ++g_checks;
    if (!condition)
        Fail(message);
}

template <typename T> T Proc(HMODULE module, const char *name)
{
    Check(InterlockedCompareExchange(&g_inScan, 0, 0) == 0,
          "test GetProcAddress must be outside the scan critical section");
    FARPROC address = GetProcAddress(module, name);
    T function = nullptr;
    static_assert(sizeof(function) == sizeof(address));
    std::memcpy(&function, &address, sizeof(function));
    Check(function != nullptr, name);
    return function;
}

struct Witness {
    volatile LONG detaches = 0;
    volatile LONG insideScan = 0;
    volatile LONG processExit = 0;
};

void WINAPI OnDetach(void *context, BOOL processExit)
{
    auto *witness = static_cast<Witness *>(context);
    InterlockedIncrement(&witness->detaches);
    if (InterlockedCompareExchange(&g_inScan, 0, 0) != 0)
        InterlockedIncrement(&witness->insideScan);
    if (processExit)
        InterlockedIncrement(&witness->processExit);
}

void Observe(HMODULE module, Witness &witness)
{
    Proc<ObserveFn>(module, "ScanFixtureObserve")(&OnDetach, &witness);
}

HMODULE Load(const wchar_t *file, Witness &witness)
{
    Check(GetModuleHandleW(file) == nullptr, "fixture was already loaded");
    const std::wstring path = g_directory + file;
    HMODULE module = LoadLibraryW(path.c_str());
    Check(module != nullptr, "LoadLibraryW fixture");
    Observe(module, witness);
    return module;
}

void Release(HMODULE module)
{
    Check(InterlockedCompareExchange(&g_inScan, 0, 0) == 0,
          "FreeLibrary must be outside the scan critical section");
    Check(module != nullptr, "release needs an owned reference");
    Check(FreeLibrary(module) != FALSE, "FreeLibrary");
}

void Alive(const Witness &witness)
{
    Check(witness.detaches == 0, "fixture detached while an owned reference was live");
    Check(witness.insideScan == 0, "DLL_PROCESS_DETACH ran inside scan critical section");
}

void Detached(const wchar_t *file, const Witness &witness)
{
    Check(witness.detaches == 1, "final owned FreeLibrary must detach exactly once");
    Check(witness.insideScan == 0, "DLL_PROCESS_DETACH ran inside scan critical section");
    Check(witness.processExit == 0, "detach must be FreeLibrary, not process shutdown");
    Check(GetModuleHandleW(file) == nullptr, "fixture reference leaked after final release");
}

const char *ResultName(NgxScanResult result)
{
    switch (result) {
    case NgxScanResult::skipped: return "skipped";
    case NgxScanResult::acquired: return "acquired";
    case NgxScanResult::retry: return "retry";
    case NgxScanResult::unavailable: return "unavailable";
    }
    return "invalid result";
}

bool ScanNoFault(const NgxModuleScanner &scanner, HMODULE module, bool vulkan,
                 HMODULE &held, NgxScanResult &result, DWORD &exception)
{
    __try {
        result = scanner.Acquire(module, vulkan, held);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        exception = GetExceptionCode();
        return false;
    }
}

HMODULE Scan(const NgxModuleScanner &scanner, HMODULE module, bool vulkan,
             NgxScanResult expected, double *elapsedMs = nullptr)
{
    HMODULE held = reinterpret_cast<HMODULE>(static_cast<ULONG_PTR>(1));
    NgxScanResult result = NgxScanResult::unavailable;
    DWORD exception = 0;
    LARGE_INTEGER frequency = {}, begin = {}, end = {};
    Check(QueryPerformanceFrequency(&frequency) != FALSE, "QueryPerformanceFrequency");
    EnterCriticalSection(&g_scanCs);
    InterlockedExchange(&g_inScan, 1);
    const BOOL began = QueryPerformanceCounter(&begin);
    const bool safe = ScanNoFault(scanner, module, vulkan, held, result, exception);
    const BOOL ended = QueryPerformanceCounter(&end);
    LeaveCriticalSection(&g_scanCs);
    InterlockedExchange(&g_inScan, 0);
    Check(began != FALSE && ended != FALSE, "QueryPerformanceCounter");
    if (!safe) {
        std::fprintf(stderr, "Acquire raised SEH 0x%08lX for module=%p\n",
                     exception, static_cast<void *>(module));
        Fail("Acquire must safely reject invalid/stale HMODULE");
    }
    if (elapsedMs != nullptr)
        *elapsedMs = 1000.0 * static_cast<double>(end.QuadPart - begin.QuadPart) /
                     static_cast<double>(frequency.QuadPart);
    if (result != expected) {
        std::fprintf(stderr, "Acquire(module=%p, vulkan=%d): expected=%s actual=%s held=%p\n",
                     static_cast<void *>(module), vulkan ? 1 : 0, ResultName(expected),
                     ResultName(result), static_cast<void *>(held));
        Fail("unexpected scan result");
    }
    Check(expected == NgxScanResult::acquired ? held == module : held == nullptr,
          "acquired must return the module; every other result must clear held");
    return held;
}

bool CallNoFault(ValueFn function, int &value)
{
    __try {
        value = function();
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Call(ValueFn function)
{
    int value = 0;
    Check(CallNoFault(function, value), "export pointer became invalid while source was owned");
    Check(value == kFixtureValue, "fixture export returned wrong value");
}

void Stale(const NgxModuleScanner &scanner, HMODULE module)
{
    Scan(scanner, module, false, NgxScanResult::skipped);
    Scan(scanner, module, true, NgxScanResult::skipped);
}

struct ScanCase {
    const char *label;
    const wchar_t *file;
    bool vulkan;
    bool candidate;
};

void TestCase(const NgxModuleScanner &scanner, const ScanCase &test)
{
    g_case = test.label;
    for (unsigned cycle = 0; cycle != kCycles; ++cycle) {
        Witness witness;
        HMODULE module = Load(test.file, witness);
        HMODULE held = Scan(scanner, module, test.vulkan,
                            test.candidate ? NgxScanResult::acquired : NgxScanResult::skipped);
        Alive(witness);
        Release(module);
        if (test.candidate) {
            Alive(witness);
            Check(GetModuleHandleW(test.file) == held, "owned ref did not retain source module");
            Call(Proc<ValueFn>(held, "ScanFixtureValue"));
            Release(held);
        }
        Detached(test.file, witness);
        Stale(scanner, module);
    }
    std::printf("PASS %s: %u unload/reload cycles, including stale handles\n", g_case, kCycles);
}

HANDLE Event(const wchar_t *name = nullptr)
{
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, name);
    Check(event != nullptr, "CreateEventW");
    Check(name == nullptr || GetLastError() != ERROR_ALREADY_EXISTS,
          "named fixture event unexpectedly already existed");
    return event;
}

void Signal(HANDLE event)
{
    Check(SetEvent(event) != FALSE, "SetEvent");
}

void Wait(HANDLE object)
{
    Check(WaitForSingleObject(object, kWaitMs) == WAIT_OBJECT_0, "bounded wait failed/timed out");
}

bool Signaled(HANDLE event)
{
    const DWORD status = WaitForSingleObject(event, 0);
    Check(status == WAIT_OBJECT_0 || status == WAIT_TIMEOUT, "event state query failed");
    return status == WAIT_OBJECT_0;
}

void Close(HANDLE object)
{
    Check(CloseHandle(object) != FALSE, "CloseHandle");
}

struct LoaderHold {
    LockFn lock;
    UnlockFn unlock;
    HANDLE ready;
    HANDLE start;
    HANDLE acquired;
    HANDLE release;
    LONG lockStatus = -1;
    LONG unlockStatus = -1;
    ULONG disposition = 0;
    DWORD waitStatus = WAIT_FAILED;
    volatile LONG active = 0;
};

DWORD WINAPI HoldLoaderLock(void *context)
{
    auto &hold = *static_cast<LoaderHold *>(context);
    if (!SetEvent(hold.ready) || WaitForSingleObject(hold.start, kWaitMs) != WAIT_OBJECT_0)
        return 1;
    ULONG_PTR cookie = 0;
    hold.lockStatus = hold.lock(2, &hold.disposition, &cookie);
    if (hold.lockStatus >= 0 && hold.disposition == 1) {
        InterlockedExchange(&hold.active, 1);
        if (SetEvent(hold.acquired))
            hold.waitStatus = WaitForSingleObject(hold.release, kWaitMs);
        hold.unlockStatus = hold.unlock(0, cookie);
        InterlockedExchange(&hold.active, 0);
    } else {
        SetEvent(hold.acquired);
    }
    return 0;
}

void TestContention(const NgxModuleScanner &scanner, LockFn lock, UnlockFn unlock)
{
    g_case = "real loader-lock contention";
    double maxMs = 0.0;
    for (unsigned cycle = 0; cycle != kCycles; ++cycle) {
        Witness witness;
        HMODULE module = Load(L"scan-d11.dll", witness);
        LoaderHold hold = {lock, unlock, Event(), Event(), Event(), Event()};
        HANDLE thread = CreateThread(nullptr, 0, &HoldLoaderLock, &hold, 0, nullptr);
        Check(thread != nullptr, "CreateThread loader-lock holder");
        Wait(hold.ready);
        Signal(hold.start);
        Wait(hold.acquired);
        Check(hold.lockStatus >= 0 && hold.disposition == 1,
              "worker must actually own the native loader lock");
        double elapsedMs = 0.0;
        Scan(scanner, module, false, NgxScanResult::retry, &elapsedMs);
        Check(InterlockedCompareExchange(&hold.active, 0, 0) == 1,
              "Acquire returned only after the competing loader lock was released");
        Check(elapsedMs < kRetryLimitMs, "TRY_ONLY Acquire did not retry immediately (<250 ms)");
        if (elapsedMs > maxMs)
            maxMs = elapsedMs;
        Signal(hold.release);
        Wait(thread);
        Check(hold.waitStatus == WAIT_OBJECT_0 && hold.unlockStatus >= 0,
              "loader lock must release on the explicit event, not timeout");
        Close(thread);
        Close(hold.ready);
        Close(hold.start);
        Close(hold.acquired);
        Close(hold.release);
        HMODULE held = Scan(scanner, module, false, NgxScanResult::acquired);
        Release(module);
        Alive(witness);
        Call(Proc<ValueFn>(held, "NVSDK_NGX_D3D11_EvaluateFeature"));
        Release(held);
        Detached(L"scan-d11.dll", witness);
        Stale(scanner, module);
    }
    std::printf("PASS real loader-lock contention: %u cycles, maximum retry %.3f ms\n",
                kCycles, maxMs);
}

HANDLE TargetEvent(const wchar_t *kind)
{
    wchar_t name[96] = {};
    Check(swprintf_s(name, L"Local\\NgxScanTarget%s_%lu", kind, GetCurrentProcessId()) > 0,
          "target event name");
    return Event(name);
}

void TestForwarding(const NgxModuleScanner &scanner, bool preload, bool vulkan)
{
    g_case = preload ? "forwarding / preloaded target" : "forwarding / lazy target";
    HANDLE attached = TargetEvent(L"Attach");
    HANDLE detached = TargetEvent(L"Detach");
    constexpr const char *names[] = {
        "NVSDK_NGX_D3D11_CreateFeature",
        "NVSDK_NGX_D3D11_EvaluateFeature",
        "NVSDK_NGX_D3D11_EvaluateFeature_C",
        "NVSDK_NGX_VULKAN_CreateFeature",
        "NVSDK_NGX_VULKAN_CreateFeature1",
        "NVSDK_NGX_VULKAN_EvaluateFeature",
        "NVSDK_NGX_VULKAN_EvaluateFeature_C"
    };
    for (unsigned cycle = 0; cycle != kCycles; ++cycle) {
        Check(ResetEvent(attached) != FALSE && ResetEvent(detached) != FALSE, "reset target events");
        Check(GetModuleHandleW(L"scan-target.dll") == nullptr, "forward target leaked from prior cycle");
        Witness targetWitness, sourceWitness;
        HMODULE targetOwner = preload ? Load(L"scan-target.dll", targetWitness) : nullptr;
        Check(Signaled(attached) == preload, "target attach event calibration");
        HMODULE source = Load(L"scan-forward.dll", sourceWitness);
        Check(Signaled(attached) == preload, "loading forwarding source eagerly loaded target");
        HMODULE held = Scan(scanner, source, vulkan, NgxScanResult::acquired);
        Check(Signaled(attached) == preload,
              "PE prescreen loaded the forwarded target (GetProcAddress inside Acquire)");
        Check(!Signaled(detached), "prescreen transiently loaded/unloaded the forwarded target");
        Check(preload || GetModuleHandleW(L"scan-target.dll") == nullptr,
              "PE prescreen must not load the forwarded target");
        Release(source);
        Alive(sourceWitness);
        ValueFn functions[7] = {};
        for (unsigned i = 0; i != 7; ++i)
            functions[i] = Proc<ValueFn>(held, names[i]);
        Check(Signaled(attached), "outside-lock GetProcAddress must load the real target");
        HMODULE target = GetModuleHandleW(L"scan-target.dll");
        Check(target != nullptr && !Signaled(detached), "forward target missing after resolution");
        if (!preload)
            Observe(target, targetWitness);
        ValueFn actual = Proc<ValueFn>(target, "ScanFixtureValue");
        for (ValueFn function : functions)
            Check(function == actual, "NGX export must resolve to the separate target DLL");
        if (targetOwner != nullptr)
            Release(targetOwner);
        const bool targetAlive = targetWitness.detaches == 0 && !Signaled(detached) &&
                                 GetModuleHandleW(L"scan-target.dll") == target;
        std::printf("OBSERVE forward preload=%d vulkan=%d cycle=%u: source-owned=1 target-alive=%d\n",
                    preload ? 1 : 0, vulkan ? 1 : 0, cycle + 1, targetAlive ? 1 : 0);
        Check(targetAlive,
              "unsafe forwarded pointer: source owned ref did not retain target after external FreeLibrary");
        for (ValueFn function : functions)
            Call(function);
        Alive(targetWitness);
        Release(held);
        const bool targetGone = targetWitness.detaches == 1 && Signaled(detached) &&
                                GetModuleHandleW(L"scan-target.dll") == nullptr;
        std::printf("OBSERVE final source release: source-detach=%ld target-detach=%ld target-unloaded=%d\n",
                    sourceWitness.detaches, targetWitness.detaches, targetGone ? 1 : 0);
        Detached(L"scan-forward.dll", sourceWitness);
        Check(targetGone, "forward target leaked after final source release; no owned target ref exists to release");
        Detached(L"scan-target.dll", targetWitness);
        Stale(scanner, source);
        Stale(scanner, target);
    }
    Close(attached);
    Close(detached);
    std::printf("PASS %s vulkan=%d: %u cycles; all seven forwarded names\n",
                g_case, vulkan ? 1 : 0, kCycles);
}

struct Watchdog {
    HANDLE ready;
    HANDLE stop;
};

DWORD WINAPI Watch(void *context)
{
    auto &watchdog = *static_cast<Watchdog *>(context);
    if (SetEvent(watchdog.ready) &&
        WaitForSingleObject(watchdog.stop, kSuiteTimeoutMs) == WAIT_OBJECT_0)
        return 0;
    constexpr char message[] = "FAIL scan-test watchdog: suite exceeded 60 seconds or event failed\r\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), message,
              static_cast<DWORD>(sizeof(message) - 1), &written, nullptr);
    TerminateProcess(GetCurrentProcess(), 1);
    return 1;
}
} // namespace

int main()
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    wchar_t executable[32768] = {};
    const DWORD length = GetModuleFileNameW(nullptr, executable,
                                            static_cast<DWORD>(_countof(executable)));
    Check(length != 0 && length < _countof(executable), "executable path");
    g_directory.assign(executable, length);
    const auto slash = g_directory.find_last_of(L"\\/");
    Check(slash != std::wstring::npos, "executable directory");
    g_directory.resize(slash + 1);
    InitializeCriticalSection(&g_scanCs);
    Watchdog watchdog = {Event(), Event()};
    HANDLE watchThread = CreateThread(nullptr, 0, &Watch, &watchdog, 0, nullptr);
    Check(watchThread != nullptr, "CreateThread watchdog");
    Wait(watchdog.ready);
    const NgxModuleScanner scanner;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    Check(ntdll != nullptr, "ntdll.dll");
    const LockFn lock = Proc<LockFn>(ntdll, "LdrLockLoaderLock");
    const UnlockFn unlock = Proc<UnlockFn>(ntdll, "LdrUnlockLoaderLock");
    std::puts("INFO real loader APIs; unavailable branch is not fault-injected; failures are fail-fast.");

    g_case = "invalid HMODULE";
    Stale(scanner, nullptr);
    Stale(scanner, reinterpret_cast<HMODULE>(static_cast<ULONG_PTR>(1)));
    Witness invalidWitness;
    HMODULE plain = Load(L"scan-plain.dll", invalidWitness);
    Stale(scanner, reinterpret_cast<HMODULE>(reinterpret_cast<ULONG_PTR>(plain) + 1));
    Release(plain);
    Detached(L"scan-plain.dll", invalidWitness);
    Stale(scanner, plain);
    std::puts("PASS null, unreadable, interior and unloaded HMODULE");

    constexpr ScanCase cases[] = {
        {"non-candidate", L"scan-plain.dll", false, false},
        {"D11 create only", L"scan-d11-create.dll", false, false},
        {"D11 evaluate only", L"scan-d11-eval.dll", false, false},
        {"D11 evaluate_C only", L"scan-d11-eval-c.dll", false, false},
        {"Vulkan create only", L"scan-vk-create.dll", true, false},
        {"Vulkan create1 only", L"scan-vk-create1.dll", true, false},
        {"Vulkan evaluate only", L"scan-vk-eval.dll", true, false},
        {"Vulkan evaluate_C only", L"scan-vk-eval-c.dll", true, false},
        {"D11 create + Vulkan evaluate", L"scan-cross-d11.dll", true, false},
        {"Vulkan create + D11 evaluate", L"scan-cross-vk.dll", true, false},
        {"export near matches", L"scan-near.dll", true, false},
        {"D11", L"scan-d11.dll", false, true},
        {"D11_C", L"scan-d11-c.dll", false, true},
        {"D11 with Vulkan scanning", L"scan-d11.dll", true, true},
        {"D11_C with Vulkan scanning", L"scan-d11-c.dll", true, true},
        {"Vulkan create/evaluate", L"scan-vk.dll", true, true},
        {"Vulkan create/evaluate_C", L"scan-vk-c.dll", true, true},
        {"Vulkan create1/evaluate", L"scan-vk1.dll", true, true},
        {"Vulkan create1/evaluate_C", L"scan-vk1-c.dll", true, true},
        {"Vulkan rejected without flag", L"scan-vk.dll", false, false},
        {"Vulkan_C rejected without flag", L"scan-vk-c.dll", false, false},
        {"Vulkan1 rejected without flag", L"scan-vk1.dll", false, false},
        {"Vulkan1_C rejected without flag", L"scan-vk1-c.dll", false, false}
    };
    for (const ScanCase &test : cases)
        TestCase(scanner, test);
    TestContention(scanner, lock, unlock);
    TestForwarding(scanner, false, false);
    TestForwarding(scanner, true, false);
    TestForwarding(scanner, false, true);
    TestForwarding(scanner, true, true);

    g_case = "shutdown";
    Signal(watchdog.stop);
    Wait(watchThread);
    Close(watchThread);
    Close(watchdog.ready);
    Close(watchdog.stop);
    DeleteCriticalSection(&g_scanCs);
    std::printf("PASS scan-test: %u assertions; no game, NGX runtime or module API mocks\n", g_checks);
    return 0;
}
