extern "C" {
void* g_present_nr_original = nullptr;
void* g_present_sr_original = nullptr;
void PresentAdapterNrHook();
void PresentAdapterSrHook();
}
static thread_local bool g_present_nr_scope;
static thread_local unsigned g_present_nr_entries;
static thread_local unsigned g_present_sr_entries;
static thread_local unsigned g_present_sr_copies;
extern "C" void PresentAdapterNrEnter()
{
    if (g_present_nr_scope) ++g_present_nr_entries;
}

extern "C" NVSDK_NGX_Result PresentAdapterSrEnter(
    ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter* params, void*)
{
    if (!g_present_nr_scope) return 0;
    ++g_present_sr_entries;
    if (list != UnwrapList(g_s12.list)) return 0;
    ID3D12Resource* colour = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* expected_colour = g_s12.pq.in != nullptr ? g_s12.pq.in : g_s12.tex[SLOT_COLOR];
    ID3D12Resource* expected_output = g_s12.pq.out != nullptr ? g_s12.pq.out : g_s12.tex[SLOT_OUTPUT];
    if (params == nullptr || params->Get("Color", &colour) != NGX_SUCCESS ||
        params->Get("Output", &output) != NGX_SUCCESS ||
        colour != expected_colour || output != expected_output) return 0;
    const D3D12_RESOURCE_DESC input_desc = colour->GetDesc();
    const D3D12_RESOURCE_DESC output_desc = output->GetDesc();
    if (input_desc.Width != output_desc.Width || input_desc.Height != output_desc.Height ||
        input_desc.SampleDesc.Count != 1 || output_desc.SampleDesc.Count != 1 ||
        Synth12UavFormat(input_desc.Format) != Synth12UavFormat(output_desc.Format)) return 0;
    // Only the private carrier is replaced; game SR calls retain their original caller and implementation.
    Barrier(list, colour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(output, colour);
    Barrier(list, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(list, colour, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ++g_present_sr_copies;
    return NGX_SUCCESS;
}

struct PresentAdapterState {
    bool request_seen, refused, nr_hook, sr_hook, run_nr, colour_roundtrip, stopped, continuous;
    uint32_t source_space;
    ULONGLONG poll_at, deadline, accepted;
    unsigned long long token;
    unsigned remaining, frames, writes, skipped_nr;
    reshade::api::effect_runtime* owner;
};
static PresentAdapterState g_present_adapter = {};

static bool PresentAdapterConfiguration()
{
    wchar_t ini[1024] = {};
    PresentAdapterPath(ini, _countof(ini), L"ReShade.ini");
    const int hook_point = GetPrivateProfileIntW(L"RenoDX.DLSS5", L"NRHookPoint", 0, ini);
    if (GetPrivateProfileIntW(L"RenoDX.DLSS5", L"EnableHooks", 2, ini) != 1 ||
        (hook_point != 0 && hook_point != 2))
    {
        Log("[present-adapter] refused: keep EnableHooks=1 and NRHookPoint=0 (Upscaled) or2 (Present) for the D3D12 carrier.");
        return false;
    }
    if (GetPrivateProfileIntW(L"RenoDX.DLSS5", L"NRSourceEncoding", 0, ini) != 0 ||
        GetPrivateProfileIntW(L"RenoDX.DLSS5", L"NRSourcePrimaries", 0, ini) != 0 ||
        GetPrivateProfileIntW(L"RenoDX.DLSS5", L"NRLinearUnitNits", 0, ini) != 0) {
        Log("[present-adapter] refused: source encoding, primaries and linear-unit overrides must be Auto; the carrier supplies normalized SDR or scRGB.");
        return false;
    }
    return true;
}

static bool PresentAdapterConsumer()
{
    const HMODULE consumer = GetModuleHandleW(L"renodx-dlss5.addon64");
    wchar_t path[1024] = {};
    char hash[65] = {};
    if (consumer == nullptr || GetModuleFileNameW(consumer, path, _countof(path)) == 0 ||
        !Sha256File(path, hash, sizeof(hash)) ||
        _stricmp(hash, "DCD93881E976AD033D83C2BB01F4BC3E4DDC59C15FE0DD4CA165BC5FC7D1AC68") != 0)
    {
        Log("[present-adapter] refused: this experiment requires the unmodified Generic 8.5.0-rc10 DCD93881 build.");
        return false;
    }
    return PresentAdapterConfiguration();
}

static bool PresentAdapterWitness()
{
    if (g_present_adapter.nr_hook) return true;
    HMODULE runtime = GetModuleHandleW(L"nvngx_dlssnr.dll");
    void* entry = runtime != nullptr
        ? reinterpret_cast<void*>(GetProcAddress(runtime, "NVSDK_NGX_D3D12_EvaluateFeature")) : nullptr;
    if (entry == nullptr) return false;
    // The tail forward preserves the caller's return address and all four args.
    // A module reference prevents a live hook from pointing into an unloaded DLL.
    HMODULE keep = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                          reinterpret_cast<LPCWSTR>(entry), &keep)) return false;
    const MH_STATUS made = MH_CreateHook(entry, reinterpret_cast<void*>(&PresentAdapterNrHook),
                                         &g_present_nr_original);
    if (made != MH_OK) return false;
    const MH_STATUS enabled = MH_EnableHook(entry);
    if (enabled != MH_OK) { MH_RemoveHook(entry); g_present_nr_original = nullptr; return false; }
    g_present_adapter.nr_hook = true;
    Log("[present-adapter] NR entry witness ready (tail-forward, CPU observation only).");
    return true;
}

static HMODULE PresentAdapterSrModule()
{
    auto enumerate = reinterpret_cast<PFN_EnumProcessModules>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules"));
    HMODULE modules[512] = {};
    DWORD needed = 0;
    if (enumerate == nullptr || !enumerate(GetCurrentProcess(), modules, sizeof(modules), &needed) ||
        needed > sizeof(modules)) return nullptr;
    HMODULE found = nullptr;
    for (DWORD i = 0; i < needed / sizeof(HMODULE); ++i) {
        if (GetProcAddress(modules[i], "NVSDK_NGX_D3D12_EvaluateFeature") == nullptr) continue;
        wchar_t path[1024] = {};
        if (GetModuleFileNameW(modules[i], path, _countof(path)) == 0) continue;
        DWORD ignored = 0;
        const DWORD size = GetFileVersionInfoSizeW(path, &ignored);
        if (size == 0) continue;
        std::vector<unsigned char> version(size);
        if (!GetFileVersionInfoW(path, 0, size, version.data())) continue;
        struct Translation { WORD language, codepage; };
        Translation* translations = nullptr;
        UINT bytes = 0;
        if (!VerQueryValueW(version.data(), L"\\VarFileInfo\\Translation",
                            reinterpret_cast<void**>(&translations), &bytes)) continue;
        bool is_sr = false;
        for (UINT j = 0; j < bytes / sizeof(Translation); ++j) {
            wchar_t* name = nullptr;
            wchar_t* product = nullptr;
            UINT length = 0;
            wchar_t key[96] = {};
            swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\InternalName",
                       translations[j].language, translations[j].codepage);
            if (!VerQueryValueW(version.data(), key, reinterpret_cast<void**>(&name), &length) ||
                name == nullptr || length == 0) continue;
            swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\ProductName",
                       translations[j].language, translations[j].codepage);
            if (!VerQueryValueW(version.data(), key, reinterpret_cast<void**>(&product), &length) ||
                product == nullptr || length == 0) continue;
            const wchar_t* leaf = wcsrchr(path, L'\\');
            leaf = leaf != nullptr ? leaf + 1 : path;
            // Full paths with non-ANSI characters print empty through %ls; the leaf is enough to identify the module.
            Log("[present-adapter] NGX evaluate module: %ls internal=%ls product=%ls.", leaf, name, product);
            // Ray Reconstruction shares InternalName=DLSS; only the super-sampling product is the SR carrier.
            if (_wcsicmp(name, L"DLSS") == 0 &&
                _wcsicmp(product, L"NVIDIA Deep Learning SuperSampling") == 0) is_sr = true;
        }
        if (!is_sr) continue;
        if (found != nullptr && found != modules[i]) {
            Log("[present-adapter] multiple SR modules found; refusing ambiguous carrier hook.");
            return nullptr;
        }
        found = modules[i];
    }
    return found;
}

