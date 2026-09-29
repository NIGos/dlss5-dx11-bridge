// The GPU relay for the frame-generation input feed.
//
// Pipeline=2 and Pipeline=3 move the feed's synchronisation off the CPU and start
// the real frame's NR earlier:
//
//   * Pipeline=2 keeps the copy at the FG evaluate but submits it as this
//     add-on's own Vulkan batch on DLSSG.CmdQueue instead of recording it into
//     Streamline's command buffer, and the park in that command buffer is
//     released by the GPU.
//   * Pipeline=3 does the copy at the game's own vkQueuePresentKHR, on the
//     game's present queue -- the moment the frame's render is done and, by the
//     trace2 session of 2026-09-29, 5.3 ms on average before the FG evaluate for
//     that frame. NR starts while Streamline is still finishing the previous
//     group instead of after the evaluate has arrived.
//
// The chain, in both modes:
//
//   copies (Vulkan, this add-on's batch) -> [copy done]
//   -> the private D3D12 queue waits the copy on the GPU through an imported
//      timeline semaphore (a bounded host wait instead when it is unavailable)
//   -> NR + composite on the private D3D12 session, signalled on a
//      D3D12_FENCE_FLAG_SHARED fence
//   -> Vulkan waits that fence as an imported timeline semaphore in a one-command
//      batch whose command is vkCmdSetEvent on the park's event
//   -> Streamline's parked command buffer resumes, records the copy back, and
//      frame generation consumes the processed frame.
//
// VK_KHR_external_semaphore_win32 is what makes the last two hops GPU-side, and
// ReShade does not enable it on the devices it creates (the note above
// SynthVkOpen says why). The import is an explicit opt-in ([Adapter] Import=1),
// and with it this file adds the extension where it can still be added: in the
// create info of the game's own device, through a hook on the ReShade layer's
// vkCreateDevice ("Device extension injection" below). A device the extension
// was not accepted on falls back to the host wait and the host release while
// everything else -- the present-start copy, the removed ev_in polling, the
// pre-queued D3D12 chain -- is kept. Both verdicts are logged once.
//
// Nothing here runs unless Pipeline is 2 or 3. Every frame the relay does not arm
// falls through to the serial feed unchanged, which is also how the session gets
// built: the relay only opens once the serial feed has made the private feature,
// the shared textures and the imported images.

// ---------------------------------------------------------------------------
// The Vulkan entry points the relay adds. Declared here rather than in synth.inc
// so the transports that never use them do not carry them.
// ---------------------------------------------------------------------------

typedef struct SVkQueue_T *SVkQueue;

static const uint32_t kFgrStSemaphoreCreate    = 9;
static const uint32_t kFgrStFenceCreate        = 8;
static const uint32_t kFgrStCommandPoolCreate  = 39;
static const uint32_t kFgrStCommandBufferAlloc = 40;
static const uint32_t kFgrStCommandBufferBegin = 42;
static const uint32_t kFgrStSubmitInfo         = 4;
static const uint32_t kFgrStExportSemaphore    = 1000077000;
static const uint32_t kFgrStImportSemWin32     = 1000078000;
static const uint32_t kFgrStGetSemWin32        = 1000078003;  // VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR
static const uint32_t kFgrStSemaphoreType      = 1000207002;
static const uint32_t kFgrStTimelineSubmit     = 1000207003;
static const uint32_t kFgrSemTypeTimeline      = 1;
static const uint32_t kFgrHandleTypeD3D12Fence = 0x8;      // VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT
static const uint32_t kFgrPoolResetCommandBuffer = 0x2;    // VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
static const uint32_t kFgrCbOneTimeSubmit      = 0x1;      // VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
static const uint32_t kFgrFenceSignaledFlag    = 0x1;      // VK_FENCE_CREATE_SIGNALED_BIT
static const uint32_t kFgrPrimary              = 0;        // VK_COMMAND_BUFFER_LEVEL_PRIMARY

struct FgrSemaphoreTypeCreateInfo   { uint32_t sType; const void *pNext; uint32_t semaphoreType; uint64_t initialValue; };
struct FgrSemaphoreCreateInfo       { uint32_t sType; const void *pNext; uint32_t flags; };
struct FgrExportSemaphoreCreateInfo { uint32_t sType; const void *pNext; uint32_t handleTypes; };
struct FgrImportSemaphoreWin32      { uint32_t sType; const void *pNext; SVkHandle semaphore;
                                      uint32_t flags; uint32_t handleType; HANDLE handle; LPCWSTR name; };
struct FgrGetSemaphoreWin32         { uint32_t sType; const void *pNext; SVkHandle semaphore; uint32_t handleType; };
struct FgrSubmitInfo                { uint32_t sType; const void *pNext;
                                      uint32_t waitCount; const SVkHandle *pWaits; const uint32_t *pWaitStages;
                                      uint32_t cbCount; const SVkCommandBuffer *pCbs;
                                      uint32_t signalCount; const SVkHandle *pSignals; };
struct FgrTimelineSubmitInfo        { uint32_t sType; const void *pNext;
                                      uint32_t waitValueCount; const uint64_t *pWaitValues;
                                      uint32_t signalValueCount; const uint64_t *pSignalValues; };
struct FgrFenceCreateInfo           { uint32_t sType; const void *pNext; uint32_t flags; };
struct FgrCommandPoolCreateInfo     { uint32_t sType; const void *pNext; uint32_t flags; uint32_t queueFamilyIndex; };
struct FgrCommandBufferAllocInfo    { uint32_t sType; const void *pNext; SVkHandle pool; uint32_t level; uint32_t count; };
struct FgrCommandBufferBeginInfo    { uint32_t sType; const void *pNext; uint32_t flags; const void *pInheritance; };

typedef uint32_t (*PFN_FgrCreateSemaphore)(SVkDevice, const FgrSemaphoreCreateInfo *, const void *, SVkHandle *);
typedef uint32_t (*PFN_FgrImportSemaphoreWin32)(SVkDevice, const FgrImportSemaphoreWin32 *);
typedef uint32_t (*PFN_FgrGetSemaphoreWin32)(SVkDevice, const FgrGetSemaphoreWin32 *, HANDLE *);
typedef uint32_t (*PFN_FgrCreateFence)(SVkDevice, const FgrFenceCreateInfo *, const void *, SVkHandle *);
typedef uint32_t (*PFN_FgrResetFences)(SVkDevice, uint32_t, const SVkHandle *);
typedef uint32_t (*PFN_FgrWaitForFences)(SVkDevice, uint32_t, const SVkHandle *, uint32_t, uint64_t);
typedef uint32_t (*PFN_FgrGetFenceStatus)(SVkDevice, SVkHandle);
typedef uint32_t (*PFN_FgrCreateCommandPool)(SVkDevice, const FgrCommandPoolCreateInfo *, const void *, SVkHandle *);
typedef uint32_t (*PFN_FgrAllocateCommandBuffers)(SVkDevice, const FgrCommandBufferAllocInfo *, SVkCommandBuffer *);
typedef uint32_t (*PFN_FgrBeginCommandBuffer)(SVkCommandBuffer, const FgrCommandBufferBeginInfo *);
typedef uint32_t (*PFN_FgrEndCommandBuffer)(SVkCommandBuffer);
typedef uint32_t (*PFN_FgrResetCommandBuffer)(SVkCommandBuffer, uint32_t);
typedef uint32_t (*PFN_FgrQueueSubmit)(SVkQueue, uint32_t, const FgrSubmitInfo *, SVkHandle);

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

// The park ring. Four rather than two because both things that gate a slot's
// reuse need the frame in it to be provably over: the batch that waited on the
// slot's event must have executed it (proved by ev_mark, which that batch sets
// after the wait), and the D3D12 signal that released it must have fired (proved
// by the shared fence's completed value, which the host can read). The copy and
// release command buffers and their fences are per slot for the same reason: a
// frame's release is still waiting on its NR when the next frame is armed.
static const unsigned kFgrSlots = 4;

struct FgrArm
{
    bool     valid;
    uint64_t hud, back, depth, mv;
    uint64_t copy_val, nr_val;
    uint32_t w, h, vk_fmt;
    unsigned slot;
    LONG64   tick;
};

struct FgrPool
{
    void            *queue;
    SVkHandle        pool;
    SVkCommandBuffer cb_copy[kFgrSlots], cb_release[kFgrSlots];
};

struct FgrState
{
    CRITICAL_SECTION cs;
    bool     cs_ready, tried, ready, import_ok, fell_back;
    const char *why;

    // True while a batch submitted from here is the only proof its fence is
    // still meaningful. A failed submit leaves the fence unsignalled with
    // nothing in flight; without these the next frame reads that as "not done"
    // and the relay deadlocks instead of retrying.
    bool     copy_in_flight[kFgrSlots], release_in_flight[kFgrSlots];

    // The last evaluate's inputs. The guides are single-buffered in this game
    // (trace2, 2026-09-29: one handle each over 25000+ groups), which is what
    // makes a present-time copy of them the same frame's content. The back
    // buffer answers to three handles, so it is looked up by present image index
    // through the map below -- learned at the evaluate, where both the index and
    // the handle are known.
    bool     have_last;
    uint64_t last_hud, last_depth, last_mv;
    uint32_t last_w, last_h, last_fmt;
    unsigned last_depth_inverted;
    float    last_mv_scale_x, last_mv_scale_y;

    uint64_t idx_map[8];
    bool     have_present;
    uint32_t present_idx;
    volatile LONG idx_conflicts;

    // Shared objects. copy_sem is ours (exported); nr_fence12 is the D3D12 half
    // of the release and nr_sem its Vulkan import; copy_fence12 is the D3D12
    // view of copy_sem. copy_fence[s] and release_fence[s] prove slot s's
    // previous batches executed before their command buffers are recorded again.
    SVkHandle copy_sem, nr_sem, copy_fence[kFgrSlots], release_fence[kFgrSlots];
    HANDLE    copy_sem_handle, nr_fence_handle;
    ID3D12Fence *copy_fence12, *nr_fence12;
    HANDLE    nr_ev;
    SVkHandle ev_out[kFgrSlots], ev_mark[kFgrSlots];
    bool     slot_used[kFgrSlots];
    FgrArm   arm[kFgrSlots];

    // Command pools, by the queue they are made for: the present queue and
    // DLSSG.CmdQueue are different families on this game.
    FgrPool  pools[2];
    unsigned npools;

    void    *cmd_queue;            // DLSSG.CmdQueue, cached at the evaluate

    SVkDevice dev;
    volatile LONG arm_seq, worker_seq, eval_seq, next_slot, pending_slot, last_eval_tick;
    volatile LONG armed, unarmed, skipped, busy, mismatched, writes, copies_failed;
    unsigned  unarmed_streak, mismatch_streak;
    uint64_t  copy_val, nr_val;    // the last values actually submitted / signalled for
    uint64_t  last_release_nr;     // the nr value the frame before this one waited
    uint64_t  nr_progress_value;   // watchdog, evaluate thread only: the shared fence's
    LONG      nr_progress_tick;    // completed value and when it last moved or caught up
    bool      have_last_group, last_group_served;   // evaluate thread only

    // Counters are telemetry only; atomic updates keep the present, evaluate and
    // worker threads independent of the relay's state lock.
    volatile LONG64 last_arm_qpc, copy_submit_qpc[kFgrSlots];
    volatile LONG nr_gpuwait, flips;

    // Throttle (FgRelayThrottle), indexed by site: 0 the game's present, 1 the
    // game's Reflex sleep. Each site is one game thread, so each has its own
    // event. thr_off is set once waits time out repeatedly and stays set for the
    // process. last_sleep_ms says the sleep site is live, which retires the
    // present site.
    HANDLE    thr_ev[2];
    volatile LONG thr_off, thr_streak;
    volatile LONG thr_calls[2], thr_waits[2], thr_clear[2], thr_timeouts[2];
    volatile LONG64 thr_sum[2], thr_max, last_sleep_ms;
    volatile LONG presents, p_pipeline_skip, p_notready, p_fallback, p_inactive;
    volatile LONG p_noinfo, p_stale, p_unlearned;
    volatile LONG arm_guard, arm_noreq, arm_refused, arm_stopping, arm_badres, arm_badsize, attempts;
    volatile LONG busy_copyfence, slot_busy, cf_seq, cf_idle, cf_nrfence;
    volatile LONG pool_cap, pool_family, pool_create, pool_alloc;
    volatile LONG drop_fence, drop_resetcb, drop_begin, drop_end, drop_submit;
    volatile LONG eval_disabled, eval_fallback, eval_notready, eval_noinput, eval_noqueue;
    volatile LONG evals, eval_consume, eval_inplace, eval_inplace_ok, eval_unarmed, eval_relbusy, eval_watchdog;
    volatile LONG rel_ok, rel_fail, rel_noqueue, rel_nopool, rel_resetfence, rel_resetcb, rel_begin, rel_end, rel_submit;
    volatile LONG w_plays, w_ok, w_fail, w_nr_absent, w_copy_fail;
    volatile LONG64 arm_gap_sum, arm2work_sum, arm2eval_sum, copywait_sum, copyland_sum, work_sum;
    volatile LONG arm_gap_n, arm2work_n, arm2eval_n, copywait_n, copyland_n, work_n;
    volatile LONG64 arm_gap_max, arm2work_max, arm2eval_max, copywait_max, copyland_max, work_max;

