// Native frame-generation input feed for Vulkan: the real, fully composited frame
// that DLSS-G interpolates from is neural-rendered once per real frame, before the
// FG evaluate is forwarded. FG then interpolates from processed frames, so the
// presented multiplier is preserved. Generic still runs through the identity SR
// carrier on the private D3D12 session; nothing here reads Generic's layout.
#include "fg-composite-shader.h"

static NVSDK_NGX_Handle *g_fg_handles[8];
static LONG g_fg_handle_count;

static void FgInputNoteCreated(int feature_id, NVSDK_NGX_Result r, NVSDK_NGX_Handle **out)
{
    if (r != NGX_SUCCESS || out == nullptr || *out == nullptr) return;
    for (LONG i = 0; i < g_fg_handle_count; ++i)
        if (g_fg_handles[i] == *out) { g_fg_handles[i] = g_fg_handles[--g_fg_handle_count]; break; }
    if (feature_id == 11 && g_fg_handle_count < static_cast<LONG>(_countof(g_fg_handles)))
        g_fg_handles[g_fg_handle_count++] = *out;
}

static bool FgInputIsFgHandle(const NVSDK_NGX_Handle *handle)
{
    for (LONG i = 0; i < g_fg_handle_count; ++i)
        if (g_fg_handles[i] == handle) return true;
    return false;
}

struct FgComposite {
    ID3D12RootSignature *rs;
    ID3D12PipelineState *pso;
    ID3D12DescriptorHeap *heap;
    ID3D12CommandAllocator *alloc;
    ID3D12GraphicsCommandList *list;
    ID3D12Fence *fence;
    HANDLE ev;
    UINT64 fv;
    ID3D12Resource *hudless, *nr, *back;
};
static FgComposite g_fg_composite;

struct FgInputState {
    bool refused, armed, worker_ok, want_release;
    uint32_t vk_fmt;
    UINT w, h;
    VkmRes hudless, back;
    unsigned frames, writes, skipped_nr, busy_skips;
    volatile LONG eval_calls, eval_index_skipped;
    unsigned long long token;
    unsigned census_left;
};
static FgInputState g_fg_input;

