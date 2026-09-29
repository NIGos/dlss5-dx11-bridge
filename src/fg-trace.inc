// Observations are CPU-side and unpaired: stable handles do not establish frame identity or GPU readiness.

static const unsigned kFgtDistinct = 12;
struct FgtSet { uint64_t v[kFgtDistinct]; unsigned n, overflow, hits; };

static int FgtNote(FgtSet &s, uint64_t x)
{
    for (unsigned i = 0; i < s.n; ++i)
        if (s.v[i] == x) { ++s.hits; return static_cast<int>(i); }
    if (s.n < kFgtDistinct) { s.v[s.n] = x; ++s.hits; return static_cast<int>(s.n++); }
    ++s.overflow;
    return -1;
}

static void FgtSetText(const FgtSet &s, char *buf, size_t cap)
{
    size_t len = 0;
    FgCensusAppend(buf, cap, &len, "n=%u hits=%u", s.n, s.hits);
    if (s.overflow != 0) FgCensusAppend(buf, cap, &len, " overflow=%u", s.overflow);
    FgCensusAppend(buf, cap, &len, " [");
    for (unsigned i = 0; i < s.n; ++i) FgCensusAppend(buf, cap, &len, "%s%llx", i ? " " : "", s.v[i]);
    FgCensusAppend(buf, cap, &len, "]");
}

struct FgtStat { double sum, min, max; unsigned n; };

static void FgtStatAdd(FgtStat &s, double x)
{
    if (s.n == 0 || x < s.min) s.min = x;
    if (s.n == 0 || x > s.max) s.max = x;
    s.sum += x;
    ++s.n;
}

static void FgtStatText(const FgtStat &s, char *buf, size_t cap)
{
    if (s.n == 0) { strcpy_s(buf, cap, "n=0"); return; }
    _snprintf_s(buf, cap, _TRUNCATE, "n=%u avg=%.2f min=%.2f max=%.2f ms", s.n, s.sum / s.n, s.min, s.max);
}

static double FgtMs(LONGLONG from, LONGLONG to)
{
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    return static_cast<double>(to - from) * 1000.0 / static_cast<double>(freq.QuadPart);
}

static LONGLONG FgtNow()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

// Vulkan 1.0/1.3 structures the observers read. Field offsets per the specification.
struct FgtPresentInfo {
    uint32_t sType; const void *pNext;
    uint32_t waitSemaphoreCount; const uint64_t *pWaitSemaphores;
    uint32_t swapchainCount; const uint64_t *pSwapchains; const uint32_t *pImageIndices; void *pResults;
};
struct FgtSubmitInfo {
    uint32_t sType; const void *pNext;
    uint32_t waitSemaphoreCount; const uint64_t *pWaitSemaphores; const uint32_t *pWaitDstStageMask;
    uint32_t commandBufferCount; void *const *pCommandBuffers;
    uint32_t signalSemaphoreCount; const uint64_t *pSignalSemaphores;
};
struct FgtSemaphoreSubmitInfo { uint32_t sType; const void *pNext; uint64_t semaphore; uint64_t value; uint64_t stageMask; uint32_t deviceIndex; };
struct FgtCommandBufferSubmitInfo { uint32_t sType; const void *pNext; void *commandBuffer; uint32_t deviceMask; };
struct FgtSubmitInfo2 {
    uint32_t sType; const void *pNext; uint32_t flags;
    uint32_t waitSemaphoreInfoCount; const FgtSemaphoreSubmitInfo *pWaitSemaphoreInfos;
    uint32_t commandBufferInfoCount; const FgtCommandBufferSubmitInfo *pCommandBufferInfos;
    uint32_t signalSemaphoreInfoCount; const FgtSemaphoreSubmitInfo *pSignalSemaphoreInfos;
};
struct FgtTimelineSubmitInfo { uint32_t sType; const void *pNext; uint32_t waitValueCount; const uint64_t *pWaitValues; uint32_t signalValueCount; const uint64_t *pSignalValues; };
static const uint32_t kFgtStTimelineSubmitInfo = 1000207003;   // VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO

static_assert(sizeof(FgtPresentInfo) == 64, "VkPresentInfoKHR");
static_assert(sizeof(FgtSubmitInfo) == 72, "VkSubmitInfo");
static_assert(sizeof(FgtSubmitInfo2) == 64, "VkSubmitInfo2");
static_assert(sizeof(FgtSemaphoreSubmitInfo) == 48, "VkSemaphoreSubmitInfo");
static_assert(sizeof(FgtCommandBufferSubmitInfo) == 32, "VkCommandBufferSubmitInfo");

// sl::Resource and sl::ResourceTag (Streamline 2.10.3 sl_core_types.h).
struct FgtSlResource {
    uint8_t base[32];
    uint8_t type; uint8_t pad[7];
    void *native; void *memory; void *view;
    uint32_t state, width, height, nativeFormat, mipLevels, arrayLayers;
    uint64_t gpuVirtualAddress;
    uint32_t flags, usage, reserved;
};
struct FgtSlResourceTag {
    uint8_t base[32];
    const FgtSlResource *resource;
    uint32_t type, lifecycle;
    uint32_t top, left, width, height;
};
static_assert(offsetof(FgtSlResource, native) == 40 && offsetof(FgtSlResource, state) == 64 &&
              offsetof(FgtSlResource, flags) == 96, "sl::Resource");
static_assert(sizeof(FgtSlResourceTag) == 64 && offsetof(FgtSlResourceTag, type) == 40, "sl::ResourceTag");

// The buffer types the FG input feed cares about, in sl::kBufferType order.
static const uint32_t kFgtTagTypes[] = { 0, 1, 2, 4, 23, 53 };
static const char *const kFgtTagNames[] = { "Depth", "MVec", "HUDLess", "ScalingOut", "UIColorAlpha", "Backbuffer" };

struct FgtTagType {
    unsigned count, lifecycle[3], lifecycle_other, cmd_nonnull, extent_nonzero;
    uint32_t state, w, h, fmt, usage;
    FgtSet natives, views;
};

struct FgtQueue {
    uint64_t queue;
    unsigned submits, submits2, cbs, waits, signals, fences, timeline;
    FgtSet threads;
};