    PFN_FgrCreateSemaphore        CreateSemaphore;
    PFN_FgrImportSemaphoreWin32   ImportSemaphoreWin32;
    PFN_FgrGetSemaphoreWin32      GetSemaphoreWin32;
    PFN_FgrCreateFence            CreateFence;
    PFN_FgrResetFences            ResetFences;
    PFN_FgrWaitForFences          WaitForFences;
    PFN_FgrGetFenceStatus         GetFenceStatus;
    PFN_FgrCreateCommandPool      CreateCommandPool;
    PFN_FgrAllocateCommandBuffers AllocateCommandBuffers;
    PFN_FgrBeginCommandBuffer     BeginCommandBuffer;
    PFN_FgrEndCommandBuffer       EndCommandBuffer;
    PFN_FgrResetCommandBuffer     ResetCommandBuffer;
    PFN_FgrQueueSubmit            QueueSubmit;
};
static FgrState g_fgr;
static const unsigned kFgrAtPresent = 0, kFgrAtSleep = 1;   // throttle sites

static LONG FgrCount(volatile LONG &value) { return InterlockedIncrement(&value); }
static LONG FgrCountRead(volatile LONG &value) { return InterlockedCompareExchange(&value, 0, 0); }
static LONG64 FgrCountRead64(volatile LONG64 &value) { return InterlockedCompareExchange64(&value, 0, 0); }
static void FgrAdd64(volatile LONG64 &value, LONG64 amount) { InterlockedExchangeAdd64(&value, amount); }
static LONG64 FgrQpcNow()
{
    LARGE_INTEGER value = {};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
static void FgrMax64(volatile LONG64 &value, LONG64 candidate)
{
    LONG64 current = FgrCountRead64(value);
    while (current < candidate) {
        const LONG64 seen = InterlockedCompareExchange64(&value, candidate, current);
        if (seen == current) break;
        current = seen;
    }
}

static void FgrLock()   { EnterCriticalSection(&g_fgr.cs); }
static void FgrUnlock() { LeaveCriticalSection(&g_fgr.cs); }

// Called once from FgtInit (the hook registration path); every entry point
// refuses until it has run.
static void FgRelayInit()
{
    if (g_fgr.cs_ready) return;
    InitializeCriticalSection(&g_fgr.cs);
    g_fgr.cs_ready = true;
}

// The queue families, from the same vkGetDeviceQueue observer the trace uses. A
// command buffer submitted to a queue whose family its pool does not belong to is
// undefined behaviour, so the relay refuses rather than guesses when it has not
// seen the queue.
struct FgrQueueId { void *queue; uint32_t family; };
static FgrQueueId g_fgr_queue_ids[8];
static unsigned g_fgr_queue_id_count;
static volatile LONG g_fgr_queue_lock;

static void FgRelayNoteQueue(void *queue, uint32_t family)
{
    if (queue == nullptr) return;
    while (InterlockedCompareExchange(&g_fgr_queue_lock, 1, 0) != 0) Sleep(0);
    bool known = false;
    for (unsigned i = 0; i < g_fgr_queue_id_count; ++i)
        if (g_fgr_queue_ids[i].queue == queue) { known = true; break; }
    if (!known && g_fgr_queue_id_count < _countof(g_fgr_queue_ids)) {
        g_fgr_queue_ids[g_fgr_queue_id_count].queue = queue;
        g_fgr_queue_ids[g_fgr_queue_id_count].family = family;
        ++g_fgr_queue_id_count;
    }
    InterlockedExchange(&g_fgr_queue_lock, 0);
}

static bool FgRelayQueueFamily(void *queue, uint32_t *family)
{
    for (unsigned i = 0; i < g_fgr_queue_id_count; ++i)
        if (g_fgr_queue_ids[i].queue == queue) { *family = g_fgr_queue_ids[i].family; return true; }
    return false;
}

static bool FgRelayFallback(const char *why)
{
    if (!g_fgr.fell_back) {
        g_fgr.fell_back = true;
        g_fgr.why = why;
        Log("[fg-relay] %s. The serial feed at the frame-generation input is what runs, so the "
            "picture keeps its NR and the present is unaffected.", why);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Device extension injection: VK_KHR_external_semaphore_win32
// ---------------------------------------------------------------------------
//
// The relay's last two hops need VK_KHR_external_semaphore_win32, and a device
// extension can only be added while that device is created. Every Vulkan device
// in this process is created through ReShade's implicit layer, so the one hook
// this file adds is that layer's vkCreateDevice: the create info goes down as a
// stack copy with the extra name appended, and a driver that refuses the name
// gets the caller's own structure retried unchanged -- which is what makes this
// add-on unable to be the reason a game's device creation fails. The device is
// recorded only on a create that succeeded and either carried the name already
// or accepted it.
//
// The hook is installed only under [Adapter] Import=1 and only from the NGX hook
// worker (FgtMaintenance, under g_hook_cs), the path that installs the
// sl.interposer observers; with Import off nothing is patched at all.
//
// Whether the import is usable cannot be asked of the driver: vkGetDeviceProcAddr
// hands out the KHR entry points for a device that never enabled the extension,
// so a non-null answer is not proof. The answer is the table below instead,
// which only a create that came through this hook ever writes to.

static const uint32_t kFgrStDeviceCreateInfo = 3;   // VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO

// VkDeviceCreateInfo, declaring only what the injector reads and writes.
struct FgrDeviceCreateInfo
{
    uint32_t            sType;
    const void         *pNext;
    uint32_t            flags;
    uint32_t            queueCreateInfoCount;
    const void         *pQueueCreateInfos;
    uint32_t            enabledLayerCount;
    const char *const  *ppEnabledLayerNames;
    uint32_t            enabledExtensionCount;
    const char *const  *ppEnabledExtensionNames;
    const void         *pEnabledFeatures;
};

typedef uint32_t (*PFN_FgrLayerCreateDevice)(void *physical, const void *info, const void *allocator, void **out);
typedef void *(*PFN_FgrLayerGipa)(void *instance, const char *name);

static const char kFgrExternalSemaphoreWin32[] = "VK_KHR_external_semaphore_win32";
static const uint32_t kFgrErrorInitFailed = 0xFFFFFFFDu;   // VK_ERROR_INITIALIZATION_FAILED

// A caller with more extensions than this is not one the injector touches; its
// create goes through unmodified and its device is not recorded. The largest
// list any title in this project uses is three, and the bound is what keeps the
// appended list on the calling thread's stack.
static const uint32_t kFgrMaxCallerExtensions = 256;

static Hook g_fgr_create_device_hook;
static volatile LONG g_fgr_inject_state;    // 0 none, 1 installing, 2 hooked, -1 refused
static const char *volatile g_fgr_inject_note = "the injection path has not run";
static void *g_fgr_injected_devices[8];
static volatile LONG g_fgr_injected_count;
static volatile LONG g_fgr_injected_lock;

static void FgrRememberInjectedDevice(void *device)
{
    if (device == nullptr) return;
    while (InterlockedCompareExchange(&g_fgr_injected_lock, 1, 0) != 0) Sleep(0);
    bool known = false;
    for (LONG i = 0; i < g_fgr_injected_count; ++i)
        if (g_fgr_injected_devices[i] == device) { known = true; break; }
    if (!known && g_fgr_injected_count < static_cast<LONG>(_countof(g_fgr_injected_devices)))
        g_fgr_injected_devices[g_fgr_injected_count++] = device;
    InterlockedExchange(&g_fgr_injected_lock, 0);
}

// A device whose create was retried without the extension must not stay in the
// table: a later device can land on a retired one's address.
static void FgrForgetInjectedDevice(void *device)
{
    if (device == nullptr) return;
    while (InterlockedCompareExchange(&g_fgr_injected_lock, 1, 0) != 0) Sleep(0);
    for (LONG i = 0; i < g_fgr_injected_count; ++i)
        if (g_fgr_injected_devices[i] == device)
        {
            g_fgr_injected_devices[i] = g_fgr_injected_devices[--g_fgr_injected_count];
            break;
        }
    InterlockedExchange(&g_fgr_injected_lock, 0);
}

static bool FgrDeviceWasInjected(void *device)
{
    if (device == nullptr) return false;
    bool found = false;
    while (InterlockedCompareExchange(&g_fgr_injected_lock, 1, 0) != 0) Sleep(0);
    for (LONG i = 0; i < g_fgr_injected_count; ++i)
        if (g_fgr_injected_devices[i] == device) { found = true; break; }
    InterlockedExchange(&g_fgr_injected_lock, 0);
    return found;
}

static const char *FgrInjectionNote()
{
    const char *note = g_fgr_inject_note;
    return note != nullptr ? note : "the injection path reported no reason";
}

static uint32_t FgrCreateDeviceDetour(void *physical, const void *info, const void *allocator, void **out)
{
    auto next = reinterpret_cast<PFN_FgrLayerCreateDevice>(g_fgr_create_device_hook.original);
    if (next == nullptr) return kFgrErrorInitFailed;

    static volatile LONG logged_accept, logged_retry, logged_present;

    const FgrDeviceCreateInfo *caller = static_cast<const FgrDeviceCreateInfo *>(info);
    const char *names[kFgrMaxCallerExtensions + 1];
    FgrDeviceCreateInfo extended;
    const void *use = info;
    bool added = false, present = false;

    if (caller != nullptr && caller->sType == kFgrStDeviceCreateInfo &&
        caller->enabledExtensionCount <= kFgrMaxCallerExtensions &&
        (caller->enabledExtensionCount == 0 || caller->ppEnabledExtensionNames != nullptr))
    {
        const uint32_t count = caller->enabledExtensionCount;
        for (uint32_t i = 0; i < count && !present; ++i)
        {
            const char *name = caller->ppEnabledExtensionNames != nullptr
                ? caller->ppEnabledExtensionNames[i] : nullptr;
            present = name != nullptr && strcmp(name, kFgrExternalSemaphoreWin32) == 0;
        }
        if (!present)
        {
            for (uint32_t i = 0; i < count; ++i) names[i] = caller->ppEnabledExtensionNames[i];
            names[count] = kFgrExternalSemaphoreWin32;
            extended = *caller;
            extended.enabledExtensionCount = count + 1;
            extended.ppEnabledExtensionNames = names;
            use = &extended;
            added = true;
        }
    }

    const uint32_t result = next(physical, use, allocator, out);

    if (added && result != kSVkSuccess)
    {
        // The driver would not take the extra name. The same call goes through
        // again with the caller's own structure, so the device the game gets is
        // what it would have gotten without this add-on -- and the device is not
        // recorded, so the relay stays on its host paths for it.
        const uint32_t retry = next(physical, info, allocator, out);
        if (retry == kSVkSuccess && out != nullptr && *out != nullptr)
            FgrForgetInjectedDevice(*out);
        g_fgr_inject_note = "device creation refused VK_KHR_external_semaphore_win32 and "
                            "went through again with the caller's own list";
        if (InterlockedCompareExchange(&logged_retry, 1, 0) == 0)
            Log("[fg-relay] Import=1: device creation refused VK_KHR_external_semaphore_win32 "
                "(result %u) and was retried with the caller's own extension list (result %u). "
                "The import stays off for this device.", result, retry);
        return retry;
    }
    if (result == kSVkSuccess && out != nullptr && *out != nullptr && (added || present))
    {
        FgrRememberInjectedDevice(*out);
        if (added && InterlockedCompareExchange(&logged_accept, 1, 0) == 0)
            Log("[fg-relay] Import=1: the device created at %p carries "
                "VK_KHR_external_semaphore_win32.", *out);
        else if (!added && InterlockedCompareExchange(&logged_present, 1, 0) == 0)
            Log("[fg-relay] Import=1: the device created at %p already carried "
                "VK_KHR_external_semaphore_win32; it is recorded for the import.", *out);
    }
    return result;
}

// Called from FgtMaintenance under g_hook_cs, once for the process. A refusal is
// not retried: the layer and the device are made once, and the fallback paths
// are complete without the import.
static void FgRelayInstallDeviceHook()
{
    if (!PresentAdapterRelayImport()) return;
    if (InterlockedCompareExchange(&g_fgr_inject_state, 1, 0) != 0) return;

    HMODULE layer = g_reshade_dll;
    void *create = nullptr;
    if (layer != nullptr)
    {
        // The entry the game's call reaches. ReShade's layer answers
        // vkGetInstanceProcAddr with no instance for its own exports, and this is
        // the address RenoDX's context hooks use in this process, with this
        // ReShade version, to see the game's device creation.
        auto gipa = reinterpret_cast<PFN_FgrLayerGipa>(GetProcAddress(layer, "vkGetInstanceProcAddr"));
        if (gipa != nullptr) create = gipa(nullptr, "vkCreateDevice");
        if (create == nullptr) create = reinterpret_cast<void *>(GetProcAddress(layer, "vkCreateDevice"));
    }
    if (create == nullptr)
    {
        g_fgr_inject_note = layer != nullptr
            ? "the ReShade layer module offers no vkCreateDevice"
            : "the ReShade layer module was not located";
        InterlockedExchange(&g_fgr_inject_state, -1);
        Log("[fg-relay] Import=1 was asked for, but %s, so the extension cannot be added to "
            "any device and the relay keeps its host paths.", g_fgr_inject_note);
        return;
    }
    if (!HookInstall(g_fgr_create_device_hook, create, reinterpret_cast<void *>(&FgrCreateDeviceDetour)))
    {
        g_fgr_inject_note = "the create-device hook could not be installed";
        InterlockedExchange(&g_fgr_inject_state, -1);
        Log("[fg-relay] Import=1 was asked for, but hooking the ReShade layer's vkCreateDevice "
            "at %p failed; the relay keeps its host paths.", create);
        return;
    }
    g_fgr_inject_note = "no device creation has gone through the hook";
    InterlockedExchange(&g_fgr_inject_state, 2);
    Log("[fg-relay] Import=1: the ReShade layer's vkCreateDevice at %p is hooked. Every device "
        "created from here on gets VK_KHR_external_semaphore_win32 appended to its extension "
        "list, and a driver that refuses the name gets the caller's own list retried unchanged.",
        create);
}

// ---------------------------------------------------------------------------
// Open: resolve the extra entry points, create the shared objects, take the
// import's answer from the injected-device table. Runs from the evaluate thread,
// once a private session exists.
// ---------------------------------------------------------------------------

static bool FgRelayCreateObjects(bool import_ok)
{
    FgrState &f = g_fgr;
    SVkDevice dev = g_svk.dev;
    auto get_proc = reinterpret_cast<PFN_SVkGetDeviceProcAddr>(GetProcAddress(g_svk.lib, "vkGetDeviceProcAddr"));
    if (get_proc == nullptr) return false;

    #define FGRVK_GET(member, name)                                        \
        f.member = reinterpret_cast<PFN_Fgr##member>(get_proc(dev, name)); \
        if (f.member == nullptr) { Log("[fg-relay] vkGetDeviceProcAddr has no %s", name); return false; }
    FGRVK_GET(CreateSemaphore,        "vkCreateSemaphore")
    FGRVK_GET(CreateFence,            "vkCreateFence")
    FGRVK_GET(ResetFences,            "vkResetFences")
    FGRVK_GET(WaitForFences,          "vkWaitForFences")
    FGRVK_GET(GetFenceStatus,         "vkGetFenceStatus")
    FGRVK_GET(CreateCommandPool,      "vkCreateCommandPool")
    FGRVK_GET(AllocateCommandBuffers, "vkAllocateCommandBuffers")
    FGRVK_GET(BeginCommandBuffer,     "vkBeginCommandBuffer")
    FGRVK_GET(EndCommandBuffer,       "vkEndCommandBuffer")
    FGRVK_GET(ResetCommandBuffer,     "vkResetCommandBuffer")
    FGRVK_GET(QueueSubmit,            "vkQueueSubmit")
    if (import_ok) {
        // Only for a device the extension is known to be on, which is the
        // injected-device table and nothing else; a non-null vkGetDeviceProcAddr
        // answer is not proof, so neither is resolved or called on a guess.
        FGRVK_GET(ImportSemaphoreWin32, "vkImportSemaphoreWin32HandleKHR")
        FGRVK_GET(GetSemaphoreWin32,    "vkGetSemaphoreWin32HandleKHR")
    }
    #undef FGRVK_GET

    // The park's ring. ev_out is what Streamline's command buffer waits on;
    // ev_mark is set by that same command buffer after the wait, so the host can
    // prove the wait passed before the slot is used again.
    for (unsigned i = 0; i < kFgrSlots; ++i) {
        SVkEventCreateInfo eci = {};
        eci.sType = kSVkStEventCreateInfo;
        if (g_svk.EvCreate(dev, &eci, nullptr, &f.ev_out[i]) != kSVkSuccess ||
            g_svk.EvCreate(dev, &eci, nullptr, &f.ev_mark[i]) != kSVkSuccess)
            return false;
    }

    FgrFenceCreateInfo fci = {};
    fci.sType = kFgrStFenceCreate;
    fci.flags = kFgrFenceSignaledFlag;
    for (unsigned i = 0; i < kFgrSlots; ++i) {
        if (f.CreateFence(dev, &fci, nullptr, &f.copy_fence[i]) != kSVkSuccess) return false;
        if (f.CreateFence(dev, &fci, nullptr, &f.release_fence[i]) != kSVkSuccess) return false;
    }

    if (import_ok) {
        // copy_sem: a Vulkan timeline semaphore exported as a D3D12 fence, so
        // the private queue can wait the copies without the host.
        FgrSemaphoreTypeCreateInfo st = {};
        st.sType = kFgrStSemaphoreType;
        st.semaphoreType = kFgrSemTypeTimeline;
        FgrExportSemaphoreCreateInfo ex = {};
        ex.sType = kFgrStExportSemaphore;
        ex.pNext = &st;
        ex.handleTypes = kFgrHandleTypeD3D12Fence;
        FgrSemaphoreCreateInfo sci = {};
        sci.sType = kFgrStSemaphoreCreate;
        sci.pNext = &ex;
        if (f.CreateSemaphore(dev, &sci, nullptr, &f.copy_sem) != kSVkSuccess) return false;
        FgrGetSemaphoreWin32 gw = {};
        gw.sType = kFgrStGetSemWin32;
        gw.semaphore = f.copy_sem;
        gw.handleType = kFgrHandleTypeD3D12Fence;
        if (f.GetSemaphoreWin32(dev, &gw, &f.copy_sem_handle) != kSVkSuccess ||
            f.copy_sem_handle == nullptr)
            return false;
        void *fence = nullptr;
        if (FAILED(g_s12.dev->OpenSharedHandle(f.copy_sem_handle, __uuidof(ID3D12Fence), &fence)) ||
            fence == nullptr)
            return false;
        f.copy_fence12 = static_cast<ID3D12Fence *>(fence);

        // nr_fence12: the release. Created shared on the D3D12 side and imported
        // as a Vulkan timeline semaphore, which is the direction the release
        // batch waits.
        if (FAILED(g_s12.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence),
                                          reinterpret_cast<void **>(&f.nr_fence12))) ||
            f.nr_fence12 == nullptr)
            return false;
        if (FAILED(g_s12.dev->CreateSharedHandle(f.nr_fence12, nullptr, GENERIC_ALL, nullptr,
                                                 &f.nr_fence_handle)) ||
            f.nr_fence_handle == nullptr)
            return false;
        FgrSemaphoreTypeCreateInfo st2 = {};
        st2.sType = kFgrStSemaphoreType;
        st2.semaphoreType = kFgrSemTypeTimeline;
        FgrSemaphoreCreateInfo sci2 = {};
        sci2.sType = kFgrStSemaphoreCreate;
        sci2.pNext = &st2;
        if (f.CreateSemaphore(dev, &sci2, nullptr, &f.nr_sem) != kSVkSuccess) return false;
        FgrImportSemaphoreWin32 imp = {};
        imp.sType = kFgrStImportSemWin32;
        imp.semaphore = f.nr_sem;
        imp.handleType = kFgrHandleTypeD3D12Fence;
        imp.handle = f.nr_fence_handle;
        if (f.ImportSemaphoreWin32(dev, &imp) != kSVkSuccess) return false;
    }

    f.nr_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (f.nr_ev == nullptr) return false;
    for (unsigned i = 0; i < 2; ++i)
        if (f.thr_ev[i] == nullptr) f.thr_ev[i] = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    f.dev = dev;
    return true;
}

static void FgRelayEnsure()
{
    FgrState &f = g_fgr;
    if (!PresentAdapterRelay() || f.fell_back || !f.cs_ready) return;
    if (f.ready && f.dev != nullptr && f.dev != g_svk.dev) {
        // A device that is not the one these objects were made on retires them;
        // they went with the old one. The bookkeeping is cleared and the objects
        // are made again below.
        f.ready = false;
        f.tried = false;
        f.npools = 0;
        memset(f.pools, 0, sizeof(f.pools));
        for (unsigned i = 0; i < kFgrSlots; ++i) { f.ev_out[i] = f.ev_mark[i] = 0; f.slot_used[i] = false; }
        f.arm_seq = f.worker_seq = f.eval_seq = 0;
        f.copy_val = f.nr_val = f.last_release_nr = f.nr_progress_value = 0;
        for (unsigned i = 0; i < kFgrSlots; ++i) {
            f.copy_in_flight[i] = f.release_in_flight[i] = false;
            InterlockedExchange64(&f.copy_submit_qpc[i], 0);
        }
        f.have_last_group = false;
        f.have_last = f.have_present = false;
        memset(f.idx_map, 0, sizeof(f.idx_map));
        f.copy_fence12 = nullptr;   // the old device's objects went with it
        f.nr_fence12 = nullptr;
    }
    if (f.ready || f.tried) return;
    if (!g_svk.ready || g_svk.dev == nullptr || g_s12.dev == nullptr || g_s12.queue == nullptr ||
        g_s12.tex[SLOT_COLOR] == nullptr || g_svk.img[SLOT_COLOR] == 0)
        return;
    f.tried = true;

    // The import is an explicit opt-in (Import=1) and is taken only for a device
    // the extension was actually accepted on: the create-device hook's record
    // (see "Device extension injection"). The GPU entry points are deliberately
    // not consulted -- they resolve on a device that never enabled the extension
    // and the call then faults (2026-09-29). The default is the host release.
    const bool have_import = PresentAdapterRelayImport() && FgrDeviceWasInjected(g_svk.dev);

    if (!FgRelayCreateObjects(have_import)) {
        FgRelayFallback("the GPU relay could not create its Vulkan objects");
        return;
    }
    f.import_ok = have_import;
    f.ready = true;
    if (PresentAdapterRelayImport() && !have_import)
        Log("[fg-relay] Import=1 was asked for, but this device is not one the extension was "
            "injected into: %s. The copy wait and the park release fall back to the host.",
            FgrInjectionNote());
    Log("[fg-relay] open. Copy completion is signalled to the private queue %s; the park release "
        "%s. Pipeline=%d Import=%d.",
        have_import ? "on the GPU (exported timeline semaphore)" : "by a host wait (no shared-fence import)",
        have_import ? "is set on the GPU (imported D3D12 fence)" : "is set by the host after the session's own fences",
        PresentAdapterPipeline(), PresentAdapterRelayImport() ? 1 : 0);
}

// ---------------------------------------------------------------------------
// Pools and the copy batch
// ---------------------------------------------------------------------------

static FgrPool *FgRelayPoolFor(void *queue, bool want_release)
{
    FgrState &f = g_fgr;
    // Two threads reach this (the present hook and the evaluate), each making its
    // own queue's pool once. The critical section's rule holds: no file I/O and
    // no waits under it, and the driver calls here happen once per queue.
    FgrLock();
    FgrPool *found = nullptr;
    for (unsigned i = 0; i < f.npools; ++i)
        if (f.pools[i].queue == queue) { found = &f.pools[i]; break; }
    if (found == nullptr && f.npools >= _countof(f.pools)) {
        FgrCount(f.pool_cap);
        FgrUnlock();
        return nullptr;
    }
    if (found != nullptr && (!want_release || found->cb_release[0] != nullptr)) { FgrUnlock(); return found; }

    uint32_t family = 0;
    bool made_pool = false;
    if (found == nullptr) {
        if (!FgRelayQueueFamily(queue, &family)) {
            FgrCount(f.pool_family);
            FgrUnlock();
            // Undefined behaviour to guess; and a relay that can never arm would
            // leave every frame without NR, so it stands down to the serial feed.
            FgRelayFallback("a queue no vkGetDeviceQueue observer saw is used for the copy, so its family is "
                            "unknown and no command pool can be made for it");
            return nullptr;
        }
        FgrPool &p = f.pools[f.npools];
        FgrCommandPoolCreateInfo pci = {};
        pci.sType = kFgrStCommandPoolCreate;
        pci.flags = kFgrPoolResetCommandBuffer;
        pci.queueFamilyIndex = family;
        if (f.CreateCommandPool(g_svk.dev, &pci, nullptr, &p.pool) != kSVkSuccess) {
            FgrCount(f.pool_create);
            FgrUnlock();
            return nullptr;
        }
        p.queue = queue;
        FgrCommandBufferAllocInfo ai = {};
        ai.sType = kFgrStCommandBufferAlloc;
        ai.pool = p.pool;
        ai.level = kFgrPrimary;
        ai.count = kFgrSlots;
        if (f.AllocateCommandBuffers(g_svk.dev, &ai, p.cb_copy) != kSVkSuccess) {
            FgrCount(f.pool_alloc);
            FgrUnlock();
            return nullptr;
        }
        ++f.npools;
        made_pool = true;
        found = &p;
    }
    if (want_release && found->cb_release[0] == nullptr) {
        FgrCommandBufferAllocInfo ai = {};
        ai.sType = kFgrStCommandBufferAlloc;
        ai.pool = found->pool;
        ai.level = kFgrPrimary;
        ai.count = kFgrSlots;
        if (f.AllocateCommandBuffers(g_svk.dev, &ai, found->cb_release) != kSVkSuccess) {
            FgrCount(f.pool_alloc);
            FgrUnlock();
            return nullptr;
        }
    }
    FgrUnlock();
    if (made_pool) Log("[fg-relay] command pool for queue %p: family=%u.", queue, family);
    return found;
}

// Records the four copies into the private images exactly as the serial feed
// does, minus the vkCmdSetEvent that told the worker the copies were in (this
// batch's completion is what carries that fact now).
static void FgRelayRecordCopy(SVkCommandBuffer cb, const FgrArm &a)
{
    const uint32_t first = g_svk.laid_out ? kSVkLayoutGeneral : kSVkLayoutUndefined;
    SynthVkBarrier(cb, a.hud,   kSVkLayoutGeneral, kSVkLayoutTransferSrc);
    SynthVkBarrier(cb, a.back,  kSVkLayoutGeneral, kSVkLayoutTransferSrc);
    SynthVkBarrier(cb, a.depth, kSVkLayoutGeneral, kSVkLayoutTransferSrc);
    SynthVkBarrier(cb, a.mv,    kSVkLayoutGeneral, kSVkLayoutTransferSrc);
    for (int i = 0; i < SLOT_COUNT; ++i) SynthVkBarrier(cb, g_svk.img[i], first, kSVkLayoutGeneral);
    SynthVkBarrier(cb, g_svk.img[kOutB], first, kSVkLayoutGeneral);
    g_svk.laid_out = true;
    SynthVkCopyImage(cb, a.hud,   kSVkLayoutTransferSrc, g_svk.img[SLOT_COLOR], kSVkLayoutGeneral, a.w, a.h);
    SynthVkCopyImage(cb, a.back,  kSVkLayoutTransferSrc, g_svk.img[kOutB],      kSVkLayoutGeneral, a.w, a.h);
    SynthVkCopyImage(cb, a.mv,    kSVkLayoutTransferSrc, g_svk.img[SLOT_MV],    kSVkLayoutGeneral, a.w, a.h);
    SynthVkCopyImage(cb, a.depth, kSVkLayoutTransferSrc, g_svk.img[SLOT_DEPTH], kSVkLayoutGeneral, a.w, a.h);
    SynthVkBarrier(cb, a.hud,   kSVkLayoutTransferSrc, kSVkLayoutGeneral);
    SynthVkBarrier(cb, a.back,  kSVkLayoutTransferSrc, kSVkLayoutGeneral);
    SynthVkBarrier(cb, a.depth, kSVkLayoutTransferSrc, kSVkLayoutGeneral);
    SynthVkBarrier(cb, a.mv,    kSVkLayoutTransferSrc, kSVkLayoutGeneral);
}

// The one arm path, used by the present hook (Pipeline=3, handles learned from
// the last evaluate) and by the evaluate (Pipeline=2, its own handles). Runs
// while the caller owns `queue` on its own thread; only the bookkeeping is under
// the lock, and no driver call is made while holding it.
//
// Ordering that the park depends on: the slot's events are reset HERE, before the
// worker is woken, and never again for this frame -- the host-side release
// (vkSetEvent from the worker when the import is unavailable) may otherwise run
// before the evaluate records the wait, and a reset after it would leave the
// command buffer parked on an event nothing sets. The contract the worker reads
// is also written here, while the worker is provably idle.
static bool FgRelayArmCopy(void *queue, const char *who, const VkmRes &hud, const VkmRes &back,
                           const VkmRes &depth, const VkmRes &mv,
                           unsigned depth_inverted, float mv_scale_x, float mv_scale_y)
{
    FgrState &f = g_fgr;
    if (!f.ready || f.fell_back || !f.cs_ready) { FgrCount(f.arm_guard); return false; }
    if (!g_present_adapter.run_nr || g_present_adapter.remaining == 0) { FgrCount(f.arm_noreq); return false; }
    if (g_present_adapter.refused || g_fg_input.refused || g_vkm.refused) { FgrCount(f.arm_refused); return false; }
    if (g_vkm.stopping) { FgrCount(f.arm_stopping); return false; }
    if (g_svk.dev == nullptr || hud.image == 0 || back.image == 0 || depth.image == 0 || mv.image == 0)
    { FgrCount(f.arm_badres); return false; }
    if (hud.w != back.w || hud.h != back.h || depth.w != back.w || depth.h != back.h ||
        mv.w != back.w || mv.h != back.h || back.w == 0 || back.h == 0)
    { FgrCount(f.arm_badsize); return false; }
    FgrCount(f.attempts);

    // Each slot's copy batch is re-recorded when the slot comes round again, so
    // that slot's previous batch must be over first. Only a submit made below
    // puts a fence in flight; a failed submit leaves it unsignalled with nothing
    // to wait for, and the slot is retried. Host side, no lock: only this thread
    // submits copies, so only it reads or writes copy_in_flight.
    for (unsigned i = 0; i < kFgrSlots; ++i) {
        if (!f.copy_in_flight[i] || f.GetFenceStatus(g_svk.dev, f.copy_fence[i]) != kSVkSuccess) continue;
        f.copy_in_flight[i] = false;
        const LONG64 submitted = InterlockedExchange64(&f.copy_submit_qpc[i], 0);
        if (submitted != 0) {
            const LONG64 landed_ticks = FgrQpcNow() - submitted;
            FgrAdd64(f.copyland_sum, landed_ticks);
            FgrCount(f.copyland_n);
            FgrMax64(f.copyland_max, landed_ticks);
        }
    }

    // Where the copy runs on the queue the park and the release are on
    // (Pipeline=2) and the release is a GPU wait (Import=1), an NR the D3D12
    // side has not finished yet does not skip the frame: the copy batch waits
    // for it on the GPU instead. That wait protects the staging images the
    // unfinished NR is still reading, and it costs the queue nothing the
    // previous frame's release batch, submitted earlier on the same queue with
    // the same wait, has not already cost it. Elsewhere the host gate stays.
    const bool nr_on_gpu = f.import_ok && f.nr_fence12 != nullptr && PresentAdapterRelayAtEvaluate();

    FgrArm a = {};
    bool take = false, nr_pending = false;
    uint64_t prev_nr = 0;
    FgrLock();
    // Clean: the previous frame was consumed by both the worker and the evaluate,
    // the worker has finished with the staging images and the contract (idle), and
    // -- with the import live, where the worker only queues -- the D3D12 side has
    // actually completed the previous NR or the copy will wait for it on the GPU.
    // Each failing conjunct is counted on its own so the log names the gate that
    // is holding the chain back instead of showing one lumped "skipped" rate.
    const bool clean_seq  = f.arm_seq == f.worker_seq && f.arm_seq == f.eval_seq;
    const bool clean_idle = g_vkm.idle == nullptr || WaitForSingleObject(g_vkm.idle, 0) == WAIT_OBJECT_0;
    nr_pending = f.import_ok && f.nr_fence12 != nullptr && f.nr_fence12->GetCompletedValue() < f.nr_val;
    const bool clean_nr   = !nr_pending || nr_on_gpu;
    if (!clean_seq)  FgrCount(f.cf_seq);
    if (!clean_idle) FgrCount(f.cf_idle);
    if (!clean_nr)   FgrCount(f.cf_nrfence);
    const bool clean = clean_seq && clean_idle && clean_nr;
    if (clean) {
        const unsigned slot = static_cast<unsigned>(InterlockedIncrement(&f.next_slot)) % kFgrSlots;
        if (f.copy_in_flight[slot]) { FgrCount(f.busy); FgrCount(f.busy_copyfence); }
        else if (!f.slot_used[slot] || g_svk.EvStatus(g_svk.dev, f.ev_mark[slot]) == kSVkEventSet) {
            a.slot = slot;
            prev_nr = f.nr_val;
            a.hud = hud.image; a.back = back.image; a.depth = depth.image; a.mv = mv.image;
            a.w = back.w; a.h = back.h; a.vk_fmt = back.fmt;
            a.copy_val = f.copy_val + 1;
            a.nr_val   = f.nr_val + 1;
            g_svk.EvReset(g_svk.dev, f.ev_out[slot]);
            g_svk.EvReset(g_svk.dev, f.ev_mark[slot]);
            f.slot_used[slot] = true;
            take = true;
        } else { FgrCount(f.busy); FgrCount(f.slot_busy); }
    } else FgrCount(f.skipped);
    FgrUnlock();
    if (!take) return false;

    // From here the slot is ours. Every failure past this point gives it back:
    // a slot left marked used with no batch coming to set its ev_mark can never
    // be taken again, and four of those end the relay for the session.
    auto drop_slot = [&]() { FgrLock(); f.slot_used[a.slot] = false; FgrUnlock(); };

    FgrPool *pool = FgRelayPoolFor(queue, false);
    if (pool == nullptr || pool->cb_copy[a.slot] == nullptr) { drop_slot(); return false; }
    const SVkCommandBuffer cb = pool->cb_copy[a.slot];
    if (f.ResetFences(g_svk.dev, 1, &f.copy_fence[a.slot]) != kSVkSuccess) { FgrCount(f.drop_fence); drop_slot(); return false; }
    if (f.ResetCommandBuffer(cb, 0) != kSVkSuccess) { FgrCount(f.drop_resetcb); drop_slot(); return false; }
    FgrCommandBufferBeginInfo bi = {};
    bi.sType = kFgrStCommandBufferBegin;
    bi.flags = kFgrCbOneTimeSubmit;
    if (f.BeginCommandBuffer(cb, &bi) != kSVkSuccess) { FgrCount(f.drop_begin); drop_slot(); return false; }
    FgRelayRecordCopy(cb, a);
    if (f.EndCommandBuffer(cb) != kSVkSuccess) { FgrCount(f.drop_end); drop_slot(); return false; }

    // The worker queued the Signal for prev_nr before it went idle (checked
    // above), so this wait always names a signal already in the D3D12 queue.
    const bool wait_nr = nr_on_gpu && prev_nr != 0;
    uint64_t wait_value = prev_nr;
    uint64_t signal_value = a.copy_val;
    uint32_t wait_stage = kSVkStageAllCommands;
    FgrTimelineSubmitInfo ts = {};
    ts.sType = kFgrStTimelineSubmit;
    ts.waitValueCount = wait_nr ? 1 : 0;
    ts.pWaitValues = wait_nr ? &wait_value : nullptr;
    ts.signalValueCount = 1;
    ts.pSignalValues = &signal_value;
    const SVkHandle signal_sem = f.copy_sem;
    FgrSubmitInfo si = {};
    si.sType = kFgrStSubmitInfo;
    si.pNext = f.import_ok ? &ts : nullptr;
    si.waitCount = wait_nr ? 1 : 0;
    si.pWaits = wait_nr ? &f.nr_sem : nullptr;
    si.pWaitStages = wait_nr ? &wait_stage : nullptr;
    si.cbCount = 1;
    si.pCbs = &cb;
    si.signalCount = f.import_ok ? 1 : 0;
    si.pSignals = f.import_ok ? &signal_sem : nullptr;
    if (f.QueueSubmit(reinterpret_cast<SVkQueue>(queue), 1, &si, f.copy_fence[a.slot]) != kSVkSuccess) {
        FgrCount(f.copies_failed);
        FgrCount(f.drop_submit);
        drop_slot();
        return false;
    }
    f.copy_in_flight[a.slot] = true;
    InterlockedExchange64(&f.copy_submit_qpc[a.slot], FgrQpcNow());
    if (wait_nr && nr_pending) FgrCount(f.nr_gpuwait);

    // The worker is idle (checked under the lock above) and nothing else writes
    // the contract while the relay owns the frames, so this is the only writer.
    SynthBuild(&g_vkm.sp, a.w, a.h, MapVkColor(a.vk_fmt), reshade::api::resource{0}, reshade::api::resource{0},
               reshade::api::resource{0}, 0, -1);
    g_vkm.sp.Set("DLSS.Feature.Create.Flags", depth_inverted != 0 ? 0x08u : 0u);
    g_vkm.sp.Set("MV.Scale.X", mv_scale_x);
    g_vkm.sp.Set("MV.Scale.Y", mv_scale_y);

    FgrLock();
    f.copy_val = a.copy_val;
    f.nr_val = a.nr_val;
    const LONG64 arm_tick = FgrQpcNow();
    a.tick = arm_tick;
    const LONG64 previous_arm_tick = FgrCountRead64(f.last_arm_qpc);
    if (previous_arm_tick != 0) {
        const LONG64 gap = arm_tick - previous_arm_tick;
        FgrAdd64(f.arm_gap_sum, gap);
        FgrMax64(f.arm_gap_max, gap);
        FgrCount(f.arm_gap_n);
    }
    InterlockedExchange64(&f.last_arm_qpc, arm_tick);
    f.pending_slot = static_cast<LONG>(a.slot);
    f.arm[a.slot] = a;
    f.arm[a.slot].valid = true;
    f.arm_seq = f.arm_seq + 1;
    const unsigned armed = static_cast<unsigned>(FgrCount(f.armed));
    const LONG seq = f.arm_seq;
    FgrUnlock();

    ResetEvent(g_vkm.idle);
    SetEvent(g_vkm.wake);
    if (armed <= 3 || armed % 1800 == 0)
        Log("[fg-relay] armed by %s: hud=%llx back=%llx depth=%llx mv=%llx %ux%u copy=%llu nr=%llu "
            "import=%d slot=%u arm=%ld gpu-wait-nr=%llu pending=%d.",
            who, a.hud, a.back, a.depth, a.mv, a.w, a.h, a.copy_val, a.nr_val,
            f.import_ok ? 1 : 0, a.slot, seq, wait_nr ? prev_nr : 0ull, nr_pending ? 1 : 0);
    return true;
}

// ---------------------------------------------------------------------------
// Instrumentation: one line every 15 s, print-only
// ---------------------------------------------------------------------------

// Reports the counters above as rates, gate by gate, so the log names which
// gate is holding the chain back instead of leaving it to be inferred from
// rates several gates can produce. Called from the present thread in both
// relay pipelines; reads only.
static void FgRelayPeriodic()
{
    FgrState &f = g_fgr;
    if (!f.cs_ready) return;

    struct Snap {
        unsigned presents, p_pipeline_skip, p_notready, p_fallback, p_inactive, noinfo, stale, unlearned;
        unsigned attempts, armed, skipped, busy;
        unsigned arm_guard, arm_noreq, arm_refused, arm_stopping, arm_badres, arm_badsize;
        unsigned pool_cap, pool_family, pool_create, pool_alloc;
        unsigned drop_fence, drop_resetcb, drop_begin, drop_end, drop_submit;
        unsigned eval_disabled, eval_fallback, eval_notready, eval_noinput, eval_noqueue;
        unsigned input_evals, index_skipped, evals, consume, inplace, inplace_ok, unarmed, mismatch, relbusy, relok, relfail;
        unsigned rel_noqueue, rel_nopool, rel_resetfence, rel_resetcb, rel_begin, rel_end, rel_submit, watchdog;
        unsigned cfseq, cfidle, cfnr, slotbusy, copybusy, nrgpuwait, flips;
        unsigned wplays, wok, wfail, wnr, wcopyfail;
    };
    static volatile LONG64 last_tick;
    static Snap prev;

    const ULONGLONG now = GetTickCount64();
    const LONG64 previous_tick = FgrCountRead64(last_tick);
    if (previous_tick == 0) { InterlockedExchange64(&last_tick, static_cast<LONG64>(now)); return; }
    const ULONGLONG elapsed = now - static_cast<ULONGLONG>(previous_tick);
    if (elapsed < 15000) return;
    if (InterlockedCompareExchange64(&last_tick, static_cast<LONG64>(now), previous_tick) != previous_tick) return;

    Snap cur = {};
    cur.presents   = static_cast<unsigned>(FgrCountRead(f.presents));
    cur.p_pipeline_skip = static_cast<unsigned>(FgrCountRead(f.p_pipeline_skip));
    cur.p_notready = static_cast<unsigned>(FgrCountRead(f.p_notready));
    cur.p_fallback = static_cast<unsigned>(FgrCountRead(f.p_fallback));
    cur.p_inactive = static_cast<unsigned>(FgrCountRead(f.p_inactive));
    cur.noinfo     = static_cast<unsigned>(FgrCountRead(f.p_noinfo));
    cur.stale      = static_cast<unsigned>(FgrCountRead(f.p_stale));
    cur.unlearned  = static_cast<unsigned>(FgrCountRead(f.p_unlearned));
    cur.attempts   = static_cast<unsigned>(FgrCountRead(f.attempts));
    cur.armed      = static_cast<unsigned>(FgrCountRead(f.armed));
    cur.skipped    = static_cast<unsigned>(FgrCountRead(f.skipped));
    cur.busy       = static_cast<unsigned>(FgrCountRead(f.busy));
    cur.arm_guard  = static_cast<unsigned>(FgrCountRead(f.arm_guard));
    cur.arm_noreq  = static_cast<unsigned>(FgrCountRead(f.arm_noreq));
    cur.arm_refused = static_cast<unsigned>(FgrCountRead(f.arm_refused));
    cur.arm_stopping = static_cast<unsigned>(FgrCountRead(f.arm_stopping));
    cur.arm_badres = static_cast<unsigned>(FgrCountRead(f.arm_badres));
    cur.arm_badsize = static_cast<unsigned>(FgrCountRead(f.arm_badsize));
    cur.pool_cap = static_cast<unsigned>(FgrCountRead(f.pool_cap));
    cur.pool_family = static_cast<unsigned>(FgrCountRead(f.pool_family));
    cur.pool_create = static_cast<unsigned>(FgrCountRead(f.pool_create));
    cur.pool_alloc = static_cast<unsigned>(FgrCountRead(f.pool_alloc));
    cur.drop_fence = static_cast<unsigned>(FgrCountRead(f.drop_fence));
    cur.drop_resetcb = static_cast<unsigned>(FgrCountRead(f.drop_resetcb));
    cur.drop_begin = static_cast<unsigned>(FgrCountRead(f.drop_begin));
    cur.drop_end = static_cast<unsigned>(FgrCountRead(f.drop_end));
    cur.drop_submit = static_cast<unsigned>(FgrCountRead(f.drop_submit));
    cur.eval_disabled = static_cast<unsigned>(FgrCountRead(f.eval_disabled));
    cur.eval_fallback = static_cast<unsigned>(FgrCountRead(f.eval_fallback));
    cur.eval_notready = static_cast<unsigned>(FgrCountRead(f.eval_notready));
    cur.eval_noinput = static_cast<unsigned>(FgrCountRead(f.eval_noinput));
    cur.eval_noqueue = static_cast<unsigned>(FgrCountRead(f.eval_noqueue));
    cur.input_evals = static_cast<unsigned>(FgrCountRead(g_fg_input.eval_calls));
    cur.index_skipped = static_cast<unsigned>(FgrCountRead(g_fg_input.eval_index_skipped));
    cur.evals      = static_cast<unsigned>(FgrCountRead(f.evals));
    cur.consume    = static_cast<unsigned>(FgrCountRead(f.eval_consume));
    cur.inplace    = static_cast<unsigned>(FgrCountRead(f.eval_inplace));
    cur.inplace_ok = static_cast<unsigned>(FgrCountRead(f.eval_inplace_ok));
    cur.unarmed    = static_cast<unsigned>(FgrCountRead(f.unarmed));
    cur.mismatch   = static_cast<unsigned>(FgrCountRead(f.mismatched));
    cur.relbusy    = static_cast<unsigned>(FgrCountRead(f.eval_relbusy));
    cur.relok      = static_cast<unsigned>(FgrCountRead(f.rel_ok));
    cur.relfail    = static_cast<unsigned>(FgrCountRead(f.rel_fail));
    cur.rel_noqueue = static_cast<unsigned>(FgrCountRead(f.rel_noqueue));
    cur.rel_nopool = static_cast<unsigned>(FgrCountRead(f.rel_nopool));
    cur.rel_resetfence = static_cast<unsigned>(FgrCountRead(f.rel_resetfence));
    cur.rel_resetcb = static_cast<unsigned>(FgrCountRead(f.rel_resetcb));
    cur.rel_begin = static_cast<unsigned>(FgrCountRead(f.rel_begin));
    cur.rel_end = static_cast<unsigned>(FgrCountRead(f.rel_end));
    cur.rel_submit = static_cast<unsigned>(FgrCountRead(f.rel_submit));
    cur.watchdog   = static_cast<unsigned>(FgrCountRead(f.eval_watchdog));
    cur.cfseq      = static_cast<unsigned>(FgrCountRead(f.cf_seq));
    cur.cfidle     = static_cast<unsigned>(FgrCountRead(f.cf_idle));
    cur.cfnr       = static_cast<unsigned>(FgrCountRead(f.cf_nrfence));
    cur.slotbusy   = static_cast<unsigned>(FgrCountRead(f.slot_busy));
    cur.copybusy   = static_cast<unsigned>(FgrCountRead(f.busy_copyfence));
    cur.nrgpuwait  = static_cast<unsigned>(FgrCountRead(f.nr_gpuwait));
    cur.flips      = static_cast<unsigned>(FgrCountRead(f.flips));
    cur.wplays     = static_cast<unsigned>(FgrCountRead(f.w_plays));
    cur.wok        = static_cast<unsigned>(FgrCountRead(f.w_ok));
    cur.wfail      = static_cast<unsigned>(FgrCountRead(f.w_fail));
    cur.wnr        = static_cast<unsigned>(FgrCountRead(f.w_nr_absent));
    cur.wcopyfail  = static_cast<unsigned>(FgrCountRead(f.w_copy_fail));

    const double dt = static_cast<double>(elapsed) / 1000.0;
    auto ps = [dt](unsigned v) { return static_cast<double>(v) / dt; };
    LARGE_INTEGER qpc_frequency = {};
    QueryPerformanceFrequency(&qpc_frequency);
    const double qpc_to_ms = qpc_frequency.QuadPart > 0
        ? 1000.0 / static_cast<double>(qpc_frequency.QuadPart) : 0.0;
    auto ms = [qpc_to_ms](LONG64 ticks) { return static_cast<double>(ticks) * qpc_to_ms; };
    auto avg_ms = [ms](LONG64 sum, unsigned n) { return n != 0 ? ms(sum) / n : 0.0; };

    Log("[fg-relay] rate/%.1fs p%d import_opt=%d: presents=%.1f/s "
        "present-gate[point=%.1f ready=%.1f fallback=%.1f inactive=%.1f]/s "
        "noinfo=%.1f/s stale=%.1f/s unlearned=%.1f/s attempts=%.1f/s armed=%.1f/s skipped=%.1f/s busy=%.1f/s "
        "arm-reject[state=%.1f request=%.1f refused=%.1f stopping=%.1f res=%.1f size=%.1f]/s "
        "pool[cap=%.1f family=%.1f create=%.1f alloc=%.1f]/s "
        "copy-drop[fence=%.1f reset=%.1f begin=%.1f end=%.1f submit=%.1f]/s "
        "cf[seq=%.1f/s idle=%.1f/s nrfence=%.1f/s] slotbusy=%.1f/s copyfence-busy=%.1f/s "
        "nr-gpuwait=%.1f/s flips=%.1f/s",
        dt, PresentAdapterPipeline(), PresentAdapterRelayImport() ? 1 : 0,
        ps(cur.presents - prev.presents), ps(cur.p_pipeline_skip - prev.p_pipeline_skip),
        ps(cur.p_notready - prev.p_notready), ps(cur.p_fallback - prev.p_fallback),
        ps(cur.p_inactive - prev.p_inactive), ps(cur.noinfo - prev.noinfo),
        ps(cur.stale - prev.stale), ps(cur.unlearned - prev.unlearned),
        ps(cur.attempts - prev.attempts), ps(cur.armed - prev.armed),
        ps(cur.skipped - prev.skipped), ps(cur.busy - prev.busy),
        ps(cur.arm_guard - prev.arm_guard), ps(cur.arm_noreq - prev.arm_noreq),
        ps(cur.arm_refused - prev.arm_refused), ps(cur.arm_stopping - prev.arm_stopping),
        ps(cur.arm_badres - prev.arm_badres), ps(cur.arm_badsize - prev.arm_badsize),
        ps(cur.pool_cap - prev.pool_cap), ps(cur.pool_family - prev.pool_family),
        ps(cur.pool_create - prev.pool_create), ps(cur.pool_alloc - prev.pool_alloc),
        ps(cur.drop_fence - prev.drop_fence), ps(cur.drop_resetcb - prev.drop_resetcb),
        ps(cur.drop_begin - prev.drop_begin), ps(cur.drop_end - prev.drop_end),
        ps(cur.drop_submit - prev.drop_submit),
        ps(cur.cfseq - prev.cfseq), ps(cur.cfidle - prev.cfidle), ps(cur.cfnr - prev.cfnr),
        ps(cur.slotbusy - prev.slotbusy), ps(cur.copybusy - prev.copybusy),
        ps(cur.nrgpuwait - prev.nrgpuwait), ps(cur.flips - prev.flips));

    Log("[fg-relay] rate2: fginput eval=%.1f/s index!=1=%.1f/s "
        "eval-gate[off=%.1f fallback=%.1f notready=%.1f noinput=%.1f noqueue=%.1f]/s "
        "relay eval=%.1f/s consume=%.1f/s inplace=%.1f/s(ok %.1f/s) unarmed=%.1f/s mismatch=%.1f/s "
        "relbusy=%.1f/s relok=%.1f/s relfail=%.1f/s "
        "release-stage[queue=%.1f pool=%.1f fence=%.1f reset=%.1f begin=%.1f end=%.1f submit=%.1f]/s watchdog=%.1f/s "
        "| worker plays=%.1f/s ok=%.1f/s fail=%.1f/s nr_absent=%.1f/s copyfail=%.1f/s",
        ps(cur.input_evals - prev.input_evals), ps(cur.index_skipped - prev.index_skipped),
        ps(cur.eval_disabled - prev.eval_disabled), ps(cur.eval_fallback - prev.eval_fallback),
        ps(cur.eval_notready - prev.eval_notready), ps(cur.eval_noinput - prev.eval_noinput),
        ps(cur.eval_noqueue - prev.eval_noqueue),
        ps(cur.evals - prev.evals), ps(cur.consume - prev.consume), ps(cur.inplace - prev.inplace),
        ps(cur.inplace_ok - prev.inplace_ok), ps(cur.unarmed - prev.unarmed), ps(cur.mismatch - prev.mismatch),
        ps(cur.relbusy - prev.relbusy), ps(cur.relok - prev.relok), ps(cur.relfail - prev.relfail),
        ps(cur.rel_noqueue - prev.rel_noqueue), ps(cur.rel_nopool - prev.rel_nopool),
        ps(cur.rel_resetfence - prev.rel_resetfence), ps(cur.rel_resetcb - prev.rel_resetcb),
        ps(cur.rel_begin - prev.rel_begin), ps(cur.rel_end - prev.rel_end), ps(cur.rel_submit - prev.rel_submit),
        ps(cur.watchdog - prev.watchdog), ps(cur.wplays - prev.wplays), ps(cur.wok - prev.wok),
        ps(cur.wfail - prev.wfail), ps(cur.wnr - prev.wnr), ps(cur.wcopyfail - prev.wcopyfail));

    Log("[fg-relay] rate3: arm->arm avg=%.2fms max=%.2fms n=%u | arm->work avg=%.2fms max=%.2fms | arm->eval avg=%.2fms max=%.2fms "
        "| copywait avg=%.3fms max=%.3fms | copy-land<= avg=%.2fms max=%.2fms | work avg=%.2fms max=%.2fms "
        "| totals armed=%u unarmed=%u skipped=%u busy=%u mismatch=%u copies_failed=%u writes=%u conflicts=%u",
        avg_ms(FgrCountRead64(f.arm_gap_sum), static_cast<unsigned>(FgrCountRead(f.arm_gap_n))),
        ms(FgrCountRead64(f.arm_gap_max)), static_cast<unsigned>(FgrCountRead(f.arm_gap_n)),
        avg_ms(FgrCountRead64(f.arm2work_sum), static_cast<unsigned>(FgrCountRead(f.arm2work_n))),
        ms(FgrCountRead64(f.arm2work_max)),
        avg_ms(FgrCountRead64(f.arm2eval_sum), static_cast<unsigned>(FgrCountRead(f.arm2eval_n))),
        ms(FgrCountRead64(f.arm2eval_max)),
        avg_ms(FgrCountRead64(f.copywait_sum), static_cast<unsigned>(FgrCountRead(f.copywait_n))),
        ms(FgrCountRead64(f.copywait_max)),
        avg_ms(FgrCountRead64(f.copyland_sum), static_cast<unsigned>(FgrCountRead(f.copyland_n))),
        ms(FgrCountRead64(f.copyland_max)),
        avg_ms(FgrCountRead64(f.work_sum), static_cast<unsigned>(FgrCountRead(f.work_n))),
        ms(FgrCountRead64(f.work_max)),
        static_cast<unsigned>(FgrCountRead(f.armed)), static_cast<unsigned>(FgrCountRead(f.unarmed)),
        static_cast<unsigned>(FgrCountRead(f.skipped)), static_cast<unsigned>(FgrCountRead(f.busy)),
        static_cast<unsigned>(FgrCountRead(f.mismatched)), static_cast<unsigned>(FgrCountRead(f.copies_failed)),
        static_cast<unsigned>(FgrCountRead(f.writes)), static_cast<unsigned>(FgrCountRead(f.idx_conflicts)));

    static unsigned prev_calls[2], prev_waits[2], prev_clear[2], prev_timeouts[2];
    static LONG64 prev_sum[2];
    char site_text[2][160] = {};
    for (unsigned s = 0; s < 2; ++s) {
        const unsigned calls = static_cast<unsigned>(FgrCountRead(f.thr_calls[s]));
        const unsigned waits = static_cast<unsigned>(FgrCountRead(f.thr_waits[s]));
        const unsigned clear = static_cast<unsigned>(FgrCountRead(f.thr_clear[s]));
        const unsigned timeouts = static_cast<unsigned>(FgrCountRead(f.thr_timeouts[s]));
        const LONG64 sum = FgrCountRead64(f.thr_sum[s]);
        sprintf_s(site_text[s], "calls=%.1f/s waits=%.1f/s clear=%.1f/s timeouts=%.1f/s avg=%.2fms",
            ps(calls - prev_calls[s]), ps(waits - prev_waits[s]), ps(clear - prev_clear[s]),
            ps(timeouts - prev_timeouts[s]), avg_ms(sum - prev_sum[s], waits - prev_waits[s]));
        prev_calls[s] = calls; prev_waits[s] = waits; prev_clear[s] = clear; prev_timeouts[s] = timeouts; prev_sum[s] = sum;
    }
    Log("[fg-relay] rate4: throttle depth=%d off=%ld | sleep %s | present %s | max=%.2fms (process)",
        PresentAdapterRelayThrottle(), static_cast<long>(InterlockedCompareExchange(&f.thr_off, 0, 0)),
        site_text[kFgrAtSleep], site_text[kFgrAtPresent], ms(FgrCountRead64(f.thr_max)));

    prev = cur;
}

// ---------------------------------------------------------------------------
// The present hook half (Pipeline=3)
// ---------------------------------------------------------------------------

struct FgrPresentInfo
{
    uint32_t sType; const void *pNext;
    uint32_t waitSemaphoreCount; const uint64_t *pWaitSemaphores;
    uint32_t swapchainCount; const uint64_t *pSwapchains; const uint32_t *pImageIndices; void *pResults;
};

// Back-pressure for the stage Reflex cannot see: held until the shared fence shows
// the NR of the newest armed frame (less Throttle-1) completed. That NR is already
// queued or about to be (the worker queues it without waiting on anything the
// game does next), so the wait cannot depend on the waiting thread; it is bounded
// anyway, and a throttle that keeps timing out turns itself off for the process.
// The fence is referenced under the lock because the session release frees it
// under the lock.
//
// Where it waits decides what it buys. At the game's Reflex sleep the wait lands
// before the frame's simulation starts, so the input is sampled after it -- the
// latency goes. At the game's present the frame has already been simulated, so
// the wait only moves the queue from the GPU into the frame's own latency
// (2026-09-29: PCL 246 -> 209 ms, against 132 ms for a frame cap that sleeps
// before simulation). The present site is therefore only the fallback for a game
// that does not call slReflexSleep; it stands down while the sleep site is live.
static const DWORD    kFgrThrottleMs = 100;
static const unsigned kFgrThrottleGiveUp = 5;
static void FgRelayThrottle(unsigned site)
{
    FgrState &f = g_fgr;
    const int depth = PresentAdapterRelayThrottle();
    if (depth <= 0 || !f.cs_ready) return;
    FgrCount(f.thr_calls[site]);
    const LONG64 now_ms = static_cast<LONG64>(GetTickCount64());
    if (site == kFgrAtSleep) InterlockedExchange64(&f.last_sleep_ms, now_ms);
    else {
        const LONG64 last_sleep = FgrCountRead64(f.last_sleep_ms);
        if (last_sleep != 0 && now_ms - last_sleep < 500) return;
    }
    if (!f.ready || f.fell_back || !f.import_ok || f.thr_ev[site] == nullptr ||
        InterlockedCompareExchange(&f.thr_off, 0, 0) != 0)
        return;
    FgrLock();
    ID3D12Fence *fence = f.nr_fence12;
    const uint64_t newest = f.nr_val;
    if (fence != nullptr) fence->AddRef();
    FgrUnlock();
    if (fence == nullptr) return;
    const uint64_t back = static_cast<uint64_t>(depth - 1);
    const uint64_t target = newest > back ? newest - back : 0;
    if (target == 0 || fence->GetCompletedValue() >= target) {
        fence->Release();
        FgrCount(f.thr_clear[site]);
        return;
    }
    const LONG64 start = FgrQpcNow();
    bool done = false;
    // The event is auto-reset and shared by every wait on this site, so a wake
    // proves nothing: a registration left behind by an earlier timeout fires it
    // late, and from then on every wait would return at once, one target behind,
    // forever (2026-09-29, first sleep-site run). Only the fence value decides.
    if (SUCCEEDED(fence->SetEventOnCompletion(target, f.thr_ev[site]))) {
        const ULONGLONG deadline = GetTickCount64() + kFgrThrottleMs;
        for (;;) {
            if (fence->GetCompletedValue() >= target) { done = true; break; }
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) break;
            WaitForSingleObject(f.thr_ev[site], static_cast<DWORD>(deadline - now));
        }
    }
    fence->Release();
    const LONG64 waited = FgrQpcNow() - start;
    FgrCount(f.thr_waits[site]);
    FgrAdd64(f.thr_sum[site], waited);
    FgrMax64(f.thr_max, waited);
    if (done) { InterlockedExchange(&f.thr_streak, 0); return; }
    FgrCount(f.thr_timeouts[site]);
    if (static_cast<unsigned>(InterlockedIncrement(&f.thr_streak)) >= kFgrThrottleGiveUp &&
        InterlockedExchange(&f.thr_off, 1) == 0)
        Log("[fg-relay] throttle: the NR fence missed %u consecutive %lu ms waits; the present throttle is off "
            "for this process (frames may queue ahead of the NR again).", kFgrThrottleGiveUp, kFgrThrottleMs);
}