// Every key the game's sl.dlss_g.dll (Streamline 2.14.1) can place in the FG
// parameter block. A one-frame-delayed pipeline has to snapshot whichever of
// these change per frame, so the first evaluates log which type each key
// answers to and its value.
static const char *const kFgCensusKeys[] = {
    "DLSSG.AutomodeOverrideReset", "DLSSG.Backbuffer", "DLSSG.BackbufferFormat", "DLSSG.BackbufferFrameID",
    "DLSSG.BackbufferSubrectBaseX", "DLSSG.BackbufferSubrectBaseY", "DLSSG.BackbufferSubrectHeight",
    "DLSSG.BackbufferSubrectWidth", "DLSSG.BidirectionalDistortionField",
    "DLSSG.BidirectionalDistortionFieldLowPrecision.Bias", "DLSSG.BidirectionalDistortionFieldLowPrecision.IsLowPrecision",
    "DLSSG.BidirectionalDistortionFieldLowPrecision.Scale", "DLSSG.BidirectionalDistortionFieldSubrectBaseX",
    "DLSSG.BidirectionalDistortionFieldSubrectBaseY", "DLSSG.BidirectionalDistortionFieldSubrectHeight",
    "DLSSG.BidirectionalDistortionFieldSubrectWidth", "DLSSG.CameraAspectRatio", "DLSSG.CameraFOV", "DLSSG.CameraFar",
    "DLSSG.CameraFwdX", "DLSSG.CameraFwdY", "DLSSG.CameraFwdZ", "DLSSG.CameraMotionIncluded", "DLSSG.CameraNear",
    "DLSSG.CameraPinholeOffsetX", "DLSSG.CameraPinholeOffsetY", "DLSSG.CameraPosX", "DLSSG.CameraPosY", "DLSSG.CameraPosZ",
    "DLSSG.CameraRightX", "DLSSG.CameraRightY", "DLSSG.CameraRightZ", "DLSSG.CameraUpX", "DLSSG.CameraUpY", "DLSSG.CameraUpZ",
    "DLSSG.CameraViewToClip", "DLSSG.ClipToCameraView", "DLSSG.ClipToLensClip", "DLSSG.ClipToPrevClip", "DLSSG.CmdAlloc",
    "DLSSG.CmdQueue", "DLSSG.ColorBuffersHDR", "DLSSG.Depth", "DLSSG.DepthInverted", "DLSSG.DepthSubrectBaseX",
    "DLSSG.DepthSubrectBaseY", "DLSSG.DepthSubrectHeight", "DLSSG.DepthSubrectWidth", "DLSSG.Dynamic.ReflexDriven",
    "DLSSG.DynamicMaxFrameCount", "DLSSG.DynamicResolution", "DLSSG.EnableInterp", "DLSSG.EstimateVRAMCallback",
    "DLSSG.EvalFlags", "DLSSG.FenceEvent", "DLSSG.FlipMetering", "DLSSG.FullscreenMode", "DLSSG.HUDLess",
    "DLSSG.HUDLessSubrectBaseX", "DLSSG.HUDLessSubrectBaseY", "DLSSG.HUDLessSubrectHeight", "DLSSG.HUDLessSubrectWidth",
    "DLSSG.Height", "DLSSG.InternalHeight", "DLSSG.InternalWidth", "DLSSG.IsRecording", "DLSSG.JitterOffsetX",
    "DLSSG.JitterOffsetY", "DLSSG.MVecs", "DLSSG.MVecsSubrectBaseX", "DLSSG.MVecsSubrectBaseY", "DLSSG.MVecsSubrectHeight",
    "DLSSG.MVecsSubrectWidth", "DLSSG.MenuDetectionEnabled", "DLSSG.MinRelativeLinearDepthObjectSeparation",
    "DLSSG.ModelVersion", "DLSSG.MultiFrameCount", "DLSSG.MultiFrameCountMax", "DLSSG.MultiFrameIndex", "DLSSG.MvecDilated",
    "DLSSG.MvecInvalidValue", "DLSSG.MvecJittered", "DLSSG.MvecScaleX", "DLSSG.MvecScaleY", "DLSSG.NoWarp",
    "DLSSG.NoWarpSubrectBaseX", "DLSSG.NoWarpSubrectBaseY", "DLSSG.NoWarpSubrectHeight", "DLSSG.NoWarpSubrectWidth",
    "DLSSG.NotRenderingGameFrames", "DLSSG.NvAppOvrAppliedVal.MultiFrameCount", "DLSSG.NvAppOvrAppliedVal.StreamlineMode",
    "DLSSG.OrthoProjection", "DLSSG.OutputDisableInterpolation", "DLSSG.OutputInterpolated", "DLSSG.OutputReal",
    "DLSSG.PrevClipToClip", "DLSSG.QueueSubmitCallback", "DLSSG.QueueSubmitCallbackData", "DLSSG.RefreshRate", "DLSSG.Reset",
    "DLSSG.StreamlineMode", "DLSSG.StreamlineVersionTag", "DLSSG.SyncFlushCallback", "DLSSG.SyncFlushCallbackData",
    "DLSSG.SyncSignalCallback", "DLSSG.SyncSignalCallbackData", "DLSSG.SyncSignalOnlyCallback", "DLSSG.SyncSignalOnlyCallbackData",
    "DLSSG.SyncWaitCallback", "DLSSG.SyncWaitCallbackData", "DLSSG.SyncWaitOnlyCallback", "DLSSG.SyncWaitOnlyCallbackData",
    "DLSSG.TargetFrameRate", "DLSSG.UI", "DLSSG.UIAlpha", "DLSSG.UIAlphaSubrectBaseX", "DLSSG.UIAlphaSubrectBaseY",
    "DLSSG.UIAlphaSubrectHeight", "DLSSG.UIAlphaSubrectWidth", "DLSSG.UISubrectBaseX", "DLSSG.UISubrectBaseY",
    "DLSSG.UISubrectHeight", "DLSSG.UISubrectWidth", "DLSSG.UserInterfaceRecompositionEnabled", "DLSSG.VSyncDriverRequested",
    "DLSSG.VSyncOn", "DLSSG.VSyncSupported", "DLSSG.VkOFAModeRequest", "DLSSG.Width", "DLSSG.run_lowres_mvec_pass",
};