static bool PresentAdapterCarrier()
{
    if (g_present_adapter.sr_hook) return true;
    const HMODULE runtime = PresentAdapterSrModule();
    void* entry = runtime != nullptr
        ? reinterpret_cast<void*>(GetProcAddress(runtime, "NVSDK_NGX_D3D12_EvaluateFeature")) : nullptr;
    if (entry == nullptr) {
        Log("[present-adapter] SR export unavailable: module=%p entry=%p error=%lu.", runtime, entry, GetLastError());
        return false;
    }
    HMODULE keep = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                          reinterpret_cast<LPCWSTR>(entry), &keep)) return false;
    const MH_STATUS made = MH_CreateHook(entry, reinterpret_cast<void*>(&PresentAdapterSrHook), &g_present_sr_original);
    if (made != MH_OK) {
        Log("[present-adapter] SR hook creation failed: status=%d entry=%p.", static_cast<int>(made), entry);
        return false;
    }
    const MH_STATUS enabled = MH_EnableHook(entry);
    if (enabled != MH_OK) {
        Log("[present-adapter] SR hook enable failed: status=%d.", static_cast<int>(enabled));
        MH_RemoveHook(entry);
        g_present_sr_original = nullptr;
        return false;
    }
    g_present_adapter.sr_hook = true;
    Log("[present-adapter] private SR carrier copy ready; external SR calls remain tail-forwarded.");
    return true;
}