// Called from the interposer's vkQueuePresentKHR detour, on the thread that owns
// the present queue, before the original runs -- the copy has to be in the queue
// before the present operation is.
static void FgRelayOnPresent(void *queue, const void *info_raw)
{
    FgrState &f = g_fgr;
    // Reached on every present in both relay pipelines (the hook calls here
    // whenever Pipeline=2 or 3), which is what makes this the reporting point.
    FgRelayPeriodic();
    if (f.cs_ready) FgrCount(f.presents);
    FgRelayThrottle(kFgrAtPresent);
    if (!PresentAdapterRelayAtPresent()) { FgrCount(f.p_pipeline_skip); return; }
    if (!f.ready) { FgrCount(f.p_notready); return; }
    if (f.fell_back) { FgrCount(f.p_fallback); return; }
    if (!f.cs_ready) return;
    if (!FgInputActive()) { FgrCount(f.p_inactive); return; }
    const FgrPresentInfo *info = static_cast<const FgrPresentInfo *>(info_raw);
    if (info == nullptr || info->swapchainCount == 0 || info->pImageIndices == nullptr) { FgrCount(f.p_noinfo); return; }

    // Remember the present whatever else happens: the map from image index to
    // back buffer handle is learned at the evaluate from it.
    FgrLock();
    f.have_present = true;
    f.present_idx = info->pImageIndices[0];
    const uint64_t back = f.present_idx < _countof(f.idx_map) ? f.idx_map[f.present_idx] : 0;
    const bool have_last = f.have_last;
    const DWORD age = static_cast<DWORD>(GetTickCount()) - static_cast<DWORD>(f.last_eval_tick);
    VkmRes hud = {}, dep = {}, mvv = {};
    hud.image = f.last_hud; hud.w = f.last_w; hud.h = f.last_h; hud.fmt = f.last_fmt;
    dep.image = f.last_depth; dep.w = f.last_w; dep.h = f.last_h;
    mvv.image = f.last_mv;   mvv.w = f.last_w;  mvv.h = f.last_h;
    const unsigned depth_inverted = f.last_depth_inverted;
    const float msx = f.last_mv_scale_x, msy = f.last_mv_scale_y;
    FgrUnlock();

    // Only while the game is actually evaluating frames. During menus and loads
    // presents keep arriving with no FG groups behind them, and a copy plus NR
    // per present would be pure cost.
    if (!have_last || age > 400) { FgrCount(f.p_stale); return; }
    if (back == 0) { FgrCount(f.unarmed); FgrCount(f.p_unlearned); return; }      // this index is not learned yet
    VkmRes bk = {}; bk.image = back; bk.w = hud.w; bk.h = hud.h; bk.fmt = hud.fmt;
    FgRelayArmCopy(queue, "present", hud, bk, dep, mvv, depth_inverted, msx, msy);
}