static bool FgCensusReadFloats(const void *src, float *dst, size_t n)
{
    __try { memcpy(dst, src, n * sizeof(float)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void FgCensusAppend(char *line, size_t cap, size_t *len, const char *fmt, ...)
{
    if (*len >= cap - 1) return;
    va_list args;
    va_start(args, fmt);
    const int n = _vsnprintf_s(line + *len, cap - *len, _TRUNCATE, fmt, args);
    va_end(args);
    *len = n < 0 ? cap - 1 : *len + static_cast<size_t>(n);
}

static void FgInputCensus(const NVSDK_NGX_Parameter *p, unsigned pass)
{
    Log("[fg-census] pass %u begins: %u keys, types tried u/i/f/d/ull/ptr.", pass, static_cast<unsigned>(_countof(kFgCensusKeys)));
    for (size_t k = 0; k < _countof(kFgCensusKeys); ++k) {
        const char *key = kFgCensusKeys[k];
        char line[640];
        size_t len = 0;
        FgCensusAppend(line, sizeof(line), &len, "[fg-census] %s", key);
        bool any = false;
        unsigned u = 0; int i = 0; float f = 0.0f; double d = 0.0; unsigned long long ull = 0; void *ptr = nullptr;
        if (p->Get(key, &u) == NGX_SUCCESS)   { FgCensusAppend(line, sizeof(line), &len, " u=%u", u); any = true; }
        if (p->Get(key, &i) == NGX_SUCCESS)   { FgCensusAppend(line, sizeof(line), &len, " i=%d", i); any = true; }
        if (p->Get(key, &f) == NGX_SUCCESS)   { FgCensusAppend(line, sizeof(line), &len, " f=%.6g", f); any = true; }
        if (p->Get(key, &d) == NGX_SUCCESS)   { FgCensusAppend(line, sizeof(line), &len, " d=%.9g", d); any = true; }
        if (p->Get(key, &ull) == NGX_SUCCESS) { FgCensusAppend(line, sizeof(line), &len, " ull=%llu", ull); any = true; }
        if (p->Get(key, &ptr) == NGX_SUCCESS) {
            FgCensusAppend(line, sizeof(line), &len, " ptr=%p", ptr);
            any = true;
            if (ptr != nullptr && strstr(key, "Clip") != nullptr) {
                float m[16] = {};
                if (FgCensusReadFloats(ptr, m, 16))
                    FgCensusAppend(line, sizeof(line), &len,
                                   " m=[%.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g]",
                                   m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
            }
        }
        if (any) Log("%s", line);
    }
    Log("[fg-census] pass %u ends.", pass);
}

static bool FgCompositeWait(DWORD ms)
{
    FgComposite &c = g_fg_composite;
    if (c.fence == nullptr) return true;
    const UINT64 done = c.fence->GetCompletedValue();
    if (done == UINT64_MAX) return false;
    if (done >= c.fv) return true;
    if (FAILED(c.fence->SetEventOnCompletion(c.fv, c.ev))) return false;
    if (WaitForSingleObject(c.ev, ms) != WAIT_OBJECT_0) return false;
    const UINT64 after = c.fence->GetCompletedValue();
    return after != UINT64_MAX && after >= c.fv;
}

static bool FgCompositeMake(ID3D12Device *dev, ID3D12Resource *hudless, ID3D12Resource *nr,
                            ID3D12Resource *back)
{
    FgComposite &c = g_fg_composite;
    if (c.pso == nullptr) {
        PFN_D3DCompile_ compile = LoadSystemD3DCompiler();
        if (compile == nullptr) { Log("[fg-input] d3dcompiler_47.dll unavailable for the composite pass"); return false; }
        ID3DBlob *code = nullptr, *err = nullptr;
        HRESULT hr = compile(kFgCompositeShader, sizeof(kFgCompositeShader) - 1, "fgc", nullptr, nullptr,
                             "main", "cs_5_0", 0, 0, &code, &err);
        if (FAILED(hr) || code == nullptr) {
            Log("[fg-input] composite shader failed to compile: %s",
                err != nullptr ? static_cast<const char *>(err->GetBufferPointer()) : "");
            if (err != nullptr) err->Release();
            return false;
        }
        if (err != nullptr) err->Release();
        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 2;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].OffsetInDescriptorsFromTableStart = 2;
        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 2;
        param.DescriptorTable.pDescriptorRanges = ranges;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 1;
        rsd.pParameters = &param;
        HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
        auto serialize = d3d12 != nullptr
            ? reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
        ID3DBlob *rsblob = nullptr, *rserr = nullptr;
        hr = serialize != nullptr ? serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsblob, &rserr) : E_FAIL;
        if (FAILED(hr) || rsblob == nullptr) {
            Log("[fg-input] composite root signature would not serialize 0x%08X", hr);
            if (rserr != nullptr) rserr->Release();
            code->Release();
            return false;
        }
        if (rserr != nullptr) rserr->Release();
        hr = dev->CreateRootSignature(0, rsblob->GetBufferPointer(), rsblob->GetBufferSize(),
                                      __uuidof(ID3D12RootSignature), reinterpret_cast<void **>(&c.rs));
        rsblob->Release();
        if (FAILED(hr)) { code->Release(); Log("[fg-input] composite root signature failed 0x%08X", hr); return false; }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = c.rs;
        pd.CS.pShaderBytecode = code->GetBufferPointer();
        pd.CS.BytecodeLength = code->GetBufferSize();
        hr = dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void **>(&c.pso));
        code->Release();
        if (FAILED(hr)) { Log("[fg-input] composite pipeline failed 0x%08X", hr); return false; }
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 3;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void **>(&c.heap))) ||
            FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void **>(&c.alloc))) ||
            FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.alloc, nullptr, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&c.list))) ||
            FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&c.fence)))) {
            Log("[fg-input] composite heap/list/fence creation failed");
            return false;
        }
        c.list->Close();
        c.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (c.ev == nullptr) return false;
    }
    if (!FgCompositeWait(4000)) return false;
    c.hudless = hudless; c.nr = nr; c.back = back;
    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = c.heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    sd.Format = hudless->GetDesc().Format;
    dev->CreateShaderResourceView(hudless, &sd, cpu);
    cpu.ptr += inc;
    sd.Format = nr->GetDesc().Format;
    dev->CreateShaderResourceView(nr, &sd, cpu);
    cpu.ptr += inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    ud.Format = back->GetDesc().Format;
    dev->CreateUnorderedAccessView(back, nullptr, &ud, cpu);
    return true;
}

