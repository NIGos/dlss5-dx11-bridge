// CPU-only Trace=1 contract test. No DllMain, Vulkan device, game or NR module.
// Run through test-fg-trace.cmd; all output belongs to its exclusive evidence directory.
#include "../../src/dlss5-bridge.cpp"
#include <atomic>
#include <thread>

namespace fgt_test {
static std::atomic<unsigned> checks{0}, failures{0};
static void Check(bool ok, const char *text)
{
    checks.fetch_add(1, std::memory_order_relaxed);
    if (!ok) {
        failures.fetch_add(1, std::memory_order_relaxed);
        std::printf("FAIL: %s\n", text);
    }
}
static void *Ptr(uint64_t value) { return reinterpret_cast<void *>(value); }
static uint64_t Bits(const void *value) { return reinterpret_cast<uint64_t>(value); }

enum Kind { Present, Tag, TagFrame, Submit, Submit2, GetQueue, GetQueue2, GetQueueLayer, KindCount };
struct Expect {
    Kind kind;
    void *object;
    const void *info, *pnext, *viewport, *frame;
    void *cmd;
    uint32_t count, family, index;
    uint64_t fence;
    void **output;
    void *queue_result;
    uint32_t result;
};
static thread_local Expect expected{};
static std::atomic<unsigned> forwards[KindCount]{};
static void Forward(Kind kind, bool same)
{
    forwards[kind].fetch_add(1, std::memory_order_relaxed);
    Check(expected.kind == kind && same, "original stub receives identical arguments/extension pointers");
}
static __declspec(noinline) uint32_t PresentOriginal(void *queue, const FgtPresentInfo *info)
{
    Forward(Present, queue == expected.object && info == expected.info &&
        (info == nullptr || info->pNext == expected.pnext));
    return expected.result;
}
static uint32_t TagOriginal(const void *viewport, const FgtSlResourceTag *tags, uint32_t num, void *cmd)
{
    Forward(Tag, viewport == expected.viewport && tags == expected.info && num == expected.count && cmd == expected.cmd);
    return expected.result;
}
static uint32_t TagFrameOriginal(const void *frame, const void *viewport, const FgtSlResourceTag *tags, uint32_t num, void *cmd)
{
    Forward(TagFrame, frame == expected.frame && viewport == expected.viewport && tags == expected.info &&
        num == expected.count && cmd == expected.cmd);
    return expected.result;
}
static uint32_t SubmitOriginal(void *queue, uint32_t num, const FgtSubmitInfo *info, uint64_t fence)
{
    Forward(Submit, queue == expected.object && num == expected.count && info == expected.info && fence == expected.fence &&
        (info == nullptr || info->pNext == expected.pnext));
    return expected.result;
}
static uint32_t Submit2Original(void *queue, uint32_t num, const FgtSubmitInfo2 *info, uint64_t fence)
{
    Forward(Submit2, queue == expected.object && num == expected.count && info == expected.info && fence == expected.fence &&
        (info == nullptr || info->pNext == expected.pnext));
    return expected.result;
}
static void QueueOriginal(void *dev, uint32_t family, uint32_t index, void **queue)
{
    Forward(GetQueue, dev == expected.object && family == expected.family && index == expected.index && queue == expected.output);
    if (queue != nullptr) *queue = expected.queue_result;
}
struct QueueInfo2 { uint32_t sType; const void *pNext; uint32_t flags, family, index; };
static_assert(sizeof(QueueInfo2) == 32 && offsetof(QueueInfo2, family) == 20, "VkDeviceQueueInfo2 ABI");
static void Queue2Original(void *dev, const void *info, void **queue)
{
    Forward(GetQueue2, dev == expected.object && info == expected.info && queue == expected.output &&
        (info == nullptr || static_cast<const QueueInfo2 *>(info)->pNext == expected.pnext));
    if (queue != nullptr) *queue = expected.queue_result;
}
static void QueueLayerOriginal(void *dev, uint32_t family, uint32_t index, void **queue)
{
    Forward(GetQueueLayer, dev == expected.object && family == expected.family && index == expected.index && queue == expected.output);
    if (queue != nullptr) *queue = expected.queue_result;
}
static void SetOriginals()
{
    g_fgt_control.present_hook.original = reinterpret_cast<void *>(&PresentOriginal);
    g_fgt_control.tag_hook.original = reinterpret_cast<void *>(&TagOriginal);
    g_fgt_control.tag_frame_hook.original = reinterpret_cast<void *>(&TagFrameOriginal);
    g_fgt_control.submit_hook.original = reinterpret_cast<void *>(&SubmitOriginal);
    g_fgt_control.submit2_hook.original = reinterpret_cast<void *>(&Submit2Original);
    g_fgt_control.get_queue_hook.original = reinterpret_cast<void *>(&QueueOriginal);
    g_fgt_control.get_queue2_hook.original = reinterpret_cast<void *>(&Queue2Original);
    g_fgt_control.get_queue_layer_hook.original = reinterpret_cast<void *>(&QueueLayerOriginal);
}

// SynthParams implements the ordinary NGX slots but intentionally rejects DLSSG.*.
// Add only the FG getters in this test subclass; never modify the production table.
static const char *const resource_keys[] = { "DLSSG.HUDLess", "DLSSG.Backbuffer", "DLSSG.MVecs", "DLSSG.Depth",
    "DLSSG.OutputReal", "DLSSG.OutputInterpolated" };
class Params final : public SynthParams {
public:
    using SynthParams::Get;
    MVkResource resources[6]{};
    bool available[6] = { true, true, true, true, true, true };
    unsigned idx = 1;
    bool indexed = true, signed_index = false;
    void *queue = Ptr(0x100012345678ull);
    Params()
    {
        Set("Width", 5120u);
        constexpr uint32_t mv_format = 83; // VK_FORMAT_R16G16_SFLOAT
        for (unsigned i = 0; i < _countof(resources); ++i) {
            resources[i].iv.ImageView = 0x900000000ull + i * 0x100u;
            resources[i].iv.aspectMask = 1;
            resources[i].iv.levelCount = resources[i].iv.layerCount = 1;
            resources[i].iv.Format = i == 2 ? mv_format : i == 3 ? kVkFmtR32Sfloat : kVkFmtR8G8B8A8Unorm;
            resources[i].iv.Width = 5120;
            resources[i].iv.Height = 2160;
        }
        Ring(0);
    }
    void Ring(unsigned ring)
    {
        for (unsigned i = 0; i < _countof(resources); ++i)
            resources[i].iv.Image = 0x800000000ull + i * 0x100u + ring;
    }
    NVSDK_NGX_Result Get(const char *key, void **out) const override
    {
        if (strcmp(key, "DLSSG.CmdQueue") == 0) { *out = queue; return NGX_SUCCESS; }
        for (unsigned i = 0; i < _countof(resources); ++i)
            if (strcmp(key, resource_keys[i]) == 0) {
                if (!available[i]) return kSynthNoSuchKey;
                *out = const_cast<MVkResource *>(&resources[i]);
                return NGX_SUCCESS;
            }
        return SynthParams::Get(key, out);
    }
    NVSDK_NGX_Result Get(const char *key, unsigned *out) const override
    {
        if (strcmp(key, "DLSSG.MultiFrameIndex") != 0) return SynthParams::Get(key, out);
        if (!indexed || signed_index) return kSynthNoSuchKey;
        *out = idx;
        return NGX_SUCCESS;
    }
    NVSDK_NGX_Result Get(const char *key, int *out) const override
    {
        if (strcmp(key, "DLSSG.MultiFrameIndex") != 0) return SynthParams::Get(key, out);
        if (!indexed || !signed_index) return kSynthNoSuchKey;
        *out = static_cast<int>(idx);
        return NGX_SUCCESS;
    }
};
struct Extension { uint32_t sType; const void *pNext; uint64_t cookie; };
struct Fixture {
    Extension last{ 0x1234u, nullptr, 0xfedcba9876543210ull };
    uint64_t waits[2] = { 0x100000001ull, 0x100000002ull };
    uint64_t signals[2] = { 0x200000001ull, 0x200000002ull };
    uint64_t wait_values[2] = { 0x1000000001ull, 0x2000000002ull };
    uint64_t signal_values[2] = { 0x3000000003ull, 0x4000000004ull };
    uint32_t stages[2] = { 0x1000u, 0x2000u };
    void *commands[4] = { Ptr(0x301), Ptr(0x302), Ptr(0x303), Ptr(0x304) };
    uint64_t swapchains[2] = { 0x400000001ull, 0x400000002ull };
    uint32_t indices[2] = { 2, 1 }, results[2] = { 0x76543210u, 0xabcdef12u };
    FgtTimelineSubmitInfo timeline{ kFgtStTimelineSubmitInfo, &last, 2, wait_values, 2, signal_values };
    Extension first{ 0x5678u, &timeline, 0x123456789abcdef0ull };
    FgtPresentInfo present{ 1000001001u, &last, 2, waits, 2, swapchains, indices, results };
    FgtSubmitInfo submit{ 4, &first, 2, waits, stages, 4, commands, 2, signals };
    FgtSemaphoreSubmitInfo wait2[2]{};
    FgtSemaphoreSubmitInfo signal2[2]{};
    FgtCommandBufferSubmitInfo cmd2[4]{};
    FgtSubmitInfo2 submit2{ 1000314004u, &last, 0, 2, wait2, 4, cmd2, 2, signal2 };
    QueueInfo2 queue_info{ 1000145003u, &last, 1, 7, 3 };
    FgtSlResource resource{};
    FgtSlResourceTag tags[8]{};
    Fixture()
    {
        for (unsigned i = 0; i < 2; ++i) {
            wait2[i] = { 1000314005u, &last, waits[i], wait_values[i], 0x100000000ull << i, i };
            signal2[i] = { 1000314005u, &last, signals[i], signal_values[i], 0x400000000ull << i, i };
        }
        for (unsigned i = 0; i < 4; ++i) cmd2[i] = { 1000314006u, &last, commands[i], 1u << i };
        resource.native = Ptr(0x900012345678ull); resource.view = Ptr(0x900087654321ull);
        resource.state = 17; resource.width = 5120; resource.height = 2160;
        resource.nativeFormat = 37; resource.usage = 0x1bu;
        for (unsigned i = 0; i < _countof(tags); ++i) {
            // BaseStructure next is opaque to the observer and must stay untouched.
            const void *next = &last;
            memcpy(tags[i].base, &next, sizeof(next));
            tags[i].resource = i == 7 ? nullptr : &resource;
            tags[i].type = i < 6 ? kFgtTagTypes[i] : i == 6 ? 999u : 0u;
            tags[i].lifecycle = i < 6 ? i % 3 : 7;
            tags[i].top = 4; tags[i].left = 3; tags[i].width = 5000; tags[i].height = 2100;
        }
    }
};
static void CallPresent(Fixture &f, void *queue, PFN_FgtQueuePresent entry = &FgtPresentDetour, bool empty = false)
{
    expected = {};
    expected.kind = Present; expected.object = queue;
    expected.info = empty ? nullptr : &f.present; expected.pnext = f.present.pNext;
    expected.result = 0xf1234567u;
    Check(entry(queue, empty ? nullptr : &f.present) == expected.result, "present return bits unchanged");
}
static void CallTag(const FgtSlResourceTag *tags, uint32_t count, bool frame, void *opaque)
{
    expected = {};
    expected.kind = frame ? TagFrame : Tag; expected.info = tags; expected.count = count;
    expected.viewport = opaque; expected.frame = opaque; expected.cmd = Ptr(0x123456789abcdef0ull);
    expected.result = 0xbad00002u;
    const uint32_t result = frame ? FgtSetTagForFrameDetour(opaque, opaque, tags, count, expected.cmd)
        : FgtSetTagDetour(opaque, tags, count, expected.cmd);
    Check(result == expected.result, "tag return bits unchanged; frame/viewport remain opaque");
}
static void CallSubmit(Fixture &f, void *queue, bool second, bool empty = false)
{
    expected = {};
    expected.kind = second ? Submit2 : Submit; expected.object = queue;
    expected.count = empty ? 0u : 1u;
    expected.info = empty ? nullptr : second ? static_cast<const void *>(&f.submit2) : &f.submit;
    expected.pnext = second ? f.submit2.pNext : f.submit.pNext;
    expected.fence = empty ? 0ull : 0xfedcba9876543210ull; expected.result = 0x87654321u;
    const uint32_t result = second ? FgtQueueSubmit2Detour(queue, expected.count, static_cast<const FgtSubmitInfo2 *>(expected.info), expected.fence)
        : FgtQueueSubmitDetour(queue, expected.count, static_cast<const FgtSubmitInfo *>(expected.info), expected.fence);
    Check(result == expected.result, "submit return bits and empty-submit forwarding unchanged");
}
static void ResetStats()
{
    FgtFlushLog();
    FgtLock();
    g_fgt = {};
    g_fgt.cmdqueue_detail_left = 24; g_fgt.present_queue_detail_left = 8; g_fgt.eval_thread_detail_left = 12;
    g_fgt_queue_id_count = 0;
    FgtUnlock();
}
static bool Contains(const FgtSet &s, uint64_t value)
{
    for (unsigned i = 0; i < s.n; ++i) if (s.v[i] == value) return true;
    return false;
}
static void TestEvaluate()
{
    ResetStats();
    Params p;
    Fixture f;
    unsigned width = 0;
    Check(p.Get("Width", &width) == NGX_SUCCESS && width == 5120, "SynthParams base getters retained");
    for (unsigned group = 0; group < 6; ++group) {
        p.Ring(group % 3);
        CallPresent(f, p.queue);
        const auto saved = p.resources[0];
        for (unsigned idx = 1; idx <= 3; ++idx) {
            p.idx = idx;
            FgTraceEvaluate(f.commands[idx - 1], &p);
            Check(g_fgt.last_fg_cmd == f.commands[0], "only idx1 updates the FG command identity");
        }
        Check(memcmp(&saved, &p.resources[0], sizeof(saved)) == 0, "evaluate borrows resource without mutation");
    }
    const FgtSet *inputs[] = { &g_fgt.hud, &g_fgt.back, &g_fgt.mv, &g_fgt.depth };
    for (unsigned i = 0; i < _countof(inputs); ++i) {
        Check(inputs[i]->n == 3 && inputs[i]->hits == 6 && inputs[i]->overflow == 0, "four inputs retain a three-image ring at group frequency");
        for (unsigned ring = 0; ring < 3; ++ring)
            Check(Contains(*inputs[i], 0x800000000ull + i * 0x100u + ring), "image identity, not image-view identity, recorded");
    }
    Check(g_fgt.eval_calls == 18 && g_fgt.eval_groups == 6 && g_fgt.eval_indices.n == 3, "idx1/2/3 calls versus groups");
    Check(g_fgt.out_real.hits == 6 && g_fgt.out_interp.hits == 18, "output real/group and interpolated/call statistics");
    Check(g_fgt.present_to_eval.n == 6 && g_fgt.eval_interval.n == 5 && g_fgt.eval_group_span.n == 12, "timing sample counts");
    p.idx = 0; FgTraceEvaluate(f.commands[3], &p);
    p.idx = 5; FgTraceEvaluate(f.commands[3], &p);
    Check(g_fgt.eval_groups == 6, "idx0 and idx5 do not open groups");
    p.signed_index = true; p.idx = 1; FgTraceEvaluate(f.commands[3], &p);
    Check(g_fgt.eval_groups == 7, "signed MultiFrameIndex fallback");
    p.indexed = false; FgTraceEvaluate(f.commands[3], &p);
    p.available[4] = false; FgTraceEvaluate(f.commands[3], &p);
    Check(g_fgt.eval_groups == 8, "unindexed group requires OutputReal");
    p.indexed = true; p.idx = 1; p.available[0] = false;
    p.resources[1].Type = 1; p.resources[2].iv.Image = 0;
    FgTraceEvaluate(f.commands[3], &p);
    Check(g_fgt.hud.hits == 8 && g_fgt.back.hits == 8 && g_fgt.mv.hits == 8 && g_fgt.depth.hits == 9,
        "missing, non-image and zero-image inputs excluded independently");
    FgtSet bounded{};
    for (unsigned i = 0; i < kFgtDistinct + 3; ++i) FgtNote(bounded, i);
    FgtNote(bounded, 0);
    Check(bounded.n == kFgtDistinct && bounded.overflow == 3 && bounded.hits == kFgtDistinct + 1, "distinct-set overflow bounded and counted");
    FgtSummary();
    std::puts("COVERED: Trace=1 evaluate, four three-image rings, indices, missing resources, bounded sets");
}
static void TestDetoursAndParsers(void *guard)
{
    ResetStats();
    Fixture f;
    unsigned char before[sizeof(Fixture)];
    memcpy(before, &f, sizeof(f));
    CallPresent(f, Ptr(0x501));
    CallPresent(f, Ptr(0x501), &FgtPresentDetour, true);
    CallTag(f.tags, 8, false, guard);
    CallTag(f.tags, 8, true, guard);
    Check(g_fgt.tag_calls == 1 && g_fgt.tag_frame_calls == 1 && g_fgt.tag_bad_reads == 0, "all tag types and missing-resource tag accepted");
    Check(g_fgt.types[0].count == 4 && g_fgt.types[0].lifecycle[0] == 2 && g_fgt.types[0].lifecycle_other == 2 &&
        Contains(g_fgt.types[0].natives, 0), "missing resource is a zero-handle observation, not an unreadable tag");
    for (unsigned i = 1; i < 6; ++i) {
        const FgtTagType &t = g_fgt.types[i];
        Check(t.count == 2 && t.lifecycle[i % 3] == 2 && t.cmd_nonnull == 2 && t.extent_nonzero == 2 &&
            t.w == 5120 && t.h == 2160 && t.state == 17 && t.fmt == 37 && t.usage == 0x1b &&
            Contains(t.natives, Bits(f.resource.native)) && Contains(t.views, Bits(f.resource.view)), "tag lifecycle, extent and resource metadata");
    }
    Check(Contains(g_fgt.tag_other_types, 999), "unknown tag type counted");
    CallTag(static_cast<const FgtSlResourceTag *>(guard), 1, false, guard);
    CallTag(nullptr, 1, true, guard);
    FgtSlResourceTag bad_resource{}; bad_resource.resource = static_cast<const FgtSlResource *>(guard);
    CallTag(&bad_resource, 1, true, guard);
    CallTag(nullptr, 0, false, guard);
    Check(g_fgt.tag_bad_reads == 3, "SEH contains inaccessible tag/resource and null tag; originals still called");

    FgtSubmitView view{};
    FgtViewSubmit(&f.submit, f.commands[3], &view);
    Check(view.waits == 2 && view.cbs == 4 && view.signals == 2 && view.timeline && view.has_fg_cmd,
        "VkSubmitInfo traverses unknown pNext before timeline; FG cmd found beyond three printed entries");
    Check(view.wait_val[1] == f.wait_values[1] && view.signal_val[1] == f.signal_values[1] &&
        view.wait_sem[1] == f.waits[1] && view.signal_sem[1] == f.signals[1] && view.cb[2] == f.commands[2], "timeline values and handles preserve 64 bits");
    FgtSubmitInfo binary = f.submit; binary.pNext = &f.last;
    view = {}; FgtViewSubmit(&binary, nullptr, &view);
    Check(!view.timeline && !view.has_fg_cmd && view.wait_val[0] == 0, "VkSubmitInfo without timeline chain is not timeline");
    view = {}; FgtViewSubmit2(&f.submit2, f.commands[3], &view);
    Check(view.waits == 2 && view.cbs == 4 && view.signals == 2 && view.has_fg_cmd &&
        view.wait_sem[0] == f.waits[0] && view.signal_sem[1] == f.signals[1] &&
        view.wait_val[0] == f.wait_values[0] && view.signal_val[1] == f.signal_values[1], "submit2 nested handles/values and fourth FG cmd parsed");
    Check(!view.timeline, "submit2 structure/value alone must not claim a known timeline semaphore");
    FgtSubmitInfo2 empty2{};
    view = {}; FgtViewSubmit2(&empty2, nullptr, &view);
    Check(!view.timeline && view.waits == 0 && view.signals == 0 && view.cbs == 0, "empty submit2 is not proof of timeline usage");
    CallSubmit(f, Ptr(0x501), false);
    CallSubmit(f, Ptr(0x501), true);
    CallSubmit(f, Ptr(0x501), false, true);
    CallSubmit(f, Ptr(0x501), true, true);
    const FgtQueue &q = g_fgt.queues[0];
    Check(q.submits == 4 && q.cbs == 8 && q.waits == 4 && q.signals == 4 && q.fences == 2, "full and empty submit statistics");
    Check(q.submits2 == 2, "zero-count vkQueueSubmit2 remains a submit2 API call");
    Check(q.timeline == 1, "only explicit VkTimelineSemaphoreSubmitInfo contributes known timeline evidence");
    for (unsigned i = 0; i < 3; ++i) {
        void *output = nullptr;
        expected = {}; expected.kind = i == 0 ? GetQueue : i == 1 ? GetQueue2 : GetQueueLayer;
        expected.object = Ptr(0xface12345678ull); expected.family = i + 6; expected.index = i + 2;
        expected.output = &output; expected.queue_result = Ptr(0x550000000ull + i);
        expected.info = &f.queue_info; expected.pnext = f.queue_info.pNext;
        if (i == 0) FgtGetDeviceQueueDetour(expected.object, expected.family, expected.index, &output);
        else if (i == 1) FgtGetDeviceQueue2Detour(expected.object, &f.queue_info, &output);
        else FgtGetDeviceQueueLayerDetour(expected.object, expected.family, expected.index, &output);
        Check(output == expected.queue_result && g_fgt_queue_ids[i].queue == Bits(output) &&
            g_fgt_queue_ids[i].family == expected.family && g_fgt_queue_ids[i].index == expected.index,
            "get-queue/device-queue2/layer output and family/index preserved");
    }
    Check(memcmp(before, &f, sizeof(f)) == 0, "all input structs, nested arrays, pNext chains and resource payloads remain byte-identical");
    for (unsigned i = 0; i < 10; ++i) CallSubmit(f, Ptr(0x600u + i), false, true);
    Check(g_fgt.nqueues == 8 && g_fgt.queue_overflow == 3, "queue-table overflow counted without stopping forwarding");
    FgtSummary();
    std::puts("COVERED: eight real detours, identity/return bits, tag SEH, timeline chain, submit2 and empty submissions");
}
static __declspec(noinline) uint32_t InvokePresent(PFN_FgtQueuePresent entry, void *queue, const FgtPresentInfo *info)
{
    // Indirect, non-tail call prevents /O2 from bypassing the patched stub entry.
    volatile uint32_t result = entry(queue, info);
    return result;
}
static void TestMinHook()
{
    Fixture f;
    Check(MH_Initialize() == MH_OK, "MinHook initializes in this executable only");
    void *target = reinterpret_cast<void *>(&PresentOriginal);
    void *trampoline = nullptr;
    const MH_STATUS created = MH_CreateHook(target, reinterpret_cast<void *>(&FgtPresentDetour), &trampoline);
    Check(created == MH_OK && trampoline != nullptr && trampoline != target, "real MinHook trampoline created for self-owned present stub");
    if (created == MH_OK) {
        g_fgt_control.present_hook.original = trampoline;
        const MH_STATUS enabled = MH_EnableHook(target);
        Check(enabled == MH_OK, "real present detour enabled");
        if (enabled == MH_OK) {
            const unsigned p0 = g_fgt.present_calls, n0 = forwards[Present].load();
            expected = {}; expected.kind = Present; expected.object = Ptr(0x77); expected.info = &f.present;
            expected.pnext = f.present.pNext; expected.result = 0x80000005u;
            Check(InvokePresent(&PresentOriginal, expected.object, &f.present) == expected.result &&
                g_fgt.present_calls == p0 + 1 && forwards[Present].load() == n0 + 1, "patched stub enters actual detour then trampoline exactly once");
            Check(InvokePresent(reinterpret_cast<PFN_FgtQueuePresent>(trampoline), expected.object, &f.present) == expected.result &&
                g_fgt.present_calls == p0 + 1 && forwards[Present].load() == n0 + 2, "direct trampoline bypasses detour and retains return value");
            Check(MH_DisableHook(target) == MH_OK, "self hook disabled");
        }
        Check(MH_RemoveHook(target) == MH_OK, "self hook removed");
    }
    g_fgt_control.present_hook.original = reinterpret_cast<void *>(&PresentOriginal);
    Check(MH_Uninitialize() == MH_OK, "MinHook uninitialized");
    std::puts("COVERED: real MinHook patch and relocated trampoline, no game DLL");
}
static unsigned LogCount()
{
    AcquireSRWLockExclusive(&g_fgt_log_lock);
    const unsigned result = g_fgt_log_count;
    ReleaseSRWLockExclusive(&g_fgt_log_lock);
    return result;
}
static void TestConcurrent(void *guard)
{
    ResetStats();
    const LONG drops_before = InterlockedCompareExchange(&g_fgt_log_dropped, 0, 0);
    for (unsigned i = 0; i < _countof(g_fgt_logs) + 17; ++i) FgtLog("[fg-trace-test] bounded log %u", i);
    Check(LogCount() == _countof(g_fgt_logs) && InterlockedCompareExchange(&g_fgt_log_dropped, 0, 0) == drops_before + 17,
        "exact full-queue drop accounting");
    // Holding the producer lock is deliberate fault injection, not a production lock order.
    AcquireSRWLockExclusive(&g_fgt_log_lock);
    std::thread blocked_producer([] { FgtLog("[fg-trace-test] contention must drop, not wait"); });
    blocked_producer.join();
    ReleaseSRWLockExclusive(&g_fgt_log_lock);
    Check(InterlockedCompareExchange(&g_fgt_log_dropped, 0, 0) == drops_before + 18, "contended producer drop counted without blocking");
    FgtFlushLog();
    Check(LogCount() == 0, "flush empties a full bounded queue");
    const unsigned rounds = 1200;
    unsigned initial[KindCount];
    for (unsigned i = 0; i < KindCount; ++i) initial[i] = forwards[i].load();
    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    auto barrier = [&] { ready.fetch_add(1); while (!go.load()) std::this_thread::yield(); };
    std::thread presenters([&] {
        Fixture f; barrier();
        for (unsigned i = 0; i < rounds; ++i) { f.indices[0] = i % 3; CallPresent(f, Ptr(0x700)); }
    });
    std::thread taggers([&] {
        Fixture f; barrier();
        for (unsigned i = 0; i < rounds; ++i) CallTag(f.tags, 1, (i % 2) != 0, guard);
    });
    std::thread submitters([&] {
        Fixture f; barrier();
        for (unsigned i = 0; i < rounds; ++i) CallSubmit(f, Ptr(0x700), (i % 2) != 0);
    });
    std::thread evaluators([&] {
        Params p; p.queue = Ptr(0x700); barrier();
        for (unsigned i = 0; i < rounds; ++i) {
            p.Ring(i % 3);
            for (unsigned idx = 1; idx <= 3; ++idx) { p.idx = idx; FgTraceEvaluate(Ptr(0x304), &p); }
        }
    });
    std::thread summaries([&] {
        barrier();
        for (unsigned i = 0; i < 80; ++i) FgtSummary();
    });
    while (ready.load() != 5) std::this_thread::yield();
    go.store(true);
    presenters.join(); taggers.join(); submitters.join(); evaluators.join(); summaries.join();
    Check(g_fgt.present_calls == rounds && g_fgt.present_duration.n == rounds && g_fgt.present_interval.n == rounds - 1 &&
        g_fgt.present_indices.n == 3, "concurrent present counters and indices exact");
    Check(g_fgt.tag_calls == rounds / 2 && g_fgt.tag_frame_calls == rounds / 2 && g_fgt.types[0].count == rounds &&
        g_fgt.tag_bad_reads == 0, "concurrent tag counters exact");
    Check(g_fgt.eval_calls == rounds * 3 && g_fgt.eval_groups == rounds && g_fgt.eval_group_span.n == rounds * 2 &&
        g_fgt.hud.hits == rounds && g_fgt.back.hits == rounds && g_fgt.mv.hits == rounds && g_fgt.depth.hits == rounds &&
        g_fgt.hud.n == 3 && g_fgt.back.n == 3 && g_fgt.mv.n == 3 && g_fgt.depth.n == 3, "concurrent evaluate counts and four input rings exact");
    Check(g_fgt.nqueues == 1 && g_fgt.queues[0].submits == rounds && g_fgt.queues[0].submits2 == rounds / 2 &&
        g_fgt.queues[0].cbs == rounds * 4 && g_fgt.queues[0].waits == rounds * 2 && g_fgt.queues[0].signals == rounds * 2,
        "concurrent submit counters exact");
    Check(forwards[Present].load() - initial[Present] == rounds && forwards[Tag].load() - initial[Tag] == rounds / 2 &&
        forwards[TagFrame].load() - initial[TagFrame] == rounds / 2 && forwards[Submit].load() - initial[Submit] == rounds / 2 &&
        forwards[Submit2].load() - initial[Submit2] == rounds / 2, "full log queue never interrupts original forwarding");
    Check(LogCount() == _countof(g_fgt_logs) && InterlockedCompareExchange(&g_fgt_log_dropped, 0, 0) > drops_before + 18,
        "simultaneous present/tag/submit/evaluate/summary saturates bounded logging with counted drops");
    FgtFlushLog();
    Check(LogCount() == 0, "flush drains concurrent producers after join");
    FgtSummary(); FgtFlushLog();
    Check(LogCount() == 0, "summary is queued and flushable after overflow");
    std::printf("COVERED: five concurrent roles; present/tag/submit=%u each; evaluates=%u; dropped=%ld\n",
        rounds, rounds * 3, InterlockedCompareExchange(&g_fgt_log_dropped, 0, 0));
}
static void CheckIsolation()
{
    const wchar_t *const excluded[] = { L"sl.interposer.dll", L"sl.dlss_g.dll", L"nvngx_dlss.dll", L"nvngx_dlssd.dll",
        L"nvngx_dlssg.dll", L"nvngx_dlssnr.dll", L"renodx-dlss5.addon64", L"ReShade64.dll", L"vulkan-1.dll" };
    for (const wchar_t *name : excluded) Check(GetModuleHandleW(name) == nullptr, "no game/NR/Vulkan runtime module loaded");
    Check(!g_watching_ngx && g_worker_running == 0 && g_ldr_cookie == nullptr && g_layer_count == 0,
        "no NGX scanner or DLL notification registration started");
    Check(!g_fgt_control.early_tried && !g_fgt_control.late_tried && !g_fgt_control.queue_hooks_tried &&
        !g_fgt_control.layer_hook_tried && g_fgt_submit_target == nullptr && g_fgt_submit2_target == nullptr && g_fgt_queue_target == nullptr,
        "render observations never perform maintenance hook installation");
}
} // namespace fgt_test