// ---------------------------------------------------------------------------
// The evaluate half
// ---------------------------------------------------------------------------

// The release batch: one vkCmdSetEvent, submitted with a submit-boundary wait on
// the imported release semaphore. A semaphore wait cannot be recorded inside a
// command buffer, which is the whole reason this exists as its own batch; the
// park it releases is a vkCmdWaitEvents in Streamline's, and an event that is
// already set satisfies that wait (the property the serial feed's own release
// has relied on since fginput2).
// Last resort for a D3D12 side that stopped signalling: the host sets the shared
// fence to the newest value anything on the game's queue may be waiting for, so
// the release batches and the copy batches waiting on nr_sem drain instead of
// holding the queue until a TDR.
static void FgRelayReleaseNrWaits()
{
    FgrState &f = g_fgr;
    if (f.nr_fence12 == nullptr) return;
    FgrLock();
    const uint64_t newest = f.nr_val;
    FgrUnlock();
    if (newest != 0 && f.nr_fence12->GetCompletedValue() < newest) f.nr_fence12->Signal(newest);
}

static bool FgRelaySubmitRelease(unsigned slot, uint64_t nr_val)
{
    FgrState &f = g_fgr;
    if (f.cmd_queue == nullptr) { FgrCount(f.rel_noqueue); return false; }
    FgrPool *pool = FgRelayPoolFor(f.cmd_queue, true);
    if (pool == nullptr || pool->cb_release[slot] == nullptr) { FgrCount(f.rel_nopool); return false; }
    const SVkCommandBuffer cb = pool->cb_release[slot];
    // The caller cleared release_in_flight[slot]: the fence was observed
    // signalled, or the previous submit never entered the queue.
    if (f.ResetFences(g_svk.dev, 1, &f.release_fence[slot]) != kSVkSuccess) { FgrCount(f.rel_resetfence); return false; }
    if (f.ResetCommandBuffer(cb, 0) != kSVkSuccess) { FgrCount(f.rel_resetcb); return false; }
    FgrCommandBufferBeginInfo bi = {};
    bi.sType = kFgrStCommandBufferBegin;
    bi.flags = kFgrCbOneTimeSubmit;
    if (f.BeginCommandBuffer(cb, &bi) != kSVkSuccess) { FgrCount(f.rel_begin); return false; }
    g_svk.CmdSetEvent(cb, f.ev_out[slot], kSVkStageAllCommands);
    if (f.EndCommandBuffer(cb) != kSVkSuccess) { FgrCount(f.rel_end); return false; }

    uint64_t wait_value = nr_val;
    FgrTimelineSubmitInfo ts = {};
    ts.sType = kFgrStTimelineSubmit;
    ts.waitValueCount = 1;
    ts.pWaitValues = &wait_value;
    uint32_t stage = kSVkStageAllCommands;
    FgrSubmitInfo si = {};
    si.sType = kFgrStSubmitInfo;
    si.pNext = &ts;
    si.waitCount = 1;
    si.pWaits = &f.nr_sem;
    si.pWaitStages = &stage;
    si.cbCount = 1;
    si.pCbs = &cb;
    const bool submitted = f.QueueSubmit(reinterpret_cast<SVkQueue>(f.cmd_queue), 1, &si, f.release_fence[slot]) == kSVkSuccess;
    if (!submitted) FgrCount(f.rel_submit);
    return submitted;
}