// Runs on the private queue after the NR submission; the caller fences on it.
// wait_done is false on the relay's path, where the completion is consumed by the
// D3D12 queue's own release signal rather than by this thread.
static bool FgCompositeRun(ID3D12CommandQueue *queue, DWORD wait_ms, bool wait_done = true)
{
    FgComposite &c = g_fg_composite;
    if (c.pso == nullptr || c.back == nullptr) return false;
    if (!FgCompositeWait(wait_ms)) return false;
    if (FAILED(c.alloc->Reset()) || FAILED(c.list->Reset(c.alloc, nullptr))) return false;
    Barrier(c.list, c.hudless, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(c.list, c.nr, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(c.list, c.back, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap *heaps[] = { c.heap };
    c.list->SetDescriptorHeaps(1, heaps);
    c.list->SetComputeRootSignature(c.rs);
    c.list->SetPipelineState(c.pso);
    c.list->SetComputeRootDescriptorTable(0, c.heap->GetGPUDescriptorHandleForHeapStart());
    const D3D12_RESOURCE_DESC bd = c.back->GetDesc();
    c.list->Dispatch((static_cast<UINT>(bd.Width) + 7) / 8, (bd.Height + 7) / 8, 1);
    Barrier(c.list, c.back, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    Barrier(c.list, c.nr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(c.list, c.hudless, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    if (FAILED(c.list->Close())) return false;
    ID3D12CommandList *lists[] = { c.list };
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(queue->Signal(c.fence, ++c.fv))) return false;
    if (!wait_done) return true;
    return FgCompositeWait(wait_ms);
}

// Identity on the private side so an unconditional Vulkan copy back is harmless.
static bool FgIdentityCopy(DWORD wait_ms)
{
    if (!Synth12Begin(wait_ms)) return false;
    Barrier(g_s12.list, g_s12.tex[SLOT_COLOR], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(g_s12.list, g_s12.tex[SLOT_OUTPUT], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    g_s12.list->CopyResource(g_s12.tex[SLOT_OUTPUT], g_s12.tex[SLOT_COLOR]);
    Barrier(g_s12.list, g_s12.tex[SLOT_COLOR], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(g_s12.list, g_s12.tex[SLOT_OUTPUT], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    return Synth12WaitFence(Synth12End(), wait_ms);
}

// Worker-thread half of one FG-input frame. The game's queue is parked while this
// runs, so every wait is bounded by the mirror's evaluate budget.
static bool FgInputWorker()
{
    bool ok = false, nr_absent = false;
    unsigned nr_entries = 0, sr_entries = 0, sr_copies = 0;
    if (g_present_adapter.run_nr) {
        g_present_nr_entries = g_present_sr_entries = g_present_sr_copies = 0;
        g_present_nr_scope = true;
        if (g_ngx_cs_ready) EnterCriticalSection(&g_ngx_cs);
        ok = Synth12EvaluateShared(&g_vkm.sp, kVkmEvaluateMs);
        if (g_ngx_cs_ready) LeaveCriticalSection(&g_ngx_cs);
        g_present_nr_scope = false;
        nr_entries = g_present_nr_entries; sr_entries = g_present_sr_entries; sr_copies = g_present_sr_copies;
        if (ok && (sr_entries != 1 || sr_copies != 1)) {
            Log("[fg-input] carrier contract mismatch: sr_entries=%u sr_copies=%u; frame not written.", sr_entries, sr_copies);
            ok = false;
        }
        // Generic switched off or warming up: the carrier round-trip is the game's own
        // image, so the recorded copy back is harmless and this is not a failure.
        if (ok && nr_entries == 0) { nr_absent = true; ++g_fg_input.skipped_nr; }
        if (ok && !nr_absent) ok = FgCompositeRun(g_s12.queue, kVkmEvaluateMs);
    } else {
        ok = PresentAdapterCopy12();
    }
    ok = ok && g_s12.dev->GetDeviceRemovedReason() == S_OK;
    if (!ok) {
        // Keep the carriers equal to their inputs so the recorded copy back is a no-op.
        FgIdentityCopy(kVkmEvaluateMs);
    }
    g_fg_input.worker_ok = ok;
    ++g_fg_input.frames;
    if (ok && !nr_absent) ++g_fg_input.writes;
    const unsigned cadence = g_present_adapter.continuous ? 1800u : 120u;
    if (g_fg_input.frames <= 3 || g_fg_input.frames % cadence == 0 || !ok || g_present_adapter.remaining == 0)
        Log("[fg-input] frame=%u %ux%u vkfmt=%u mode=%s pipeline=%s nr_entries=%u sr_entries=%u sr_copies=%u ok=%d busy_skips=%u token=%llu.",
            g_fg_input.frames, g_fg_input.w, g_fg_input.h, g_fg_input.vk_fmt,
            g_present_adapter.run_nr ? "NR" : g_present_adapter.colour_roundtrip ? "carrier" : "copy",
            PresentAdapterPipeline() == 1 ? "probe(discarded)" : "serial",
            nr_entries, sr_entries, sr_copies, ok ? 1 : 0, g_fg_input.busy_skips, g_present_adapter.token);
    if (g_present_adapter.remaining == 0)
        Log("[fg-input] request complete: frames=%u writes=%u no_nr=%u busy_skips=%u. token=%llu",
            g_fg_input.frames, g_fg_input.writes, g_fg_input.skipped_nr, g_fg_input.busy_skips, g_present_adapter.token);
    return ok;
}

static void FgInputRefuse(const char *why)
{
    if (g_fg_input.refused) return;
    g_fg_input.refused = true;
    g_present_adapter.remaining = 0;
    Log("[fg-input] refused: %s. Frame generation continues on the game's own frames.", why);
}

static bool FgInputReadU(const NVSDK_NGX_Parameter *p, const char *key, unsigned *out)
{
    unsigned u = 0; int i = 0;
    if (p->Get(key, &u) == NGX_SUCCESS) { *out = u; return true; }
    if (p->Get(key, &i) == NGX_SUCCESS) { *out = static_cast<unsigned>(i); return true; }
    return false;
}

// Render-thread only (inside the game's SR evaluate, where the mirror already waits
// the device idle for its own rebuilds). Hands the private session back so the
// other path can build. Returns false and keeps everything when completion is unproven.
static bool FgInputRelease()
{
    // A relay frame still armed or still being consumed owns the staging images
    // and a parked command buffer; the release refuses rather than races it.
    if (PresentAdapterRelay() && !FgRelayIdleForRelease()) return false;
    if (g_vkm.idle != nullptr && WaitForSingleObject(g_vkm.idle, kVkmIdleMs) != WAIT_OBJECT_0) return false;
    if (!FgCompositeWait(4000)) return false;
    if (!Synth12Drain(4000)) return false;
    if (g_svk.dev != nullptr && g_svk.DeviceWaitIdle != nullptr && g_svk.DeviceWaitIdle(g_svk.dev) != 0) return false;
    if (PresentAdapterRelay()) FgRelayOnSessionRelease();
    SynthVkReleaseImages();
    if (!Synth12ReleaseTextures()) return false;
    g_fg_composite.hudless = g_fg_composite.nr = g_fg_composite.back = nullptr;
    g_fg_input.vk_fmt = 0;
    g_fg_input.want_release = false;
    g_s12.w = 0;
    g_s12.need_reset = true;
    InterlockedExchange(&g_adapter_owner, OWNER_NONE);
    Log("[fg-input] released the private session; the native mirror may take it.");
    return true;
}

// Render-thread only: the mirror's session is torn down so the FG-input feed can build.
static bool MirrorReleaseForFgInput()
{
    if (g_vkm.idle != nullptr && WaitForSingleObject(g_vkm.idle, kVkmIdleMs) != WAIT_OBJECT_0) return false;
    if (!SynthVkDrainWorker()) return false;
    if (!Synth12Drain(4000)) return false;
    if (g_svk.dev != nullptr && g_svk.DeviceWaitIdle != nullptr && g_svk.DeviceWaitIdle(g_svk.dev) != 0) return false;
    SynthVkReleaseImages();
    SynthOfaVkRebuild();
    SynthDepthCsRelease();
    if (!Synth12ReleaseTextures()) return false;
    g_s12.w = 0;
    g_s12.need_reset = true;
    InterlockedExchange(&g_adapter_owner, OWNER_NONE);
    Log("[fg-input] the native mirror released the private session; Present is served at the frame-generation input.");
    return true;
}

// Render-thread half: records copies of the FG inputs into the FG evaluate's own
// command buffer, parks it, and records the copy back before the evaluate is
// forwarded -- so FG and the real-frame present both see the processed image.
static void FgInputFrame(void *cmd_raw, const NVSDK_NGX_Parameter *p)
{
    InterlockedIncrement(&g_fg_input.eval_calls);
    if (g_fg_input.refused || g_vkm.stopping || g_cfg.stage < 2) return;
    if (!FgInputActive()) return;
    if (InterlockedCompareExchange(&g_adapter_owner, 0, 0) == OWNER_MIRROR) return;
    PresentAdapterRequest();
    if (g_present_adapter.remaining == 0 && (PresentAdapterAutoRun() || PresentAdapterFollow()) &&
        g_present_adapter.request_seen && !g_present_adapter.refused && !g_present_adapter.stopped) {
        if (!PresentAdapterConsumer()) { g_present_adapter.refused = true; return; }
        g_present_adapter.remaining = UINT_MAX;
        g_present_adapter.continuous = true;
        g_present_adapter.run_nr = true;
        g_present_adapter.colour_roundtrip = false;
        g_present_adapter.deadline = ULLONG_MAX;
        g_present_adapter.token = 1;
        g_s12.need_reset = true;
        Log("[fg-input] AutoRun: continuous NR at the frame-generation input; a request with frames=0 stops it. pipeline=%s",
            PresentAdapterPipeline() == 1 ? "probe (NR concurrent, result discarded, picture carries no NR)" : "serial park");
    }
    if (g_present_adapter.remaining == 0 || g_present_adapter.refused) return;
    if (g_fg_input.census_left != 0) {
        unsigned idx = 0;
        FgInputReadU(p, "DLSSG.MultiFrameIndex", &idx);
        Log("[fg-census] evaluate observed: MultiFrameIndex=%u thread=%lu", idx, GetCurrentThreadId());
        FgInputCensus(p, 4 - g_fg_input.census_left);
        --g_fg_input.census_left;
    }
    if (GetTickCount64() >= g_present_adapter.deadline) {
        Log("[fg-input] request complete: frames=%u writes=%u no_nr=%u (deadline). token=%llu",
            g_fg_input.frames, g_fg_input.writes, g_fg_input.skipped_nr, g_present_adapter.token);
        g_present_adapter.remaining = 0; return;
    }
    if (g_present_adapter.token != g_fg_input.token) {
        g_fg_input.token = g_present_adapter.token;
        g_fg_input.frames = g_fg_input.writes = g_fg_input.skipped_nr = g_fg_input.busy_skips = 0;
        static bool census_ran;
        if (!census_ran) { census_ran = true; g_fg_input.census_left = 3; }
    }
    unsigned index = 0;
    const bool indexed = FgInputReadU(p, "DLSSG.MultiFrameIndex", &index);
    if (indexed && index != 1) {
        InterlockedIncrement(&g_fg_input.eval_index_skipped);
        return;
    }
    if (!indexed) {
        // Without an index, the real-output slot marks the first evaluate of a group.
        const char *why_real = nullptr;
        VkmRes real = {};
        if (!VkmGetRes(p, "DLSSG.OutputReal", &real, &why_real)) return;
    }
    if (!PresentAdapterConfiguration()) { g_present_adapter.remaining = 0; g_present_adapter.refused = true; return; }
    if (g_cfg.mode != 2) { FgInputRefuse("the experiment requires Bridge mode=2"); return; }
    if (PresentAdapterRelay())
    {
        // The relay owns the frames it arms: it records the park and the copy
        // back against the copy the present (Pipeline=3) or this evaluate
        // (Pipeline=2) already submitted. 0 means it has nothing for this frame
        // and the serial feed below runs as it always has.
        const int rel = FgRelayEvaluate(cmd_raw, p);
        if (rel != 0) return;
    }
    if (g_vkm_rt == nullptr) return;
    reshade::api::device *rdev = g_vkm_rt->get_device();
    if (rdev == nullptr || rdev->get_api() != reshade::api::device_api::vulkan) return;
    if (!Synth12Open(g_vkm_rt) || !SynthVkOpen(g_vkm_rt)) { FgInputRefuse("private session unavailable"); return; }

    const char *why = nullptr;
    VkmRes hud = {}, back = {};
    if (!VkmGetRes(p, "DLSSG.HUDLess", &hud, &why)) { FgInputRefuse("no HUD-less input; this feed requires the game's HUD-less image"); return; }
    if (!VkmGetRes(p, "DLSSG.Backbuffer", &back, &why)) { FgInputRefuse("no backbuffer input"); return; }
    if (hud.w != back.w || hud.h != back.h || hud.fmt != back.fmt || hud.w == 0 || hud.h == 0) {
        FgInputRefuse("HUD-less and backbuffer differ in size or format"); return;
    }
    unsigned sub_w = 0, sub_h = 0, sub_x = 0, sub_y = 0;
    FgInputReadU(p, "DLSSG.BackbufferSubrectWidth", &sub_w);
    FgInputReadU(p, "DLSSG.BackbufferSubrectHeight", &sub_h);
    FgInputReadU(p, "DLSSG.BackbufferSubrectBaseX", &sub_x);
    FgInputReadU(p, "DLSSG.BackbufferSubrectBaseY", &sub_y);
    if (sub_x != 0 || sub_y != 0 || (sub_w != 0 && sub_w != back.w) || (sub_h != 0 && sub_h != back.h)) {
        FgInputRefuse("backbuffer subrect is not the whole image"); return;
    }
    const DXGI_FORMAT fmt = MapVkColor(back.fmt);
    if (fmt != DXGI_FORMAT_R8G8B8A8_UNORM && fmt != DXGI_FORMAT_B8G8R8A8_UNORM) {
        FgInputRefuse("only UNORM8 sRGB-encoded FG inputs are handled by this build"); return;
    }
    // The game's own guides at output resolution, as handed to frame generation.
    static const uint32_t kVkFmtR16G16Sfloat = 83;
    VkmRes mv = {}, depth = {};
    const bool have_mv = VkmGetRes(p, "DLSSG.MVecs", &mv, &why) && mv.fmt == kVkFmtR16G16Sfloat &&
                         mv.w == back.w && mv.h == back.h;
    const bool have_depth = VkmGetRes(p, "DLSSG.Depth", &depth, &why) && depth.fmt == kVkFmtR32Sfloat &&
                            depth.w == back.w && depth.h == back.h;
    float mv_scale_x = 1.0f, mv_scale_y = 1.0f;
    unsigned depth_inverted = 0;
    if (have_mv) {
        p->Get("DLSSG.MvecScaleX", &mv_scale_x);
        p->Get("DLSSG.MvecScaleY", &mv_scale_y);
    }
    FgInputReadU(p, "DLSSG.DepthInverted", &depth_inverted);
    if (!VkmEnsureEvents()) return;
    const bool probe = PresentAdapterPipeline() == 1;
    if (probe) {
        // Concurrent by design: a frame that arrives while the previous NR is still
        // running is skipped and counted, never waited for.
        if (g_vkm.idle != nullptr && WaitForSingleObject(g_vkm.idle, 0) != WAIT_OBJECT_0) { ++g_fg_input.busy_skips; return; }
    } else if (g_vkm.idle != nullptr && WaitForSingleObject(g_vkm.idle, kVkmIdleMs) != WAIT_OBJECT_0) {
        FgInputRefuse("the previous frame's park had not cleared"); return;
    }
    if (g_vkm.refused) return;

    SynthBuild(&g_vkm.sp, back.w, back.h, fmt, reshade::api::resource{0}, reshade::api::resource{0},
               reshade::api::resource{0}, 0, -1);
    g_vkm.sp.Set("DLSS.Feature.Create.Flags", depth_inverted != 0 ? 0x08u : 0u);
    if (have_mv) {
        g_vkm.sp.Set("MV.Scale.X", mv_scale_x);
        g_vkm.sp.Set("MV.Scale.Y", mv_scale_y);
    }
    const bool rebuild = g_s12.feature == nullptr || g_s12.w != back.w || g_s12.h != back.h ||
                         g_s12.fmt != fmt || !g_s12.out_b || g_fg_input.vk_fmt != back.fmt;
    if (rebuild) {
        // The FG evaluate runs on Streamline's present thread while the render thread
        // keeps submitting; a device-wide wait here deadlocks (measured2026-09-28). Only
        // a first build, with nothing imported yet, is safe; a later shape change refuses.
        if (g_s12.feature != nullptr || g_svk.img[SLOT_COLOR] != 0 || g_svk.img[kOutB] != 0) {
            if (PresentAdapterFollow()) { g_fg_input.want_release = true; return; }
            FgInputRefuse("the frame-generation input changed shape; rebuilding here would need a device-wide wait on the present thread");
            return;
        }
        Log("[fg-input] building the private feature and its Vulkan import for %ux%u %s", back.w, back.h, FormatName(fmt));
        // The synthetic source label selects the colour-conversion pass in Synth12Build.
        InterlockedExchange(&g_source, SRC_SYNTH);
        if (!Synth12Build(&g_vkm.sp, back.w, back.h, back.w, back.h, fmt, false, 0, 0, true,
                          DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_UNKNOWN, true) || !g_s12.out_b) {
            Synth12NoteBuildFailure(false);
            FgInputRefuse("the private D3D12 feature or its second carrier could not be built"); return;
        }
        if (!SynthVkImport()) { SynthVkReleaseImages(); g_svk.ready = false; FgInputRefuse("import into the game's VkDevice failed"); return; }
        if (!PresentAdapterCarrier()) { FgInputRefuse("private SR carrier hook unavailable"); return; }
        PresentAdapterWitness();
        if (!FgCompositeMake(g_s12.dev, g_s12.tex[SLOT_COLOR], g_s12.tex[SLOT_OUTPUT], g_s12.tex[kOutB])) {
            FgInputRefuse("composite pass unavailable"); return;
        }
        g_fg_input.vk_fmt = back.fmt; g_fg_input.w = back.w; g_fg_input.h = back.h;
        g_s12.need_reset = true;
        InterlockedExchange(&g_adapter_owner, OWNER_FG);
        Log("[fg-input] ready: HUD-less -> linear sRGB carrier -> Generic NR -> UI-preserving composite; FG interpolates processed frames. guides: mv=%s (scale %.3f,%.3f) depth=%s inverted=%u",
            have_mv ? "game" : "zero", mv_scale_x, mv_scale_y, have_depth ? "game" : "zero", depth_inverted);
    }

    SVkCommandBuffer cb = reinterpret_cast<SVkCommandBuffer>(cmd_raw);
    const LONG drain = InterlockedCompareExchange(&g_vkm.drain, 0, 0);
    if (drain != 0) {
        if (g_svk.EvStatus(g_svk.dev, g_vkm.ev_in[drain - 1]) != kSVkEventSet) return;
        InterlockedExchange(&g_vkm.drain, 0);
        return;
    }
    const int pair = g_vkm.next;
    g_vkm.next ^= 1;
    g_svk.EvReset(g_svk.dev, g_vkm.ev_in[pair]);
    g_svk.EvReset(g_svk.dev, g_vkm.ev_out[pair]);
    ResetEvent(g_vkm.idle);

    const uint32_t first = g_svk.laid_out ? kSVkLayoutGeneral : kSVkLayoutUndefined;
    SynthVkBarrier(cb, hud.image, kSVkLayoutGeneral, kSVkLayoutTransferSrc);
    SynthVkBarrier(cb, back.image, kSVkLayoutGeneral, kSVkLayoutTransferSrc);
    if (have_mv) SynthVkBarrier(cb, mv.image, kSVkLayoutGeneral, kSVkLayoutTransferSrc);
    if (have_depth) SynthVkBarrier(cb, depth.image, kSVkLayoutGeneral, kSVkLayoutTransferSrc);
    for (int i = 0; i < SLOT_COUNT; ++i) SynthVkBarrier(cb, g_svk.img[i], first, kSVkLayoutGeneral);
    SynthVkBarrier(cb, g_svk.img[kOutB], first, kSVkLayoutGeneral);
    const bool clear_guides = !g_svk.laid_out;
    g_svk.laid_out = true;
    SynthVkCopyImage(cb, hud.image, kSVkLayoutTransferSrc, g_svk.img[SLOT_COLOR], kSVkLayoutGeneral, back.w, back.h);
    SynthVkCopyImage(cb, back.image, kSVkLayoutTransferSrc, g_svk.img[kOutB], kSVkLayoutGeneral, back.w, back.h);
    if (have_mv)
        SynthVkCopyImage(cb, mv.image, kSVkLayoutTransferSrc, g_svk.img[SLOT_MV], kSVkLayoutGeneral, back.w, back.h);
    if (have_depth)
        SynthVkCopyImage(cb, depth.image, kSVkLayoutTransferSrc, g_svk.img[SLOT_DEPTH], kSVkLayoutGeneral, back.w, back.h);
    if (clear_guides && (!have_mv || !have_depth)) {
        using ClearImage = void(*)(SVkCommandBuffer, SVkHandle, uint32_t, const void*, uint32_t, const SVkSubresourceRange*);
        auto get_proc = reinterpret_cast<PFN_SVkGetDeviceProcAddr>(GetProcAddress(g_svk.lib, "vkGetDeviceProcAddr"));
        auto clear = get_proc != nullptr ? reinterpret_cast<ClearImage>(get_proc(g_svk.dev, "vkCmdClearColorImage")) : nullptr;
        const float zero[4] = {};
        const SVkSubresourceRange range = {1, 0, 1, 0, 1};
        if (clear != nullptr) {
            if (!have_mv) clear(cb, g_svk.img[SLOT_MV], kSVkLayoutGeneral, zero, 1, &range);
            if (!have_depth) clear(cb, g_svk.img[SLOT_DEPTH], kSVkLayoutGeneral, zero, 1, &range);
        }
    }
    SynthVkBarrier(cb, hud.image, kSVkLayoutTransferSrc, kSVkLayoutGeneral);
    SynthVkBarrier(cb, back.image, kSVkLayoutTransferSrc, kSVkLayoutGeneral);
    if (have_mv) SynthVkBarrier(cb, mv.image, kSVkLayoutTransferSrc, kSVkLayoutGeneral);
    if (have_depth) SynthVkBarrier(cb, depth.image, kSVkLayoutTransferSrc, kSVkLayoutGeneral);

    g_svk.CmdSetEvent(cb, g_vkm.ev_in[pair], kSVkStageAllCommands);
    g_vkm.cur = pair;
    g_vkm.fg_arm = true;
    --g_present_adapter.remaining;
    SetEvent(g_vkm.wake);
    // Probe: no park and no copy back. FG proceeds on the game's own image while the
    // private device runs NR on the copies; the picture carries no NR for the run.
    if (probe) return;
    {
        SVkMemoryBarrier mb = {};
        mb.sType = kSVkStMemoryBarrier;
        mb.srcAccessMask = kSVkAccessMemoryRead | kSVkAccessMemoryWrite;
        mb.dstAccessMask = kSVkAccessMemoryRead | kSVkAccessMemoryWrite;
        const SVkHandle ev = g_vkm.ev_out[pair];
        g_svk.CmdWaitEvents(cb, 1, &ev, kSVkStageHost, kSVkStageAllCommands, 1, &mb, 0, nullptr, 0, nullptr);
    }
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
}
