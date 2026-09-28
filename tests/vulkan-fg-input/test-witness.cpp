#include <windows.h>
#include <intrin.h>
#include <cstdio>
#include <cstdint>
#include <thread>

using Entry = int(*)(void*, const void*, const void*, void*);
extern "C" {
void* g_present_nr_original;
void* g_present_sr_original;
int PresentAdapterNrHook(void*, const void*, const void*, void*);
int PresentAdapterSrHook(void*, const void*, const void*, void*);
}
static unsigned before_count, target_count;
static thread_local int sr_result;
static uintptr_t sr_received[4];
extern "C" int PresentAdapterSrEnter(void* a, const void* b, const void* c, void* d) {
    sr_received[0] = reinterpret_cast<uintptr_t>(a);
    sr_received[1] = reinterpret_cast<uintptr_t>(b);
    sr_received[2] = reinterpret_cast<uintptr_t>(c);
    sr_received[3] = reinterpret_cast<uintptr_t>(d);
    return sr_result;
}
static bool scoped;
static void* target_return;
static uintptr_t received[4];
extern "C" void PresentAdapterNrEnter() { if (scoped) ++before_count; }
static __declspec(noinline) int Target(void* a, const void* b, const void* c, void* d) {
    ++target_count;
    target_return = _ReturnAddress();
    received[0] = reinterpret_cast<uintptr_t>(a);
    received[1] = reinterpret_cast<uintptr_t>(b);
    received[2] = reinterpret_cast<uintptr_t>(c);
    received[3] = reinterpret_cast<uintptr_t>(d);
    return static_cast<int>(0xbad00002u);
}
extern "C" void* g_present_nr_original = reinterpret_cast<void*>(&Target);
static __declspec(noinline) int Invoke(Entry fn) {
    volatile int result = fn(reinterpret_cast<void*>(0x1111222233334444ull),
                            reinterpret_cast<void*>(0x5555666677778888ull),
                            reinterpret_cast<void*>(0x9999aaaabbbbccccull),
                            reinterpret_cast<void*>(0xddddeeeeffff0000ull));
    return result;
}
int main() {
    const int baseline = Invoke(&Target);
    void* baseline_return = target_return;
    scoped = true;
    const int forwarded = Invoke(&PresentAdapterNrHook);
    if (baseline != forwarded || target_return != baseline_return || target_count != 2 || before_count != 1) return 1;
    if (received[0] != 0x1111222233334444ull || received[1] != 0x5555666677778888ull ||
        received[2] != 0x9999aaaabbbbccccull || received[3] != 0xddddeeeeffff0000ull) return 2;
    scoped = false;
    if (Invoke(&PresentAdapterNrHook) != baseline || target_count != 3 || before_count != 1) return 3;
    g_present_sr_original = reinterpret_cast<void*>(&Target);
    if (Invoke(&PresentAdapterSrHook) != baseline || target_count != 4 || target_return != baseline_return) return 4;
    for (unsigned i = 0; i < 4; ++i) if (sr_received[i] != received[i]) return 5;
    sr_result = 1;
    if (Invoke(&PresentAdapterSrHook) != 1 || target_count != 4) return 6;
    int other_thread_result = 0;
    std::thread other([&] { other_thread_result = Invoke(&PresentAdapterSrHook); });
    other.join();
    if (other_thread_result != baseline || target_count != 5 || target_return != baseline_return) return 7;
    if (Invoke(&PresentAdapterSrHook) != 1 || target_count != 5) return 8;
    sr_result = 0;
    if (Invoke(&PresentAdapterSrHook) != baseline || target_count != 6 || target_return != baseline_return) return 9;
    std::puts("PASS NR tail forwarding; SR identity-return gate, four arguments, caller address and thread-local isolation");
    return 0;
}