// Called from the FG-input feed's evaluate detour, under g_bridge_cs, on
// Streamline's present thread -- the thread that owns DLSSG.CmdQueue.
//   1  the relay recorded the park and the copy back for this frame
//   0  the relay has nothing for this frame; the serial feed may run
//  -1  a relay frame is in flight or the armed copy did not match; skip the frame
static int FgRelayEvaluate(void *cmd_raw, const NVSDK_NGX_Parameter *p)
{
    FgrState &f = g_fgr;
    if (!PresentAdapterRelay() || !f.cs_ready) { FgrCount(f.eval_disabled); return 0; }
    if (f.fell_back) {
        FgrCount(f.eval_fallback);
        // An arm that was published as the relay stood down is consumed here so
        // the sequence numbers converge; the worker drains it, and the serial feed
        // below waits idle before it touches the staging images.
        if (f.ready) { FgrLock(); f.eval_seq = f.arm_seq; FgrUnlock(); }
        return 0;
    }
    FgRelayEnsure();
    if (!f.ready) { FgrCount(f.eval_notready); return 0; }
    FgrCount(f.evals);

    if (p != nullptr) {
        void *q = nullptr;
        if (p->Get("DLSSG.CmdQueue", &q) == NGX_SUCCESS && q != nullptr) f.cmd_queue = q;
    }

    // A park whose release never arrived is a hang, and the release lives on the
    // D3D12 queue now. If the shared fence is behind the last release and has not
    // moved for well past any legitimate NR, release every park and every GPU
    // wait by hand and stand the relay down; the serial feed's own releases are
    // unaffected. Measured from the fence's last progress, not from the last
    // release: frames keep being armed while the copies wait on the GPU, so a
    // fresh release every frame says nothing about the D3D12 side.
    bool nr_stalled = false;
    if (f.import_ok && f.nr_fence12 != nullptr && f.last_release_nr != 0) {
        const uint64_t done = f.nr_fence12->GetCompletedValue();
        const LONG now = static_cast<LONG>(GetTickCount());
        if (done >= f.last_release_nr || done != f.nr_progress_value) {
            f.nr_progress_value = done;
            f.nr_progress_tick = now;
        } else nr_stalled = static_cast<DWORD>(now - f.nr_progress_tick) > 1500;
    }
    if (nr_stalled) {
        FgrLock();
        for (unsigned i = 0; i < kFgrSlots; ++i)
            if (f.ev_out[i] != 0 && g_svk.EvSet != nullptr) g_svk.EvSet(g_svk.dev, f.ev_out[i]);
        FgrUnlock();
        FgRelayReleaseNrWaits();
        FgrCount(f.eval_watchdog);
        FgRelayFallback("a relay frame's release did not arrive within 1.5 s");
        return -1;
    }

    const char *why = nullptr;
    VkmRes hud = {}, back = {}, dep = {}, mvv = {};
    const bool have = VkmGetRes(p, "DLSSG.HUDLess", &hud, &why) &&
                      VkmGetRes(p, "DLSSG.Backbuffer", &back, &why) &&
                      VkmGetRes(p, "DLSSG.Depth", &dep, &why) &&
                      VkmGetRes(p, "DLSSG.MVecs", &mvv, &why);
    unsigned depth_inverted = 0;
    float mv_scale_x = 1.0f, mv_scale_y = 1.0f;
    if (have) {
        FgInputReadU(p, "DLSSG.DepthInverted", &depth_inverted);
        p->Get("DLSSG.MvecScaleX", &mv_scale_x);
        p->Get("DLSSG.MvecScaleY", &mv_scale_y);
        // Learned on every evaluate, armed or not: the present hook needs the
        // handles and the image-index map before it can arm the first frame.
        FgrLock();
        f.have_last = true;
        f.last_hud = hud.image; f.last_depth = dep.image; f.last_mv = mvv.image;
        f.last_w = back.w; f.last_h = back.h; f.last_fmt = back.fmt;
        f.last_depth_inverted = depth_inverted;
        f.last_mv_scale_x = mv_scale_x; f.last_mv_scale_y = mv_scale_y;
        f.last_eval_tick = static_cast<LONG>(GetTickCount());
        if (f.have_present && f.present_idx < _countof(f.idx_map)) {
            if (f.idx_map[f.present_idx] != 0 && f.idx_map[f.present_idx] != back.image) FgrCount(f.idx_conflicts);
            f.idx_map[f.present_idx] = back.image;
        }
        FgrUnlock();
    }

    LONG arm_seq = InterlockedCompareExchange(&f.arm_seq, 0, 0);
    LONG eval_seq = InterlockedCompareExchange(&f.eval_seq, 0, 0);

    FgrArm a = {};
    bool armed = false;
    if (arm_seq != eval_seq) {
        // A copy was submitted for this frame. Consume it whether or not it can
        // be used, so a mismatch cannot be consumed twice.
        FgrCount(f.eval_consume);
        FgrLock();
        const unsigned slot = static_cast<unsigned>(InterlockedCompareExchange(&f.pending_slot, 0, 0));
        a = f.arm[slot];
        a.slot = slot;
        f.eval_seq = arm_seq;
        FgrUnlock();
        if (a.valid) {
            const LONG64 arm_to_eval_ticks = FgrQpcNow() - a.tick;
            FgrAdd64(f.arm2eval_sum, arm_to_eval_ticks);
            FgrCount(f.arm2eval_n);
            FgrMax64(f.arm2eval_max, arm_to_eval_ticks);
        }
        armed = a.valid;
    } else if (PresentAdapterRelayAtEvaluate()) {
        if (!have) FgrCount(f.eval_noinput);
        else if (f.cmd_queue == nullptr) FgrCount(f.eval_noqueue);
        else {
            // Pipeline=2: the copy happens here, on the queue this thread owns, and
            // the frame is armed with the handles the evaluate itself carries.
            FgrCount(f.eval_inplace);
            if (FgRelayArmCopy(f.cmd_queue, "evaluate", hud, back, dep, mvv, depth_inverted, mv_scale_x, mv_scale_y)) {
                FgrCount(f.eval_inplace_ok);
                arm_seq = InterlockedCompareExchange(&f.arm_seq, 0, 0);
                FgrLock();
                const unsigned slot = static_cast<unsigned>(InterlockedCompareExchange(&f.pending_slot, 0, 0));
                a = f.arm[slot];
                a.slot = slot;
                f.eval_seq = arm_seq;
                FgrUnlock();
                armed = a.valid;
            }
        }
    }

    // flips counts groups whose NR state differs from the group before: a group
    // served with NR followed by one on the game's own image, or the reverse.
    auto note_group = [&f](bool served) {
        if (f.have_last_group && f.last_group_served != served) FgrCount(f.flips);
        f.have_last_group = true;
        f.last_group_served = served;
    };

    if (!armed) {
        // Once the relay is open it owns every frame: the serial feed writes the
        // same contract and staging images the worker may be using for an armed
        // frame, so it must not run alongside. A frame the relay could not arm
        // is frame generation's own image for one group, and is counted. A relay
        // that keeps failing to arm stands down so the serial feed's NR returns.
        note_group(false);
        const LONG unarmed = FgrCount(f.unarmed);
        FgrCount(f.eval_unarmed);
        if (unarmed <= 3 || unarmed % 600 == 0)
            Log("[fg-relay] frame not armed (unarmed=%u skipped=%u busy=%u): FG runs on the game's own image this group.",
                static_cast<unsigned>(unarmed), static_cast<unsigned>(FgrCountRead(f.skipped)),
                static_cast<unsigned>(FgrCountRead(f.busy)));
        if (++f.unarmed_streak >= 120)
            FgRelayFallback("120 consecutive frames could not be armed");
        return -1;
    }
    f.unarmed_streak = 0;

    // From here on the arm is consumed. A frame that cannot record its park frees
    // the slot again: no command buffer will ever set its ev_mark.
    const unsigned slot = a.slot;
    auto drop_slot = [&]() { FgrLock(); f.slot_used[slot] = false; FgrUnlock(); };

    if (!have || hud.image != a.hud || back.image != a.back || dep.image != a.depth ||
        mvv.image != a.mv || back.w != a.w || back.h != a.h || back.fmt != a.vk_fmt) {
        note_group(false);
        const LONG mismatched = FgrCount(f.mismatched);
        drop_slot();
        if (mismatched <= 3)
            Log("[fg-relay] armed copy does not match this evaluate: hud %llx/%llx back %llx/%llx depth %llx/%llx mv %llx/%llx.",
                a.hud, hud.image, a.back, back.image, a.depth, dep.image, a.mv, mvv.image);
        if (++f.mismatch_streak >= 8)
            FgRelayFallback("the armed copy's inputs and the evaluate's inputs kept disagreeing");
        return -1;
    }
    f.mismatch_streak = 0;

    // The slot's release batch is re-recorded when the slot comes round again, so
    // its previous release must be over before its fence is touched. A failed
    // submit left nothing in flight, so it retries here instead of blocking.
    if (f.import_ok && f.release_in_flight[slot]) {
        if (f.GetFenceStatus(g_svk.dev, f.release_fence[slot]) != kSVkSuccess) {
            note_group(false);
            FgrCount(f.busy);
            FgrCount(f.eval_relbusy);
            drop_slot();
            return -1;
        }
        f.release_in_flight[slot] = false;
    }

    // The park. The slot's events were reset at the arm, before the worker was
    // woken; an event that is already set satisfies this wait, so the host-side
    // release may run before or after this is recorded.
    SVkCommandBuffer cb = reinterpret_cast<SVkCommandBuffer>(cmd_raw);
    {
        SVkMemoryBarrier mb = {};
        mb.sType = kSVkStMemoryBarrier;
        mb.srcAccessMask = kSVkAccessMemoryRead | kSVkAccessMemoryWrite;
        mb.dstAccessMask = kSVkAccessMemoryRead | kSVkAccessMemoryWrite;
        const SVkHandle ev = f.ev_out[slot];
        g_svk.CmdWaitEvents(cb, 1, &ev, kSVkStageHost, kSVkStageAllCommands, 1, &mb, 0, nullptr, 0, nullptr);
    }
    // ev_mark is set right after the park: the next frame that wants this slot
    // proves the park passed before it may reset the events again.
    g_svk.CmdSetEvent(cb, f.ev_mark[slot], kSVkStageAllCommands);
    if (g_cfg.stage >= 3) {
        SynthVkBarrier(cb, g_svk.img[SLOT_OUTPUT], kSVkLayoutGeneral, kSVkLayoutTransferSrc);
        SynthVkBarrier(cb, g_svk.img[kOutB], kSVkLayoutGeneral, kSVkLayoutTransferSrc);
        SynthVkBarrier(cb, hud.image, kSVkLayoutGeneral, kSVkLayoutTransferDst);
        SynthVkBarrier(cb, back.image, kSVkLayoutGeneral, kSVkLayoutTransferDst);
        SynthVkCopyImage(cb, g_svk.img[SLOT_OUTPUT], kSVkLayoutTransferSrc, hud.image, kSVkLayoutTransferDst, back.w, back.h);
        SynthVkCopyImage(cb, g_svk.img[kOutB], kSVkLayoutTransferSrc, back.image, kSVkLayoutTransferDst, back.w, back.h);
        SynthVkBarrier(cb, hud.image, kSVkLayoutTransferDst, kSVkLayoutGeneral);
        SynthVkBarrier(cb, back.image, kSVkLayoutTransferDst, kSVkLayoutGeneral);
        SynthVkBarrier(cb, g_svk.img[SLOT_OUTPUT], kSVkLayoutTransferSrc, kSVkLayoutGeneral);
        SynthVkBarrier(cb, g_svk.img[kOutB], kSVkLayoutTransferSrc, kSVkLayoutGeneral);
    }

    if (f.import_ok) {
        if (FgRelaySubmitRelease(slot, a.nr_val)) {
            FgrCount(f.rel_ok);
            f.release_in_flight[slot] = true;
            // The first release of a session arms the watchdog, so its clock
            // starts here. The session release zeroes last_release_nr but not the
            // tick, and a tick left from before a hook-point switch read as a
            // fence stalled for the whole time the relay was closed (2026-09-29:
            // the relay stood down 0.2 s after reopening).
            if (f.last_release_nr == 0) {
                f.nr_progress_value = f.nr_fence12 != nullptr ? f.nr_fence12->GetCompletedValue() : 0;
                f.nr_progress_tick = static_cast<LONG>(GetTickCount());
            }
            f.last_release_nr = a.nr_val;
        } else {
            FgrCount(f.rel_fail);
            // The park is recorded and nothing on the GPU will release it: let it
            // through now (this frame may be torn) rather than hang the queue.
            FgrLock();
            if (g_svk.EvSet != nullptr) g_svk.EvSet(g_svk.dev, f.ev_out[slot]);
            FgrUnlock();
            Log("[fg-relay] release batch could not be submitted; the park was released by the host.");
        }
    }
    note_group(true);
    if (g_present_adapter.remaining != 0) --g_present_adapter.remaining;
    const unsigned armed_total = static_cast<unsigned>(FgrCountRead(f.armed));
    if (armed_total <= 3 || armed_total % 1800 == 0)
        Log("[fg-relay] evaluate: slot=%u nr=%llu import=%d stage=%d.",
            slot, a.nr_val, f.import_ok ? 1 : 0, g_cfg.stage);
    return 1;
}