struct FgtTagView {
    unsigned type, lifecycle; uint64_t native, view; uint32_t state, w, h, fmt, usage, top, left, ew, eh; bool ok;
};

struct FgTrace {
    unsigned present_calls;
    LONGLONG last_present_qpc, prev_present_qpc;
    uint64_t present_queue;
    FgtSet present_swapchains, present_threads, present_indices, present_wait_counts, present_pnext_types;
    FgtStat present_interval, present_duration;
    unsigned tag_calls, tag_frame_calls, tag_bad_reads;
    FgtSet tag_threads, tag_other_types;
    FgtTagType types[_countof(kFgtTagTypes)];
    unsigned eval_calls, eval_groups;
    uint64_t cmd_queue;
    DWORD eval_thread;
    void *last_fg_cmd;
    LONGLONG last_eval1_qpc;
    FgtSet hud, back, mv, depth, out_real, out_interp, eval_threads, eval_indices;
    FgtStat present_to_eval, eval_interval, eval_group_span;
    LONGLONG group_first_qpc;
    FgtQueue queues[8];
    unsigned nqueues, queue_overflow;
    unsigned cmdqueue_detail_left, present_queue_detail_left, eval_thread_detail_left;
    FgtSet reshade_queues;
    unsigned reshade_queue_types[kFgtDistinct];
};
struct FgtControl {
    CRITICAL_SECTION cs;
    bool cs_ready, early_tried, late_tried, queue_hooks_tried, layer_hook_tried, sleep_tried;
    Hook present_hook, tag_hook, tag_frame_hook, submit_hook, submit2_hook, sleep_hook;
    Hook get_queue_hook, get_queue2_hook, get_queue_layer_hook;
};
static FgTrace g_fgt;
static FgtControl g_fgt_control;
static void *volatile g_fgt_submit_target;
static void *volatile g_fgt_submit2_target;
static void *volatile g_fgt_queue_target;

static void FgtLock()   { EnterCriticalSection(&g_fgt_control.cs); }
static void FgtUnlock() { LeaveCriticalSection(&g_fgt_control.cs); }

static bool FgtEnabled() { return PresentAdapterTrace(); }

// The present and queue-discovery hooks serve the GPU relay too: Pipeline=3 arms
// each frame's copy from inside the interposer's vkQueuePresentKHR, and the relay
// needs the family of the queues it submits on. Everything else this file
// observes stays trace-only.
static bool FgtHooksWanted() { return PresentAdapterTrace() || PresentAdapterRelay(); }

static SRWLOCK g_fgt_log_lock = SRWLOCK_INIT;
static char g_fgt_logs[256][1536];
static unsigned g_fgt_log_read, g_fgt_log_count;
static volatile LONG g_fgt_log_dropped;

static void FgtLog(_Printf_format_string_ const char *fmt, ...)
{
    char line[1536];
    const int prefix = sprintf_s(line, "qpc=%lld ", FgtNow());
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(line + prefix, sizeof(line) - prefix, _TRUNCATE, fmt, args);
    va_end(args);
    if (!TryAcquireSRWLockExclusive(&g_fgt_log_lock)) { InterlockedIncrement(&g_fgt_log_dropped); return; }
    const bool wake = g_fgt_log_count == 0;
    if (g_fgt_log_count < _countof(g_fgt_logs)) {
        strcpy_s(g_fgt_logs[(g_fgt_log_read + g_fgt_log_count++) % _countof(g_fgt_logs)], line);
    } else InterlockedIncrement(&g_fgt_log_dropped);
    ReleaseSRWLockExclusive(&g_fgt_log_lock);
    if (wake) TriggerHookScan();
}

static void FgtFlushLog()
{
    for (unsigned i = 0; i < _countof(g_fgt_logs); ++i) {
        char line[1536];
        AcquireSRWLockExclusive(&g_fgt_log_lock);
        if (g_fgt_log_count == 0) { ReleaseSRWLockExclusive(&g_fgt_log_lock); break; }
        strcpy_s(line, g_fgt_logs[g_fgt_log_read]);
        g_fgt_log_read = (g_fgt_log_read + 1) % _countof(g_fgt_logs);
        --g_fgt_log_count;
        ReleaseSRWLockExclusive(&g_fgt_log_lock);
        Log("%s", line);
    }
    AcquireSRWLockShared(&g_fgt_log_lock);
    const bool pending = g_fgt_log_count != 0;
    ReleaseSRWLockShared(&g_fgt_log_lock);
    if (pending) InterlockedExchange(&g_scan_pending, 1);
}

static const char *FgtLifecycleName(unsigned l)
{
    return l == 0 ? "OnlyValidNow" : l == 1 ? "ValidUntilPresent" : l == 2 ? "ValidUntilEvaluate" : "?";
}

// ---------------------------------------------------------------------------
// Game-side vkQueuePresentKHR: the interposer's export, which is also what its
// vkGetDeviceProcAddr hands out (SL_INTERCEPT). The DLSS-G before-hook runs
// inside the original, so this detour sees frame N the moment the game is done.
typedef uint32_t (*PFN_FgtQueuePresent)(void *, const FgtPresentInfo *);

static void FgtNotePresent(void *queue, const FgtPresentInfo *info, LONGLONG now)
{
    FgTrace &t = g_fgt;
    FgtLock();
    const unsigned call = ++t.present_calls;
    if (t.prev_present_qpc != 0) FgtStatAdd(t.present_interval, FgtMs(t.prev_present_qpc, now));
    t.prev_present_qpc = now;
    t.present_queue = reinterpret_cast<uint64_t>(queue);
    FgtNote(t.present_threads, GetCurrentThreadId());
    uint64_t sc = 0; uint32_t idx = 0xFFFFFFFFu; uint32_t waits = 0; uint32_t pnext_type = 0;
    if (info != nullptr) {
        waits = info->waitSemaphoreCount;
        if (info->swapchainCount != 0 && info->pSwapchains != nullptr) sc = info->pSwapchains[0];
        if (info->swapchainCount != 0 && info->pImageIndices != nullptr) idx = info->pImageIndices[0];
        if (info->pNext != nullptr) pnext_type = *static_cast<const uint32_t *>(info->pNext);
        FgtNote(t.present_swapchains, sc);
        FgtNote(t.present_indices, idx);
        FgtNote(t.present_wait_counts, waits);
        FgtNote(t.present_pnext_types, pnext_type);
    }
    t.last_present_qpc = now;
    FgtUnlock();
    if (call <= 12 || call % 1800 == 0) {
        char sems[160] = {}; size_t len = 0;
        for (uint32_t i = 0; info != nullptr && info->pWaitSemaphores != nullptr && i < waits && i < 3; ++i)
            FgCensusAppend(sems, sizeof(sems), &len, "%s%llx", i ? " " : "", info->pWaitSemaphores[i]);
        FgtLog("[fg-trace] game present #%u thread=%lu queue=%p swapchains=%u sc=%llx image=%u waits=%u [%s] pNext=%u",
            call, GetCurrentThreadId(), queue, info != nullptr ? info->swapchainCount : 0u, sc, idx, waits, sems, pnext_type);
    }
}