static void PresentAdapterRequest()
{
    const ULONGLONG now = GetTickCount64();
    if (now < g_present_adapter.poll_at) return;
    g_present_adapter.poll_at = now + 500;
    wchar_t path[1024] = {};
    PresentAdapterPath(path, _countof(path), L"vk-present-adapter.request");
    FILE* file = nullptr;
    unsigned long long token = 0;
    unsigned frames = 0, nr = 0;
    if (_wfopen_s(&file, path, L"rb") == 0 && file != nullptr) {
        if (fscanf_s(file, "%llu %u %u", &token, &frames, &nr) != 3) token = 0;
        fclose(file);
    }
    if (!g_present_adapter.request_seen) {
        g_present_adapter.request_seen = true;
        g_present_adapter.token = token;
        Log("[present-adapter] enabled, idle. Awaiting a new request token; existing requests are ignored.");
        return;
    }
    if (token == 0 || token == g_present_adapter.token) return;
    g_present_adapter.token = token;
    if (frames == 0) { g_present_adapter.remaining = 0; g_present_adapter.stopped = true; g_present_adapter.continuous = false; Log("[present-adapter] stopped by request."); return; }
    if (frames > 1800 || nr > 2 || g_present_adapter.refused) {
        Log("[present-adapter] request refused (invalid count/mode or retired session)."); return;
    }
    if (!PresentAdapterConsumer()) { g_present_adapter.refused = true; return; }
    g_present_adapter.remaining = frames;
    g_present_adapter.stopped = false;
    g_present_adapter.continuous = false;
    g_present_adapter.run_nr = nr == 1;
    g_present_adapter.colour_roundtrip = nr == 2;
    // Frames between requests were not evaluated, so their temporal history is missing.
    if (g_present_adapter.run_nr) g_s12.need_reset = true;
    g_present_adapter.frames = g_present_adapter.writes = g_present_adapter.skipped_nr = 0;
    g_present_adapter.accepted = now;
    g_present_adapter.deadline = now + 60000;
    Log("[present-adapter] request %llu accepted: max=%u frames, deadline=60s, mode=%s, guides=neutral.",
        token, frames, nr == 1 ? "Generic NR via identity SR carrier" :
                       nr == 2 ? "carrier colour roundtrip only" : "copy roundtrip only");
}