// ---------------------------------------------------------------------------
// The worker half
// ---------------------------------------------------------------------------

// Read by the worker loop before it polls the serial feed's ev_in: a relay frame
// has no ev_in, its completion order is the copy fence / the shared semaphore.
// Deliberately not gated on fell_back: a frame armed before the relay stood down
// still has to be consumed, or idle would never be set again and the serial feed
// would refuse on its own idle wait.
static bool FgRelayPending()
{
    FgrState &f = g_fgr;
    if (!f.cs_ready || !f.ready) return false;
    return InterlockedCompareExchange(&f.arm_seq, 0, 0) != InterlockedCompareExchange(&f.worker_seq, 0, 0);
}

// One relay frame, on the worker thread. The NR is queued and the release is
// signalled; nothing here waits for the GPU when the import is live, which is
// what makes the release fire the moment the D3D12 work completes instead of the
// moment a host thread notices.
static bool FgRelayWorker()
{
    FgrState &f = g_fgr;
    const LONG64 work_start = FgrQpcNow();
    FgrCount(f.w_plays);
    const LONG seq = InterlockedCompareExchange(&f.arm_seq, 0, 0);
    FgrLock();
    const unsigned slot = static_cast<unsigned>(InterlockedCompareExchange(&f.pending_slot, 0, 0));
    const FgrArm a = f.arm[slot];
    f.worker_seq = seq;
    FgrUnlock();

    if (a.valid) {
        const LONG64 arm_to_work_ticks = FgrQpcNow() - a.tick;
        FgrAdd64(f.arm2work_sum, arm_to_work_ticks);
        FgrCount(f.arm2work_n);
        FgrMax64(f.arm2work_max, arm_to_work_ticks);
    }

    // The private device has to see the copies completed before it reads the
    // staging images. With the import live this is a wait the queue performs;
    // without it, a bounded host wait on the same fence the GPU would have
    // waited -- the copies are sub-millisecond work, so the wait is short by
    // construction and is bounded anyway. A copy that has not landed in time is
    // treated as a failed frame: nothing is read from it.
    const LONG64 copy_wait_start = FgrQpcNow();
    bool copy_seen = true;
    if (f.import_ok && f.copy_fence12 != nullptr)
        g_s12.queue->Wait(f.copy_fence12, a.copy_val);
    else if (f.WaitForFences != nullptr && f.copy_fence[slot] != 0)
        copy_seen = f.WaitForFences(g_svk.dev, 1, &f.copy_fence[slot], 1, 250ull * 1000000ull) == kSVkSuccess;
    const LONG64 copy_wait_ticks = FgrQpcNow() - copy_wait_start;
    FgrAdd64(f.copywait_sum, copy_wait_ticks);
    FgrCount(f.copywait_n);
    FgrMax64(f.copywait_max, copy_wait_ticks);

    bool ok = false, nr_absent = false, wrote = false;
    unsigned nr_entries = 0, sr_entries = 0, sr_copies = 0;
    if (!copy_seen) {
        FgrCount(f.copies_failed);
        FgrCount(f.w_copy_fail);
    } else if (g_present_adapter.run_nr) {
        g_present_nr_entries = g_present_sr_entries = g_present_sr_copies = 0;
        g_present_nr_scope = true;
        if (g_ngx_cs_ready) EnterCriticalSection(&g_ngx_cs);
        // No trailing wait: the release is signalled by the queue below and the
        // next frame's Synth12Begin waits the allocator's retire fence, which is
        // this submission's own fence value.
        ok = Synth12EvaluateShared(&g_vkm.sp, kVkmEvaluateMs, SLOT_OUTPUT, &wrote, false);
        if (g_ngx_cs_ready) LeaveCriticalSection(&g_ngx_cs);
        g_present_nr_scope = false;
        nr_entries = g_present_nr_entries; sr_entries = g_present_sr_entries; sr_copies = g_present_sr_copies;
        if (ok && (sr_entries != 1 || sr_copies != 1)) {
            Log("[fg-relay] carrier contract mismatch: sr_entries=%u sr_copies=%u; frame not written.", sr_entries, sr_copies);
            ok = false;
        }
        if (ok && nr_entries == 0) { nr_absent = true; ++g_fg_input.skipped_nr; FgrCount(f.w_nr_absent); }
        if (ok && !nr_absent) ok = FgCompositeRun(g_s12.queue, kVkmEvaluateMs, false);
    } else ok = PresentAdapterCopy12();
    ok = ok && g_s12.dev->GetDeviceRemovedReason() == S_OK;
    if (!ok) {
        // Keep the carriers equal to their inputs so the copy back already
        // recorded in the game's command buffer is a no-op.
        FgIdentityCopy(kVkmEvaluateMs);
    }

    // Signalled on every path. A frame whose NR failed still has a command
    // buffer parked on this value, and the identity copy above is what makes
    // letting it through harmless.
    if (f.nr_fence12 != nullptr) g_s12.queue->Signal(f.nr_fence12, a.nr_val);
    if (!f.import_ok) {
        // No shared object to signal through: the CPU stands in, waiting the
        // session's own fences -- the composite's, then the NR submission's --
        // and setting the park's event afterwards. Still no ev_in polling and no
        // dependency on the game's queue having reached anything.
        FgCompositeWait(kVkmEvaluateMs);
        Synth12Drain(kVkmEvaluateMs);
        if (g_svk.EvSet != nullptr) g_svk.EvSet(g_svk.dev, f.ev_out[slot]);
    }

    FgrLock();
    if (ok && wrote) FgrCount(f.writes);
    const unsigned n = static_cast<unsigned>(FgrCountRead(f.armed));
    FgrUnlock();
    if (ok) FgrCount(f.w_ok); else FgrCount(f.w_fail);
    const LONG64 work_ticks = FgrQpcNow() - work_start;
    FgrAdd64(f.work_sum, work_ticks);
    FgrCount(f.work_n);
    FgrMax64(f.work_max, work_ticks);
    if (n <= 3 || n % 1800 == 0 || !ok)
        Log("[fg-relay] frame=%u slot=%u nr=%llu ok=%d wrote=%d nr_entries=%u import=%d "
            "armed=%u skipped=%u busy=%u mismatch=%u conflicts=%u.",
            n, slot, a.nr_val, ok ? 1 : 0, wrote ? 1 : 0, nr_entries, f.import_ok ? 1 : 0,
            static_cast<unsigned>(FgrCountRead(f.armed)), static_cast<unsigned>(FgrCountRead(f.skipped)),
            static_cast<unsigned>(FgrCountRead(f.busy)), static_cast<unsigned>(FgrCountRead(f.mismatched)),
            static_cast<unsigned>(FgrCountRead(f.idx_conflicts)));
    return ok;
}