static uint32_t FgtPresentDetour(void *queue, const FgtPresentInfo *info)
{
    if (PresentAdapterTrace()) FgtNotePresent(queue, info, FgtNow());
    // The GPU relay arms its copy here (Pipeline=3), before the original runs, so
    // the copy is in the present queue ahead of the present operation itself.
    if (PresentAdapterRelay()) FgRelayOnPresent(queue, info);
    const LONGLONG start = FgtNow();
    const uint32_t result = reinterpret_cast<PFN_FgtQueuePresent>(g_fgt_control.present_hook.original)(queue, info);
    const LONGLONG end = FgtNow();
    if (PresentAdapterTrace() && g_fgt_control.cs_ready) {
        FgtLock();
        FgtStatAdd(g_fgt.present_duration, FgtMs(start, end));
        FgtUnlock();
    }
    return result;
}

// slReflexSleep, the Reflex plugin's entry the game calls at the top of each frame,
// before it samples input and simulates. The relay's throttle waits here, after
// Reflex's own sleep, so the frame starts only once the NR it would queue behind
// has completed. The address is the plugin's own (slGetPluginFunction), which is
// what slGetFeatureFunction hands the game, so patching it covers the game however
// early it fetched the pointer.
typedef int (*PFN_FgtReflexSleep)(const void *);
static int FgtReflexSleepDetour(const void *frame)
{
    const int result = reinterpret_cast<PFN_FgtReflexSleep>(g_fgt_control.sleep_hook.original)(frame);
    if (PresentAdapterRelay()) FgRelayThrottle(kFgrAtSleep);
    return result;
}

// ---------------------------------------------------------------------------
// slSetTag / slSetTagForFrame: what the game hands Streamline, with which lifecycle.
typedef uint32_t (*PFN_FgtSetTag)(const void *, const FgtSlResourceTag *, uint32_t, void *);
typedef uint32_t (*PFN_FgtSetTagForFrame)(const void *, const void *, const FgtSlResourceTag *, uint32_t, void *);