static bool PresentAdapterCopy12()
{
    const bool converted = g_present_adapter.colour_roundtrip && g_s12.pq.in != nullptr;
    ID3D12Resource* colour = converted ? g_s12.pq.in : g_s12.tex[SLOT_COLOR];
    ID3D12Resource* output = converted ? g_s12.pq.out : g_s12.tex[SLOT_OUTPUT];
    if (Synth12UavFormat(colour->GetDesc().Format) != Synth12UavFormat(output->GetDesc().Format)) return false;
    if (converted && !PqPassBefore(&g_s12.pq, g_s12.queue, 1000)) return false;
    if (!Synth12Begin(1000)) return false;
    const D3D12_RESOURCE_STATES input_state = converted ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON;
    const D3D12_RESOURCE_STATES output_state = converted ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COMMON;
    Barrier(g_s12.list, colour, input_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(g_s12.list, output, output_state, D3D12_RESOURCE_STATE_COPY_DEST);
    g_s12.list->CopyResource(output, colour);
    Barrier(g_s12.list, colour, D3D12_RESOURCE_STATE_COPY_SOURCE, input_state);
    Barrier(g_s12.list, output, D3D12_RESOURCE_STATE_COPY_DEST, output_state);
    UINT64 submitted = Synth12End();
    if (converted) {
        const UINT64 encoded = PqPassAfter(&g_s12.pq, g_s12.queue, g_s12.fence,
                                           &g_s12.fence_value, submitted, 0, 1000);
        if (encoded == submitted) return false;
        submitted = encoded;
    }
    return Synth12WaitFence(submitted, 1000) && g_s12.dev->GetDeviceRemovedReason() == S_OK;
}

static void PresentAdapterFrame(reshade::api::effect_runtime* rt, reshade::api::command_list* cmd,
                                reshade::api::resource_view rtv)
{
    using namespace reshade::api;
    static thread_local bool in_frame;
    if (in_frame) return;
    if (rt == nullptr || cmd == nullptr || rtv.handle == 0) return;
    if (PresentAdapterFgInput()) return;
    device* dev = rt->get_device();
    if (dev == nullptr || dev->get_api() != device_api::vulkan) return;
    if (!TryEnterCriticalSection(&g_bridge_cs)) return;
    struct Unlock { ~Unlock() { LeaveCriticalSection(&g_bridge_cs); } } unlock;
    in_frame = true;
    struct FrameScope { bool& value; ~FrameScope() { value = false; } } frame_scope{in_frame};
    PresentAdapterRequest();
    if (g_present_adapter.remaining == 0 || g_present_adapter.refused) return;
    if (GetTickCount64() >= g_present_adapter.deadline) {
        Log("[present-adapter] request complete: frames=%u writes=%u no_nr=%u (deadline).",
            g_present_adapter.frames, g_present_adapter.writes, g_present_adapter.skipped_nr);
        g_present_adapter.remaining = 0; return;
    }
    if (g_present_adapter.owner != nullptr && g_present_adapter.owner != rt) return;
    if (!PresentAdapterConfiguration()) {
        g_present_adapter.remaining = 0;
        g_present_adapter.refused = true;
        return;
    }
    if (g_cfg.mode != 2 || g_cfg.stage != 3) {
        Log("[present-adapter] refused: the experiment requires Bridge mode=2, stage=3.");
        g_present_adapter.refused = true; return;
    }
    SynthBound back = {};
    SynthResolveView(dev, static_cast<uint32_t>(device_api::vulkan), rtv, &back);
    if (back.res.handle == 0 || back.w == 0 || back.h == 0 || back.samples != 1 ||
        (back.usage & static_cast<uint32_t>(resource_usage::copy_source)) == 0) return;
    if (back.res.handle != rt->get_current_back_buffer().handle) return;
    command_queue* queue = rt->get_command_queue();
    if (queue == nullptr || queue->get_immediate_command_list() != cmd) {
        Log("[present-adapter] refused: callback is not the queue's immediate command list.");
        g_present_adapter.refused = true; return;
    }
    PresentSwapchainContract source;
    {
        std::lock_guard<std::mutex> lock(g_present_swapchain_mutex);
        source = g_present_swapchain;
    }
    if (source.device != dev || source.window != rt->get_hwnd() || source.first_buffer == 0 ||
        rt->get_back_buffer_count() == 0 || source.first_buffer != rt->get_back_buffer(0).handle) {
        Log("[present-adapter] no matching swapchain colour contract; refusing processing.");
        g_present_adapter.refused = true;
        return;
    }
    const color_space space = source.space;
    const bool floating = SynthFormatCanExceedOne(back.fmt);
    const bool srgb = space == color_space::srgb && !floating;
    const bool pq = space == color_space::hdr10_pq && !floating;
    if (!srgb && !pq && !(space == color_space::scrgb && floating)) {
        Log("[present-adapter] unsupported source colour contract: space=%u format=%u.",
            static_cast<unsigned>(space), static_cast<unsigned>(back.fmt));
        g_present_adapter.refused = true;
        return;
    }
    g_present_adapter.owner = rt;
    InterlockedExchange(&g_source, SRC_SYNTH);
    SynthParams params;
    SynthBuild(&params, back.w, back.h, back.fmt, back.res, resource{0}, resource{0}, 0, -1);
    params.Set("DLSS.Feature.Create.Flags", srgb ? 0u : 1u);
    if (!Synth12Open(rt) || !SynthVkOpen(rt)) {
        g_present_adapter.refused = true; Log("[present-adapter] session creation failed; no image modified."); return;
    }
    const bool rebuild = g_s12.feature == nullptr || g_s12.w != back.w ||
                         g_s12.h != back.h || g_s12.fmt != back.fmt ||
                         g_present_adapter.source_space != static_cast<uint32_t>(space);
    if (rebuild) {
        queue->wait_idle();
        if (!Synth12Drain(1000)) {
            g_present_adapter.refused = true; return;
        }
        SynthVkPresentRelease();
        SynthVkReleaseImages();
        if (!Synth12Build(&params, back.w, back.h, back.w, back.h, back.fmt,
                          false, 0, 0, false, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_UNKNOWN, srgb, pq) ||
            !SynthVkImport()) {
            g_present_adapter.refused = true; Log("[present-adapter] image build/import failed."); return;
        }
        g_present_adapter.source_space = static_cast<uint32_t>(space);
        Log("[present-adapter] source space=%u; working colour=%s.", static_cast<unsigned>(space),
            srgb ? "linear normalized SDR" : "linear BT.709, 1.0=80 nits");
    }
    if (g_present_adapter.run_nr && !PresentAdapterCarrier()) {
        g_present_adapter.refused = true;
        Log("[present-adapter] private SR carrier hook unavailable; no image modified.");
        return;
    }
    const bool draw = (back.usage & static_cast<uint32_t>(resource_usage::copy_dest)) == 0;
    if (draw && (!SynthVkPresentOpen(dev, back.fmt) || !SynthVkPresentSize(back.w, back.h, back.fmt))) {
        g_present_adapter.refused = true; Log("[present-adapter] draw-back resources failed."); return;
    }
    using ClearImage = void(*)(SVkCommandBuffer, SVkHandle, uint32_t, const void*, uint32_t,
                               const SVkSubresourceRange*);
    auto get_proc = reinterpret_cast<PFN_SVkGetDeviceProcAddr>(GetProcAddress(g_svk.lib, "vkGetDeviceProcAddr"));
    auto clear = get_proc != nullptr ? reinterpret_cast<ClearImage>(get_proc(g_svk.dev, "vkCmdClearColorImage")) : nullptr;
    if (clear == nullptr) { g_present_adapter.refused = true; return; }
    const double t0 = SynthNowMs();
    cmd->barrier(back.res, resource_usage::render_target, resource_usage::copy_source);
    auto cb = reinterpret_cast<SVkCommandBuffer>(static_cast<uintptr_t>(cmd->get_native()));
    const bool initialize_guides = !g_svk.laid_out;
    for (int i = 0; i < SLOT_COUNT; ++i)
        SynthVkBarrier(cb, g_svk.img[i], g_svk.laid_out ? kSVkLayoutGeneral : kSVkLayoutUndefined,
                       kSVkLayoutGeneral);
    g_svk.laid_out = true;
    if (initialize_guides) {
        const float zero[4] = {};
        const SVkSubresourceRange range = {1, 0, 1, 0, 1};
        clear(cb, g_svk.img[SLOT_DEPTH], kSVkLayoutGeneral, zero, 1, &range);
        clear(cb, g_svk.img[SLOT_MV], kSVkLayoutGeneral, zero, 1, &range);
        SynthVkBarrier(cb, g_svk.img[SLOT_DEPTH], kSVkLayoutGeneral, kSVkLayoutGeneral);
        SynthVkBarrier(cb, g_svk.img[SLOT_MV], kSVkLayoutGeneral, kSVkLayoutGeneral);
    }
    SynthVkCopyImage(cb, static_cast<SVkHandle>(back.res.handle), kSVkLayoutTransferSrc,
                     g_svk.img[SLOT_COLOR], kSVkLayoutGeneral, back.w, back.h);
    // Flush the borrowed immediate list before D3D12 reads the imported image.
    // No host-event park, guessed completion or writeback on a failed wait.
    queue->wait_idle();
    const double input_ready = SynthNowMs();
    const bool history_reset = g_s12.need_reset;
    bool ok = false;
    unsigned nr_entries = 0, sr_entries = 0, sr_copies = 0;
    if (g_present_adapter.run_nr) {
        PresentAdapterWitness();
        g_present_nr_entries = g_present_sr_entries = g_present_sr_copies = 0;
        g_present_nr_scope = true;
        if (g_ngx_cs_ready) EnterCriticalSection(&g_ngx_cs);
        ok = Synth12EvaluateShared(&params, 1000);
        if (g_ngx_cs_ready) LeaveCriticalSection(&g_ngx_cs);
        g_present_nr_scope = false;
        nr_entries = g_present_nr_entries;
        sr_entries = g_present_sr_entries;
        sr_copies = g_present_sr_copies;
        if (sr_entries != 1 || sr_copies != 1) {
            Log("[present-adapter] carrier contract mismatch: sr_entries=%u sr_copies=%u; refusing writeback.",
                sr_entries, sr_copies);
            ok = false;
        }
    } else {
        ok = PresentAdapterCopy12();
    }
    ok = ok && g_s12.dev->GetDeviceRemovedReason() == S_OK &&
         g_s12.fence->GetCompletedValue() != UINT64_MAX;
    cb = reinterpret_cast<SVkCommandBuffer>(static_cast<uintptr_t>(cmd->get_native()));
    const bool write = ok && (!g_present_adapter.run_nr || nr_entries != 0);
    if (write && draw) {
        SynthVkPresentDraw(cmd, cb, back.res, rtv, back.w, back.h, g_svk.img[SLOT_OUTPUT]);
    } else if (write) {
        cmd->barrier(back.res, resource_usage::copy_source, resource_usage::copy_dest);
        SynthVkBarrier(cb, g_svk.img[SLOT_OUTPUT], kSVkLayoutGeneral, kSVkLayoutGeneral);
        SynthVkCopyImage(cb, g_svk.img[SLOT_OUTPUT], kSVkLayoutGeneral,
                         static_cast<SVkHandle>(back.res.handle), kSVkLayoutTransferDst, back.w, back.h);
        cmd->barrier(back.res, resource_usage::copy_dest, resource_usage::render_target);
    } else {
        cmd->barrier(back.res, resource_usage::copy_source, resource_usage::render_target);
    }
    ++g_present_adapter.frames;
    if (write) ++g_present_adapter.writes;
    if (ok && g_present_adapter.run_nr && nr_entries == 0) ++g_present_adapter.skipped_nr;
    --g_present_adapter.remaining;
    if (g_present_adapter.frames <= 3 || g_present_adapter.frames % 120 == 0 ||
        g_present_adapter.remaining == 0 || !ok)
        Log("[present-adapter] frame=%u input=backbuffer-before-ReShade %ux%u fmt=%u image=0x%llX mode=%s nr_entries=%u completed=%d writeback=%d cpu_ms=%.2f. token=%llu reset=%d sr_entries=%u sr_copies=%u input_wait_ms=%.2f.",
            g_present_adapter.frames, back.w, back.h, static_cast<unsigned>(back.fmt),
            back.res.handle, g_present_adapter.run_nr ? "NR" : g_present_adapter.colour_roundtrip ? "carrier" : "copy", nr_entries,
            ok ? 1 : 0, write ? 1 : 0, SynthNowMs() - t0,
            g_present_adapter.token, history_reset ? 1 : 0, sr_entries, sr_copies, input_ready - t0);
    if (!ok) {
        g_present_adapter.refused = true;
        Log("[present-adapter] stopped after failed processing/completion; no copyback recorded for this frame.");
    }
    if (g_present_adapter.remaining == 0)
        Log("[present-adapter] request complete: frames=%u writes=%u no_nr=%u. token=%llu",
            g_present_adapter.frames, g_present_adapter.writes, g_present_adapter.skipped_nr,
            g_present_adapter.token);
}