// ---------------------------------------------------------------------------
// Session lifetime
// ---------------------------------------------------------------------------

// The release paths tear the private session down; a relay frame that is still
// armed or still being consumed owns images and a parked command buffer, so the
// release has to refuse rather than race it. Same answer the serial feed gives
// when its own park has not cleared.
static bool FgRelayIdleForRelease()
{
    FgrState &f = g_fgr;
    if (!f.cs_ready || !f.ready) return true;
    const LONG armed = InterlockedCompareExchange(&f.arm_seq, 0, 0);
    return armed == InterlockedCompareExchange(&f.worker_seq, 0, 0) &&
           armed == InterlockedCompareExchange(&f.eval_seq, 0, 0);
}

// Every relay park released by hand. Only for the shutdown path, where the worker
// that would release them is about to be joined and the command buffer parked on
// an event is the game's own.
static void FgRelayForceReleaseParks()
{
    FgrState &f = g_fgr;
    if (!f.cs_ready || g_svk.EvSet == nullptr || g_svk.dev == nullptr) return;
    FgrLock();
    for (unsigned i = 0; i < kFgrSlots; ++i)
        if (f.ev_out[i] != 0) g_svk.EvSet(g_svk.dev, f.ev_out[i]);
    FgrUnlock();
}

static void FgRelayOnSessionRelease()
{
    FgrState &f = g_fgr;
    if (!f.cs_ready) return;
    // The objects this state names -- semaphores, fences, events, pools -- belong
    // to the session that is going away, and are left standing rather than
    // destroyed under work that may still name them. A session opened again on
    // the same device makes new ones; the old ones are a bounded leak on a path
    // the user walks by hand. The value counters restart with the new objects:
    // a fresh fence starts at zero, and a stale last_release_nr against it would
    // read as a release that never arrived.
    FgrLock();
    f.ready = false;
    f.tried = false;
    f.have_last = false;
    f.have_present = false;
    memset(f.idx_map, 0, sizeof(f.idx_map));
    f.arm_seq = f.worker_seq = f.eval_seq = 0;
    for (unsigned i = 0; i < kFgrSlots; ++i) { f.slot_used[i] = false; f.arm[i].valid = false; }
    f.npools = 0;
    memset(f.pools, 0, sizeof(f.pools));
    f.cmd_queue = nullptr;
    f.copy_val = f.nr_val = f.last_release_nr = f.nr_progress_value = 0;
    for (unsigned i = 0; i < kFgrSlots; ++i) {
        f.copy_in_flight[i] = f.release_in_flight[i] = false;
        InterlockedExchange64(&f.copy_submit_qpc[i], 0);
    }
    f.have_last_group = false;
    f.mismatch_streak = 0;
    // The caller has drained the D3D12 session and idled the VkDevice, so the
    // D3D12 views can go; the Vulkan objects stay (see above).
    if (f.copy_fence12 != nullptr) { f.copy_fence12->Release(); f.copy_fence12 = nullptr; }
    if (f.nr_fence12 != nullptr) { f.nr_fence12->Release(); f.nr_fence12 = nullptr; }
    FgrUnlock();
}