static bool FgtReadTag(const FgtSlResourceTag *tag, FgtTagView *out)
{
    __try {
        out->type = tag->type; out->lifecycle = tag->lifecycle;
        out->top = tag->top; out->left = tag->left; out->ew = tag->width; out->eh = tag->height;
        const FgtSlResource *r = tag->resource;
        if (r != nullptr) {
            out->native = reinterpret_cast<uint64_t>(r->native);
            out->view = reinterpret_cast<uint64_t>(r->view);
            out->state = r->state; out->w = r->width; out->h = r->height; out->fmt = r->nativeFormat; out->usage = r->usage;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void FgtNoteTags(const FgtSlResourceTag *tags, uint32_t num, void *cmd, bool for_frame, const void *frame)
{
    FgTrace &t = g_fgt;
    FgtTagView views[16] = {};
    const uint32_t n = num < _countof(views) ? num : static_cast<uint32_t>(_countof(views));
    for (uint32_t i = 0; tags != nullptr && i < n; ++i) views[i].ok = FgtReadTag(tags + i, &views[i]);
    FgtLock();
    const unsigned call = for_frame ? ++t.tag_frame_calls : ++t.tag_calls;
    FgtNote(t.tag_threads, GetCurrentThreadId());
    for (uint32_t i = 0; i < n; ++i) {
        if (!views[i].ok) { ++t.tag_bad_reads; continue; }
        int slot = -1;
        for (unsigned k = 0; k < _countof(kFgtTagTypes); ++k) if (kFgtTagTypes[k] == views[i].type) slot = static_cast<int>(k);
        if (slot < 0) { FgtNote(t.tag_other_types, views[i].type); continue; }
        FgtTagType &tt = t.types[slot];
        ++tt.count;
        if (views[i].lifecycle < 3) ++tt.lifecycle[views[i].lifecycle]; else ++tt.lifecycle_other;
        if (cmd != nullptr) ++tt.cmd_nonnull;
        if (views[i].ew != 0 || views[i].eh != 0) ++tt.extent_nonzero;
        tt.state = views[i].state; tt.w = views[i].w; tt.h = views[i].h; tt.fmt = views[i].fmt; tt.usage = views[i].usage;
        FgtNote(tt.natives, views[i].native);
        FgtNote(tt.views, views[i].view);
    }
    FgtUnlock();
    if (call <= 8 || call % 1800 == 0) {
        FgtLog("[fg-trace] %s #%u thread=%lu tags=%u cmd=%p frame=%p", for_frame ? "slSetTagForFrame" : "slSetTag",
            call, GetCurrentThreadId(), num, cmd, frame);
        for (uint32_t i = 0; i < n; ++i) {
            if (!views[i].ok) { FgtLog("[fg-trace]   tag[%u] unreadable", i); continue; }
            FgtLog("[fg-trace]   tag[%u] type=%u lifecycle=%s native=%llx view=%llx state=%u %ux%u fmt=%u usage=0x%x extent=%u,%u %ux%u",
                i, views[i].type, FgtLifecycleName(views[i].lifecycle), views[i].native, views[i].view, views[i].state,
                views[i].w, views[i].h, views[i].fmt, views[i].usage, views[i].left, views[i].top, views[i].ew, views[i].eh);
        }
    }
}

static uint32_t FgtSetTagDetour(const void *viewport, const FgtSlResourceTag *tags, uint32_t num, void *cmd)
{
    FgtNoteTags(tags, num, cmd, false, nullptr);
    return reinterpret_cast<PFN_FgtSetTag>(g_fgt_control.tag_hook.original)(viewport, tags, num, cmd);
}

static uint32_t FgtSetTagForFrameDetour(const void *frame, const void *viewport, const FgtSlResourceTag *tags, uint32_t num, void *cmd)
{
    FgtNoteTags(tags, num, cmd, true, frame);
    return reinterpret_cast<PFN_FgtSetTagForFrame>(g_fgt_control.tag_frame_hook.original)(frame, viewport, tags, num, cmd);
}

// ---------------------------------------------------------------------------
// vkQueueSubmit / vkQueueSubmit2 at the top of the layer chain: the function the
// loader's vkGetDeviceProcAddr returns, which every dispatch table in the process
// -- the interposer's, Streamline's plugins', the game's -- points at.
typedef uint32_t (*PFN_FgtQueueSubmit)(void *, uint32_t, const FgtSubmitInfo *, uint64_t);
typedef uint32_t (*PFN_FgtQueueSubmit2)(void *, uint32_t, const FgtSubmitInfo2 *, uint64_t);

static FgtQueue *FgtQueueSlot(uint64_t q)
{
    FgTrace &t = g_fgt;
    for (unsigned i = 0; i < t.nqueues; ++i) if (t.queues[i].queue == q) return &t.queues[i];
    if (t.nqueues < _countof(t.queues)) { t.queues[t.nqueues].queue = q; return &t.queues[t.nqueues++]; }
    ++t.queue_overflow;
    return nullptr;
}

struct FgtSubmitView {
    uint32_t waits, cbs, signals; bool timeline, has_fg_cmd;
    uint64_t wait_sem[3], signal_sem[3]; uint64_t wait_val[3], signal_val[3]; void *cb[3];
};

static void FgtViewSubmit(const FgtSubmitInfo *s, void *fg_cmd, FgtSubmitView *v)
{
    v->waits = s->waitSemaphoreCount; v->cbs = s->commandBufferCount; v->signals = s->signalSemaphoreCount;
    for (uint32_t i = 0; s->pWaitSemaphores != nullptr && i < v->waits && i < 3; ++i) v->wait_sem[i] = s->pWaitSemaphores[i];
    for (uint32_t i = 0; s->pSignalSemaphores != nullptr && i < v->signals && i < 3; ++i) v->signal_sem[i] = s->pSignalSemaphores[i];
    for (uint32_t i = 0; s->pCommandBuffers != nullptr && i < v->cbs; ++i) {
        if (i < 3) v->cb[i] = s->pCommandBuffers[i];
        if (s->pCommandBuffers[i] == fg_cmd && fg_cmd != nullptr) v->has_fg_cmd = true;
    }
    for (const uint32_t *ext = static_cast<const uint32_t *>(s->pNext); ext != nullptr;
         ext = static_cast<const uint32_t *>(*reinterpret_cast<const void *const *>(ext + 2))) {
        if (*ext != kFgtStTimelineSubmitInfo) continue;
        const FgtTimelineSubmitInfo *tl = reinterpret_cast<const FgtTimelineSubmitInfo *>(ext);
        v->timeline = true;
        for (uint32_t i = 0; tl->pWaitValues != nullptr && i < tl->waitValueCount && i < 3; ++i) v->wait_val[i] = tl->pWaitValues[i];
        for (uint32_t i = 0; tl->pSignalValues != nullptr && i < tl->signalValueCount && i < 3; ++i) v->signal_val[i] = tl->pSignalValues[i];
        break;
    }
}

static void FgtViewSubmit2(const FgtSubmitInfo2 *s, void *fg_cmd, FgtSubmitView *v)
{
    v->waits = s->waitSemaphoreInfoCount; v->cbs = s->commandBufferInfoCount; v->signals = s->signalSemaphoreInfoCount;
    v->timeline = false;
    for (uint32_t i = 0; s->pWaitSemaphoreInfos != nullptr && i < v->waits && i < 3; ++i) {
        v->wait_sem[i] = s->pWaitSemaphoreInfos[i].semaphore; v->wait_val[i] = s->pWaitSemaphoreInfos[i].value;
    }
    for (uint32_t i = 0; s->pSignalSemaphoreInfos != nullptr && i < v->signals && i < 3; ++i) {
        v->signal_sem[i] = s->pSignalSemaphoreInfos[i].semaphore; v->signal_val[i] = s->pSignalSemaphoreInfos[i].value;
    }
    for (uint32_t i = 0; s->pCommandBufferInfos != nullptr && i < v->cbs; ++i) {
        if (i < 3) v->cb[i] = s->pCommandBufferInfos[i].commandBuffer;
        if (s->pCommandBufferInfos[i].commandBuffer == fg_cmd && fg_cmd != nullptr) v->has_fg_cmd = true;
    }
}

static void FgtNoteSubmit(void *queue, uint32_t count, const FgtSubmitInfo *subs, const FgtSubmitInfo2 *subs2, uint64_t fence, bool is_submit2)
{
    FgTrace &t = g_fgt;
    const uint64_t q = reinterpret_cast<uint64_t>(queue);
    const LONGLONG now = FgtNow();
    const DWORD tid = GetCurrentThreadId();
    FgtSubmitView views[4] = {};
    const uint32_t n = count < _countof(views) ? count : static_cast<uint32_t>(_countof(views));
    FgtLock();
    void *const fg_cmd = t.last_fg_cmd;
    FgtUnlock();
    for (uint32_t i = 0; i < n; ++i) {
        if (subs != nullptr) FgtViewSubmit(subs + i, fg_cmd, &views[i]);
        else if (subs2 != nullptr) FgtViewSubmit2(subs2 + i, fg_cmd, &views[i]);
    }
    bool detail = false;
    const char *why = "";
    FgtLock();
    FgtQueue *slot = FgtQueueSlot(q);
    if (slot != nullptr) {
        ++slot->submits;
        if (is_submit2) ++slot->submits2;
        if (fence != 0) ++slot->fences;
        FgtNote(slot->threads, tid);
        for (uint32_t i = 0; i < n; ++i) {
            slot->cbs += views[i].cbs; slot->waits += views[i].waits; slot->signals += views[i].signals;
            if (views[i].timeline) ++slot->timeline;
        }
    }
    if (q == t.cmd_queue && t.cmd_queue != 0 && t.cmdqueue_detail_left != 0) { --t.cmdqueue_detail_left; detail = true; why = "FG queue"; }
    else if (q == t.present_queue && t.present_queue != 0 && t.present_queue_detail_left != 0) { --t.present_queue_detail_left; detail = true; why = "game present queue"; }
    else if (tid == t.eval_thread && t.eval_thread != 0 && t.eval_thread_detail_left != 0) { --t.eval_thread_detail_left; detail = true; why = "evaluate thread"; }
    const LONGLONG eval1 = t.last_eval1_qpc;
    const LONGLONG present = t.last_present_qpc;
    FgtUnlock();
    if (!detail) return;
    FgtLog("[fg-trace] %s (%s) queue=%p thread=%lu batches=%u fence=%llx since_present=%.2fms since_eval1=%.2fms",
        is_submit2 ? "vkQueueSubmit2" : "vkQueueSubmit", why, queue, tid, count, fence,
        present != 0 ? FgtMs(present, now) : -1.0, eval1 != 0 ? FgtMs(eval1, now) : -1.0);
    for (uint32_t i = 0; i < n; ++i) {
        const FgtSubmitView &v = views[i];
        FgtLog("[fg-trace]   batch[%u] waits=%u [%llx:%llu %llx:%llu %llx:%llu] cbs=%u [%p %p %p]%s signals=%u [%llx:%llu %llx:%llu %llx:%llu]%s",
            i, v.waits, v.wait_sem[0], v.wait_val[0], v.wait_sem[1], v.wait_val[1], v.wait_sem[2], v.wait_val[2],
            v.cbs, v.cb[0], v.cb[1], v.cb[2], v.has_fg_cmd ? " <FG evaluate cmd>" : "",
            v.signals, v.signal_sem[0], v.signal_val[0], v.signal_sem[1], v.signal_val[1], v.signal_sem[2], v.signal_val[2],
            v.timeline ? " timeline" : "");
    }
}

static uint32_t FgtQueueSubmitDetour(void *queue, uint32_t count, const FgtSubmitInfo *subs, uint64_t fence)
{
    FgtNoteSubmit(queue, count, subs, nullptr, fence, false);
    return reinterpret_cast<PFN_FgtQueueSubmit>(g_fgt_control.submit_hook.original)(queue, count, subs, fence);
}

static uint32_t FgtQueueSubmit2Detour(void *queue, uint32_t count, const FgtSubmitInfo2 *subs, uint64_t fence)
{
    FgtNoteSubmit(queue, count, nullptr, subs, fence, true);
    return reinterpret_cast<PFN_FgtQueueSubmit2>(g_fgt_control.submit2_hook.original)(queue, count, subs, fence);
}

// ---------------------------------------------------------------------------
// ReShade's queues: which VkQueue it presents on and which types it knows.
static void OnFgtInitCommandQueue(reshade::api::command_queue *queue)
{
    if (queue == nullptr || queue->get_device()->get_api() != reshade::api::device_api::vulkan) return;
    FgtLock();
    const int slot = FgtNote(g_fgt.reshade_queues, queue->get_native());
    if (slot >= 0 && slot < static_cast<int>(_countof(g_fgt.reshade_queue_types)))
        g_fgt.reshade_queue_types[slot] = static_cast<unsigned>(queue->get_type());
    FgtUnlock();
    FgtLog("[fg-trace] ReShade init_command_queue native=%llx type=0x%x thread=%lu",
        queue->get_native(), static_cast<unsigned>(queue->get_type()), GetCurrentThreadId());
}

// Asynchronous installation may miss queue retrievals; an unknown family must remain unknown.
typedef void (*PFN_FgtGetDeviceQueue)(void *, uint32_t, uint32_t, void **);
typedef void (*PFN_FgtGetDeviceQueue2)(void *, const void *, void **);
struct FgtQueueId { uint64_t queue; uint32_t family, index; };
static FgtQueueId g_fgt_queue_ids[16];
static unsigned g_fgt_queue_id_count;

static void FgtNoteQueueId(void *queue, uint32_t family, uint32_t index, const char *via)
{
    FgtLock();
    for (unsigned i = 0; i < g_fgt_queue_id_count; ++i) {
        const FgtQueueId &q = g_fgt_queue_ids[i];
        if (q.queue == reinterpret_cast<uint64_t>(queue) && q.family == family && q.index == index) { FgtUnlock(); return; }
    }
    const bool stored = g_fgt_queue_id_count < _countof(g_fgt_queue_ids);
    if (stored) {
        FgtQueueId &q = g_fgt_queue_ids[g_fgt_queue_id_count++];
        q.queue = reinterpret_cast<uint64_t>(queue); q.family = family; q.index = index;
    }
    FgtUnlock();
    if (stored) FgtLog("[fg-trace] %s family=%u index=%u -> queue=%p thread=%lu", via, family, index, queue, GetCurrentThreadId());
}

static void FgtGetDeviceQueueDetour(void *dev, uint32_t family, uint32_t index, void **queue)
{
    reinterpret_cast<PFN_FgtGetDeviceQueue>(g_fgt_control.get_queue_hook.original)(dev, family, index, queue);
    void *q = queue != nullptr ? *queue : nullptr;
    FgRelayNoteQueue(q, family);
    FgtNoteQueueId(q, family, index, "vkGetDeviceQueue");
}

static void FgtGetDeviceQueue2Detour(void *dev, const void *info, void **queue)
{
    reinterpret_cast<PFN_FgtGetDeviceQueue2>(g_fgt_control.get_queue2_hook.original)(dev, info, queue);
    const uint32_t *u = static_cast<const uint32_t *>(info);
    void *q = queue != nullptr ? *queue : nullptr;
    FgRelayNoteQueue(q, u != nullptr ? u[5] : 0xFFFFFFFFu);
    FgtNoteQueueId(q, u != nullptr ? u[5] : 0xFFFFFFFFu, u != nullptr ? u[6] : 0xFFFFFFFFu, "vkGetDeviceQueue2");
}

static void FgtGetDeviceQueueLayerDetour(void *dev, uint32_t family, uint32_t index, void **queue)
{
    reinterpret_cast<PFN_FgtGetDeviceQueue>(g_fgt_control.get_queue_layer_hook.original)(dev, family, index, queue);
    void *q = queue != nullptr ? *queue : nullptr;
    FgRelayNoteQueue(q, family);
    FgtNoteQueueId(q, family, index, "vkGetDeviceQueue(layer)");
}

static void FgtPublishTarget(void *volatile *slot, void *target)
{
    if (target == nullptr || !PinHookModule(target)) return;
    void *const previous = InterlockedCompareExchangePointer(slot, target, nullptr);
    if (previous != nullptr && previous != target)
        FgtLog("[fg-trace] additional dispatch target %p not observed (retaining %p)", target, previous);
    if (previous == nullptr) TriggerHookScan();
}

static void OnFgtInitDevice(reshade::api::device *dev)
{
    if (dev == nullptr || dev->get_api() != reshade::api::device_api::vulkan) return;
    auto gdpa = g_reshade_dll != nullptr
        ? reinterpret_cast<PFN_SVkGetDeviceProcAddr>(GetProcAddress(g_reshade_dll, "vkGetDeviceProcAddr")) : nullptr;
    if (gdpa != nullptr)
        FgtPublishTarget(&g_fgt_queue_target, gdpa(reinterpret_cast<SVkDevice>(static_cast<uintptr_t>(dev->get_native())), "vkGetDeviceQueue"));
}

static void FgtOnRuntime(reshade::api::effect_runtime *rt)
{
    if (!FgtHooksWanted() || rt == nullptr) return;
    auto *dev = rt->get_device();
    if (dev == nullptr || dev->get_api() != reshade::api::device_api::vulkan) return;
    // The queue-discovery target is what gives the relay the family of the queues
    // it submits on, so it is published whether or not the trace is on.
    OnFgtInitDevice(dev);
    if (!FgtEnabled()) return;
    HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
    auto gdpa = loader != nullptr
        ? reinterpret_cast<PFN_SVkGetDeviceProcAddr>(GetProcAddress(loader, "vkGetDeviceProcAddr")) : nullptr;
    if (gdpa != nullptr) {
        auto native = reinterpret_cast<SVkDevice>(static_cast<uintptr_t>(dev->get_native()));
        void *submit2 = gdpa(native, "vkQueueSubmit2");
        if (submit2 == nullptr) submit2 = gdpa(native, "vkQueueSubmit2KHR");
        FgtPublishTarget(&g_fgt_submit2_target, submit2);
        FgtPublishTarget(&g_fgt_submit_target, gdpa(native, "vkQueueSubmit"));
    }
    auto *rq = rt->get_command_queue();
    const uint32_t n = rt->get_back_buffer_count();
    char list[512] = {}; size_t len = 0;
    for (uint32_t i = 0; i < n && i < 16; ++i)
        FgCensusAppend(list, sizeof(list), &len, "%s%llx", i ? " " : "", rt->get_back_buffer(i).handle);
    static volatile LONG generation;
    FgtLog("[fg-trace] runtime generation=%ld device=%llx present queue=%llx type=0x%x back buffers=%u [%s]",
        InterlockedIncrement(&generation), dev->get_native(), rq != nullptr ? rq->get_native() : 0ull,
        rq != nullptr ? static_cast<unsigned>(rq->get_type()) : 0u, n, list);
}

static void FgtInit()
{
    FgRelayInit();
    if (g_fgt_control.cs_ready) return;
    InitializeCriticalSection(&g_fgt_control.cs);
    g_fgt_control.cs_ready = true;
    g_fgt.cmdqueue_detail_left = 24;
    g_fgt.present_queue_detail_left = 8;
    g_fgt.eval_thread_detail_left = 12;
}

static void FgtRegisterEvents(PFN_ReShadeRegisterEvent reg)
{
    if (!FgtHooksWanted() || reg == nullptr) return;
    FgtInit();
    if (!FgtEnabled()) return;   // relay-only: nothing to register, the hooks come from the worker
    reg(reshade::addon_event::init_command_queue, reinterpret_cast<void *>(&OnFgtInitCommandQueue));
    FgtLog("[fg-trace] Trace=1: asynchronous log; hook installation deferred to the NGX hook worker. Early calls may be missed.");
}

// Only HookWorkerProc calls this; rendering callbacks publish pinned code addresses, never device objects.
static void FgtMaintenance()
{
    if (!FgtHooksWanted() || !g_fgt_control.cs_ready) return;
    const bool trace = FgtEnabled();
    FgtControl &t = g_fgt_control;
    if (TryEnterCriticalSection(&g_hook_cs)) {
        HMODULE sl = nullptr;
        if ((!t.early_tried || !t.queue_hooks_tried) &&
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"sl.interposer.dll", &sl)) {
            if (!t.early_tried) {
                t.early_tried = true;
                const bool a = HookInstall(t.present_hook, reinterpret_cast<void *>(GetProcAddress(sl, "vkQueuePresentKHR")), reinterpret_cast<void *>(&FgtPresentDetour));
                bool b = false, c = false;
                if (trace) {
                    b = HookInstall(t.tag_hook, reinterpret_cast<void *>(GetProcAddress(sl, "slSetTag")), reinterpret_cast<void *>(&FgtSetTagDetour));
                    c = HookInstall(t.tag_frame_hook, reinterpret_cast<void *>(GetProcAddress(sl, "slSetTagForFrame")), reinterpret_cast<void *>(&FgtSetTagForFrameDetour));
                }
                FgtLog("%s: present=%s%s", trace ? "[fg-trace] interposer observers" : "[fg-relay] interposer hook",
                    a ? "hooked" : "FAILED", trace ? (b && c ? " tag=hooked tag_frame=hooked" : " tag/tag_frame FAILED") : "");
            }
            if (!t.queue_hooks_tried) {
                t.queue_hooks_tried = true;
                const bool a = HookInstall(t.get_queue_hook, reinterpret_cast<void *>(GetProcAddress(sl, "vkGetDeviceQueue")), reinterpret_cast<void *>(&FgtGetDeviceQueueDetour));
                const bool b = HookInstall(t.get_queue2_hook, reinterpret_cast<void *>(GetProcAddress(sl, "vkGetDeviceQueue2")), reinterpret_cast<void *>(&FgtGetDeviceQueue2Detour));
                FgtLog("[fg-trace] queue discovery exports: get_queue=%s get_queue2=%s", a ? "hooked" : "FAILED", b ? "hooked" : "FAILED");
            }
        }
        HMODULE reflex = nullptr;
        if (!t.sleep_tried && PresentAdapterRelayThrottle() > 0 &&
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"sl.reflex.dll", &reflex)) {
            t.sleep_tried = true;
            typedef void *(*PFN_FgtSlGetPluginFunction)(const char *);
            auto get = reinterpret_cast<PFN_FgtSlGetPluginFunction>(GetProcAddress(reflex, "slGetPluginFunction"));
            void *sleep = get != nullptr ? get("slReflexSleep") : nullptr;
            const bool ok = sleep != nullptr &&
                HookInstall(t.sleep_hook, sleep, reinterpret_cast<void *>(&FgtReflexSleepDetour));
            FgtLog("[fg-relay] Reflex sleep hook: slGetPluginFunction=%p slReflexSleep=%p %s",
                reinterpret_cast<void *>(get), sleep, ok ? "hooked" : "FAILED (the present-side throttle is the only one)");
        }
        void *queue = InterlockedCompareExchangePointer(&g_fgt_queue_target, nullptr, nullptr);
        if (queue != nullptr && !t.layer_hook_tried) {
            t.layer_hook_tried = true;
            const bool ok = queue != t.get_queue_hook.target &&
                HookInstall(t.get_queue_layer_hook, queue, reinterpret_cast<void *>(&FgtGetDeviceQueueLayerDetour));
            FgtLog("[fg-trace] layer queue discovery: %p %s", queue, ok ? "hooked" : "unavailable/duplicate");
        }
        void *submit = InterlockedCompareExchangePointer(&g_fgt_submit_target, nullptr, nullptr);
        if (trace && submit != nullptr && !t.late_tried) {
            t.late_tried = true;
            void *submit2 = InterlockedCompareExchangePointer(&g_fgt_submit2_target, nullptr, nullptr);
            const bool a = HookInstall(t.submit_hook, submit, reinterpret_cast<void *>(&FgtQueueSubmitDetour));
            const bool b = submit2 != nullptr && submit2 != submit && HookInstall(t.submit2_hook, submit2, reinterpret_cast<void *>(&FgtQueueSubmit2Detour));
            FgtLog("[fg-trace] queue observers: submit=%p %s submit2=%p %s", submit, a ? "hooked" : "FAILED", submit2, b ? "hooked" : "unavailable");
        }
        // The relay's own device-creation hook (Import=1 only; it logs its
        // verdict). Here rather than at the top because it is the same kind of
        // decision as the observers above -- a patch on a process-wide entry
        // point -- and belongs behind the same lock.
        FgRelayInstallDeviceHook();
        LeaveCriticalSection(&g_hook_cs);
    } else InterlockedExchange(&g_scan_pending, 1);
    FgtFlushLog();
}

static void FgtSummary()
{
    FgTrace t;
    FgtQueueId queue_ids[16];
    unsigned queue_id_count;
    FgtLock();
    t = g_fgt;
    queue_id_count = g_fgt_queue_id_count;
    memcpy(queue_ids, g_fgt_queue_ids, sizeof(queue_ids));
    FgtUnlock();
    char a[512], b[256];
    FgtLog("[fg-trace] ---- summary at %u FG groups / %u evaluates / %u game presents; log_dropped=%ld ----",
        t.eval_groups, t.eval_calls, t.present_calls, InterlockedCompareExchange(&g_fgt_log_dropped, 0, 0));
    FgtStatText(t.present_duration, a, sizeof(a));
    FgtLog("[fg-trace] game present CPU call duration %s (not input-to-display latency)", a);
    FgtStatText(t.present_interval, b, sizeof(b));
    FgtSetText(t.present_threads, a, sizeof(a));
    FgtLog("[fg-trace] game present: queue=%llx interval %s threads %s", t.present_queue, b, a);
    FgtSetText(t.present_swapchains, a, sizeof(a)); FgtLog("[fg-trace] game present swapchains %s", a);
    FgtSetText(t.present_indices, a, sizeof(a));    FgtLog("[fg-trace] game present image indices %s", a);
    FgtSetText(t.present_wait_counts, a, sizeof(a)); FgtSetText(t.present_pnext_types, b, sizeof(b));
    FgtLog("[fg-trace] game present wait counts %s pNext sTypes %s", a, b);
    FgtStatText(t.present_to_eval, a, sizeof(a)); FgtStatText(t.eval_interval, b, sizeof(b));
    FgtLog("[fg-trace] evaluate idx1: thread=%lu cmd_queue=%llx latest-present age (unpaired) %s | evaluate interval %s", t.eval_thread, t.cmd_queue, a, b);
    FgtStatText(t.eval_group_span, a, sizeof(a)); FgtSetText(t.eval_indices, b, sizeof(b));
    FgtLog("[fg-trace] evaluate group span (idx1->last) %s indices %s", a, b);
    FgtSetText(t.eval_threads, a, sizeof(a)); FgtLog("[fg-trace] evaluate threads %s", a);
    FgtSetText(t.hud, a, sizeof(a));        FgtLog("[fg-trace] handles HUDLess    %s", a);
    FgtSetText(t.back, a, sizeof(a));       FgtLog("[fg-trace] handles Backbuffer %s", a);
    FgtSetText(t.depth, a, sizeof(a));      FgtLog("[fg-trace] handles Depth      %s", a);
    FgtSetText(t.mv, a, sizeof(a));         FgtLog("[fg-trace] handles MVecs      %s", a);
    FgtSetText(t.out_real, a, sizeof(a));   FgtLog("[fg-trace] handles OutputReal %s", a);
    FgtSetText(t.out_interp, a, sizeof(a)); FgtLog("[fg-trace] handles OutputInterpolated %s", a);
    FgtSetText(t.tag_threads, a, sizeof(a));
    FgtLog("[fg-trace] tags: slSetTag=%u slSetTagForFrame=%u unreadable=%u threads %s", t.tag_calls, t.tag_frame_calls, t.tag_bad_reads, a);
    for (unsigned k = 0; k < _countof(kFgtTagTypes); ++k) {
        const FgtTagType &tt = t.types[k];
        if (tt.count == 0) continue;
        FgtSetText(tt.natives, a, sizeof(a)); FgtSetText(tt.views, b, sizeof(b));
        FgtLog("[fg-trace] tag %-12s count=%u lifecycle now/present/evaluate/other=%u/%u/%u/%u cmd_nonnull=%u extent_nonzero=%u last state=%u %ux%u fmt=%u usage=0x%x natives %s views %s",
            kFgtTagNames[k], tt.count, tt.lifecycle[0], tt.lifecycle[1], tt.lifecycle[2], tt.lifecycle_other, tt.cmd_nonnull, tt.extent_nonzero,
            tt.state, tt.w, tt.h, tt.fmt, tt.usage, a, b);
    }
    if (t.tag_other_types.n != 0) { FgtSetText(t.tag_other_types, a, sizeof(a)); FgtLog("[fg-trace] tag other types %s", a); }
    for (unsigned i = 0; i < t.nqueues; ++i) {
        const FgtQueue &q = t.queues[i];
        FgtSetText(q.threads, a, sizeof(a));
        const char *label = q.queue == t.cmd_queue && t.cmd_queue != 0 ? " =DLSSG.CmdQueue" : "";
        const char *label2 = q.queue == t.present_queue && t.present_queue != 0 ? " =game present queue" : "";
        int rs = -1;
        for (unsigned r = 0; r < t.reshade_queues.n; ++r) if (t.reshade_queues.v[r] == q.queue) rs = static_cast<int>(r);
        char fam[48] = "family=? index=?";
        for (unsigned k = 0; k < queue_id_count; ++k)
            if (queue_ids[k].queue == q.queue) { _snprintf_s(fam, sizeof(fam), _TRUNCATE, "family=%u index=%u", queue_ids[k].family, queue_ids[k].index); break; }
        FgtLog("[fg-trace] queue %llx%s%s %s reshade_type=%s0x%x submits=%u (submit2 %u, timeline %u, fenced %u) cbs=%u waits=%u signals=%u threads %s",
            q.queue, label, label2, fam, rs >= 0 ? "" : "unknown ", rs >= 0 && rs < static_cast<int>(kFgtDistinct) ? t.reshade_queue_types[rs] : 0u,
            q.submits, q.submits2, q.timeline, q.fences, q.cbs, q.waits, q.signals, a);
    }
    if (t.queue_overflow != 0) FgtLog("[fg-trace] %u submits to queues beyond the table", t.queue_overflow);
}

// Every FG evaluate, on Streamline's thread, before the input feed looks at it.
static void FgTraceEvaluate(void *cmd, const NVSDK_NGX_Parameter *p)
{
    if (!FgtEnabled() || !g_fgt_control.cs_ready) return;
    FgTrace &t = g_fgt;
    const LONGLONG now = FgtNow();
    unsigned idx = 0;
    const bool indexed = FgInputReadU(p, "DLSSG.MultiFrameIndex", &idx);
    void *queue = nullptr;
    p->Get("DLSSG.CmdQueue", &queue);
    const char *why = nullptr;
    VkmRes hud = {}, back = {}, mv = {}, depth = {}, oreal = {}, ointerp = {};
    const bool have_hud = VkmGetRes(p, "DLSSG.HUDLess", &hud, &why);
    const bool have_back = VkmGetRes(p, "DLSSG.Backbuffer", &back, &why);
    const bool have_mv = VkmGetRes(p, "DLSSG.MVecs", &mv, &why);
    const bool have_depth = VkmGetRes(p, "DLSSG.Depth", &depth, &why);
    const bool have_real = VkmGetRes(p, "DLSSG.OutputReal", &oreal, &why);
    const bool have_interp = VkmGetRes(p, "DLSSG.OutputInterpolated", &ointerp, &why);
    const bool first = indexed ? idx == 1 : have_real;
    unsigned groups = 0, calls = 0;
    double since_present = -1.0;
    FgtLock();
    const LONGLONG present = t.last_present_qpc;
    calls = ++t.eval_calls;
    t.cmd_queue = reinterpret_cast<uint64_t>(queue);
    t.eval_thread = GetCurrentThreadId();
    FgtNote(t.eval_threads, GetCurrentThreadId());
    FgtNote(t.eval_indices, idx);
    if (first) {
        ++t.eval_groups;
        t.last_fg_cmd = cmd;
        if (present != 0 && now >= present) { since_present = FgtMs(present, now); FgtStatAdd(t.present_to_eval, since_present); }
        if (t.last_eval1_qpc != 0) FgtStatAdd(t.eval_interval, FgtMs(t.last_eval1_qpc, now));
        t.last_eval1_qpc = now;
        t.group_first_qpc = now;
        if (have_hud) FgtNote(t.hud, hud.image);
        if (have_back) FgtNote(t.back, back.image);
        if (have_mv) FgtNote(t.mv, mv.image);
        if (have_depth) FgtNote(t.depth, depth.image);
        if (have_real) FgtNote(t.out_real, oreal.image);
    } else if (t.group_first_qpc != 0) {
        FgtStatAdd(t.eval_group_span, FgtMs(t.group_first_qpc, now));
    }
    if (have_interp) FgtNote(t.out_interp, ointerp.image);
    groups = t.eval_groups;
    FgtUnlock();
    if (groups <= 3 || (first && groups % 1800 == 0)) {
        FgtLog("[fg-trace] evaluate #%u idx=%u%s thread=%lu cmd=%p queue=%p since_present=%.2fms hud=%llx back=%llx depth=%llx mv=%llx real=%llx interp=%llx",
            calls, idx, indexed ? "" : "(unindexed)", GetCurrentThreadId(), cmd, queue,
            first ? since_present : (present != 0 ? FgtMs(present, now) : -1.0),
            have_hud ? hud.image : 0ull, have_back ? back.image : 0ull, have_depth ? depth.image : 0ull,
            have_mv ? mv.image : 0ull, have_real ? oreal.image : 0ull, have_interp ? ointerp.image : 0ull);
    }
    if (first && (groups == 60 || groups % 600 == 0)) FgtSummary();
}