int main()
{
    using namespace fgt_test;
    g_self = GetModuleHandleW(nullptr);
    InitializeCriticalSection(&g_log_cs);
    InitializeCriticalSection(&g_hook_cs);
    InitializeCriticalSection(&g_ngx_cs); g_ngx_cs_ready = true;
    InitializeCriticalSection(&g_bridge_cs);
    g_watching_ngx = false;
    strcpy_s(g_log_path, "fg-trace.log");
    CheckIsolation();
    Check(PresentationAdapterEnabled() && PresentAdapterFgInput() && FgtEnabled() && PresentAdapterPipeline() == 0,
        "actual executable-relative INI enables Trace=1, Source=fg-input, Pipeline=0");
    if (failures.load() != 0) return 2;
    Params before_init;
    FgTraceEvaluate(nullptr, &before_init);
    Check(g_fgt.eval_calls == 0, "evaluate before FgtInit is safely ignored");
    FgtInit(); FgtInit();
    Check(g_fgt_control.cs_ready && g_fgt.cmdqueue_detail_left == 24, "FgtInit idempotent");
    SetOriginals();
    void *guard = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    Check(guard != nullptr, "owned inaccessible page for SEH and opaque frame tests");
    if (guard == nullptr) return 2;
    TestEvaluate();
    TestDetoursAndParsers(guard);
    TestMinHook();
    TestConcurrent(guard);
    CheckIsolation();
    Check(VirtualFree(guard, 0, MEM_RELEASE) != FALSE, "owned guard page released");
    FgtFlushLog();
    Check(LogCount() == 0, "final trace queue empty");
    FILE *log = nullptr;
    Check(fopen_s(&log, g_log_path, "rb") == 0 && log != nullptr, "trace log persisted in independent run directory");
    if (log != nullptr) { Check(fseek(log, 0, SEEK_END) == 0 && ftell(log) > 0, "trace log is nonempty"); fclose(log); }
    for (unsigned i = 0; i < KindCount; ++i) std::printf("original[%u]=%u\n", i, forwards[i].load());
    const unsigned failed = failures.load();
    std::printf("%s: checks=%u failures=%u; CPU synthetic contracts only, NOT game/GPU validation\n",
        failed == 0 ? "PASS" : "FAIL", checks.load(), failed);
    DeleteCriticalSection(&g_fgt_control.cs); g_fgt_control.cs_ready = false;
    DeleteCriticalSection(&g_bridge_cs);
    g_ngx_cs_ready = false; DeleteCriticalSection(&g_ngx_cs);
    DeleteCriticalSection(&g_hook_cs); DeleteCriticalSection(&g_log_cs);
    return failed == 0 ? 0 : 1;
}
