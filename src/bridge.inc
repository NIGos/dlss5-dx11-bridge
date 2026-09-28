#include "poll-yield.hpp"
// Implementation of the D3D11 -> D3D12 NGX bridge described in bridge.h.

static NVSDK_NGX_Result SafeEvaluateGuarded(PFN_D3D12EvaluateFeature fn,
                                            ID3D12GraphicsCommandList *list,
                                            const NVSDK_NGX_Handle *feature,
                                            const NVSDK_NGX_Parameter *p, DWORD *code)
{
    *code = 0;
    __try { return fn(list, feature, p, nullptr); }
    __except (CaptureFault(GetExceptionInformation()))
    { *code = GetExceptionCode(); return NGX_EXCEPTION_MARKER; }
}

// Everything worth trying here is a value, not code, and the add-on binary is
// locked while the game runs. So the tunables live in dlss5-bridge.cfg,
// re-read while playing: edit it, and within a second the bridge picks the
// change up and rebuilds whatever it invalidates. One session, many
// experiments.
//
//   mode=2         0 never writes to the game, 1 transport only, 2 full path
//   skip_exe=1     hold off on the host executable's own NGX entry points, and
//                  hook them only if nothing has called DLSS a minute later. A
//                  game that links the NGX client statically -- The Elder
//                  Scrolls Online does -- would otherwise have its code section
//                  patched at startup, which is what an integrity check
//                  terminates a process for. 0 hooks them at once, 2 never
//   dred=1         ask D3D12 to record what the GPU was executing, so a reset
//                  can be explained. Read once, when the session opens, so a
//                  change takes effect on the next launch
//   flags=-1       DLSS.Feature.Create.Flags. -1 copies whatever the game
//                  asked for, which is the only value guaranteed to describe
//                  the game's own buffers. Any other value forces that value:
//                  bit 0 is IsHDR, which is what makes RenoDX apply its HDR
//                  codec, and 107 is what Baldur's Gate 3 asks for
//   subrects=0     value for DLSS.Enable.Output.Subrects
//   reset_every=0  1 forces Reset on every frame, killing temporal history
//   pixels=0       1 enables the centre-pixel readback (stalls the GPU hard)
//   stage=3        how much of the bridge runs, to find which part disturbs the
//                  game's own rendering:
//                    0 fully inert -- the game must look perfect
//                    1 the Color and MV copies only
//                    2 also the depth compute blit
//                    3 also the fence sync and the D3D12 evaluate
//   skip_game=1    do not forward the game's own DLSS evaluate. Its result is
//                  overwritten by this bridge anyway, so running it is wasted
//                  work -- this is where most of the frame cost comes back.
//                  Only takes effect while the bridge is actually delivering.
//   source=auto    which contract the D3D12 side runs on: auto, mirror, synth
//                  or off. A word rather than a number, because auto is not a
//                  quantity. auto lets the game's own DLSS hold the session and
//                  falls back to the synthetic contract when it has none or
//                  stops calling it; mirror and synth pin one; off disables both
//   synth_after=0  seconds with no DLSS call from the game before a synthetic
//                  contract is built for a game that has never called DLSS at
//                  all. 0 leaves that opt-in. It does NOT gate the fallback for
//                  a game whose own DLSS is switched off mid-session: that one
//                  needs no key and is on
// The create flags are the only place the game states what its own buffers
// mean: whether the colour carries values above 1.0, whether the depth is
// reversed, whether the motion vectors are at render resolution. Spelling them
// out costs one line and removes the arithmetic from every report.
// The spelling on its own, because the status panel has to print the same names
// into an ImGui line rather than into the log. Two copies of this list is two
// lists to forget to extend the day NGX defines another bit.
static void CreateFlagNames(char *b, size_t n, unsigned int f)
{
    b[0] = 0;
    if (f & 0x01) strcat_s(b, n, " IsHDR");
    if (f & 0x02) strcat_s(b, n, " MVLowRes");
    if (f & 0x04) strcat_s(b, n, " MVJittered");
    if (f & 0x08) strcat_s(b, n, " DepthInverted");
    if (f & 0x20) strcat_s(b, n, " DoSharpening");
    if (f & 0x40) strcat_s(b, n, " AutoExposure");
    if (b[0] == 0) strcat_s(b, n, " none");
}

static void LogCreateFlags(const char *what, unsigned int f)
{
    char b[192];
    CreateFlagNames(b, sizeof(b), f);
    Log("[bridge] %s flags %u:%s", what, f, b);
}

// An 8-bit normalised format cannot hold a value above 1.0, so a buffer in one
// is not HDR whatever the flags say. The DLSS 5 add-on picks its codec from
// that flag, and picking the HDR codec for an SDR buffer is a silent wrong
// answer rather than an error, so it is worth stating outright.
static bool FormatIsLowPrecisionUnorm(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return true;
    default:
        return false;
    }
}

struct BridgeCfg
{
    int mode;
    int flags;
    int subrects;
    int reset_every;
    int pixels;
    int stage;
    int skip_game;
    int dred;
    int skip_exe;
    int unwrap;
    int source;        // CfgSource
    // The whole synthetic path, off by default. It is a substitute contract built
    // from ReShade's depth and the driver's optical flow, and it looks like one:
    // Red Dead Redemption 2 melts under it and Baldur's Gate 3 keeps its geometry
    // but not its text. Neither is a defect to fix in the estimator -- optical
    // flow has no idea what a glyph is, and no depth ReShade can see says which
    // pixels are foliage -- so it is a thing a user asks for, not a thing that
    // happens to them.
    //
    // Until now only the PRE-ARM case was gated, and the fallback for a game whose
    // own DLSS is switched off armed with no key at all. That was decided on
    // safety -- by then the game has demonstrably had DLSS and stopped calling it,
    // so nothing is being stolen -- and safety was the wrong question.
    int synth;
    // Read the D3D12 output texture back once and log a hash of it. Off by
    // default: it is one 8 MB readback and one fence wait, which is nothing in a
    // session and still not something to do in somebody's game unasked.
    //
    // It answers the one question nothing else here can: whether a NEIGHBOUR
    // add-on changed the picture. This add-on's own verdict proves it built a
    // contract and delivered frames; a DLSS 5 add-on that attaches, says it is
    // active and writes nothing looks identical from in here. Two runs, one with
    // the neighbour and one without, and the same hash means it did nothing.
    //
    // Measured need, 2026-09-01: exactly that happened, across two games, and
    // the afternoon went into this add-on before the difference turned out to be
    // two builds of the neighbour.
    int hash_out;
    int synth_after;   // seconds; at or below 0 means kSynthAfterDefaultSec
    // Which way a third-party motion-vector shader points its Y, and its X.
    // Neither ReshadeMotionEstimation nor qUINT_of.fx states the direction in
    // its declaration, and there is no parameter, semantic or annotation that
    // does -- so this cannot be derived, only tried. MV.Scale carries the sign,
    // and a sign that is wrong gathers the temporal history from the wrong side
    // of the movement: exactly the smear Trails beyond the Horizon showed when
    // -1.0 was invented for it. Guessing once and shipping it is how that
    // happened; a knob is how a user fixes it in the version they already have.
    int mv_sign_x;   // -1 / +1 force; 0 uses the provider's own convention
    int mv_sign_y;
    // NVOFA output grid size: 0 off, else 1, 2 or 4. Both the on/off switch and
    // the calibration knob, so there is no second key. Default 2, measured on an
    // RTX 5090 at 3840x1600: 1.6 ms/frame and 109 MB driver-side, and a median
    // error of 0.09-0.11 px against grid 4's 0.12-0.19 px on rotation and zoom.
    // Grid 4 is 0.6 ms and not broken; grid 1 buys nothing measurable over 2 and
    // costs 8.8 ms.
    int ofa_grid;
    // 5 SLOW, 10 MEDIUM, 20 FAST -- NVOFA's own NV_OF_PERF_LEVEL values, so a
    // report quoting this number can be read against NVIDIA's table directly.
    int ofa_perf;
    // How the Vulkan transport puts its result into the back buffer: 0 copies
    // when the image allows it and draws otherwise, 1 copies only, 2 draws
    // always. ReShade's layer gives every swapchain COLOR_ATTACHMENT and
    // TRANSFER_SRC and nothing else, so an image a game created without
    // TRANSFER_DST can be drawn into and not copied into (#23).
    int vk_present;
    // How the Vulkan transport of the SUBSTITUTE contract orders itself against
    // the private D3D12 device: 0 parks the game's queue on a Vulkan event while
    // a worker thread runs the optical flow and the evaluate, 1 waits the queue
    // idle on the CPU and runs both on the present thread, which is what 1.4.8
    // did, and 2 pipelines -- one frame's evaluate runs while the game renders
    // the next, so the wait the queue reaches is on work that started a frame
    // ago. 0 is the default because 1 costs a full CPU-GPU serialisation per
    // frame -- Ghost Recon Breakpoint measured ~150 fps of its own against ~75
    // with the substitute and the GPU 46% busy (#23). 1 is the fallback if a
    // driver does not like the park; 2 is opt-in because it shows the result one
    // present later and carries a second Output texture; the mirror's own park is
    // not affected by this key.
    int vk_sync;
    // Diagnostic; see UnwrapList. 0 hands NGX the command list as it comes.
    int unwrap_list;
    // What this add-on does about a driver whose NGX loader drives neural
    // rendering into a snippet that faults: 0 closes that route in memory and
    // leaves everything else alone, 1 does nothing, 2 loads the previous
    // driver's loader instead. See PreferWorkingNgxLoader.
    int ngx_loader;
    // 1 builds the mirrored feature at the render region the game declares on
    // each evaluate (DLSS.Render.Subrect.Dimensions) rather than at the size it
    // created the feature with. Off by default; see the note where it applies.
    int shape;
    // Diagnostic: hold the private D3D12 queue for this many ms, once, at the
    // 60th submission. 0 is off. See the stall fields in bridge.h.
    int stall_test;
};

// auto is not a fourth point on a scale with the other three, so this is stored
// as a word in the file: "source=1" would have to be looked up to be read, and
// the value it names is the one thing in this file a user is most likely to be
// told to change by someone else.
enum CfgSource { CFG_SRC_AUTO, CFG_SRC_MIRROR, CFG_SRC_SYNTH, CFG_SRC_OFF };

static const char *kCfgSourceName[] = { "auto", "mirror", "synth", "off" };

static int ParseSourceWord(const char *w, int fallback)
{
    for (int i = 0; i < static_cast<int>(_countof(kCfgSourceName)); ++i)
        if (_stricmp(w, kCfgSourceName[i]) == 0) return i;
    return fallback;   // an unreadable value is not a request to change anything
}

static BridgeCfg g_cfg = { 2, -1, 1, 0, 0, 3, 1, 1, 1, 1, CFG_SRC_AUTO, 0, 1, 0, 0, 0, 2, 20, 0, 0, 0, 0, 0, 0 };

// synth_after answers one question and used to answer two. It is still the
// opt-in for synthesising a contract in a game that has never called DLSS --
// see the measurement in SynthArmRefusal for why that cannot be automatic --
// but it no longer gates the fallback for a game whose own DLSS is switched
// off, which is decided by evidence rather than by a key.
//
// The seconds themselves: at or below zero resolves to the default, so a file
// that only ever said 0 needs no edit once the opt-in is given elsewhere.
static const int kSynthAfterDefaultSec = 10;

static int SynthAfterSec()
{
    return g_cfg.synth_after > 0 ? g_cfg.synth_after : kSynthAfterDefaultSec;
}

// The one gate on the whole synthetic path. synth_after > 0 implies it: that key
// was the opt-in for the pre-arm case before this one existed, and a
// configuration file already carrying it is a user who has asked, so an upgrade
// must not quietly stop doing what they set it to do.
static bool SynthOptedIn()
{
    return g_cfg.synth != 0 || g_cfg.synth_after > 0;
}

// CfgReload's one-second gate, promoted out of that function so the panel can
// reach it. Two uses, both of them reads of a clock rather than of a setting:
// zeroing it after a panel write makes the next delivered frame re-read the file
// instead of waiting the second out, and a value still zero seconds after a
// write is how the panel tells "the value was rejected" from "nothing has called
// CfgReload at all". It is a gate, not a setting: g_cfg keeps exactly one writer.
static volatile LONGLONG g_cfg_last_read;

// Accumulates the two figures and reports them together, because either one
// alone is misleading.
// Whether this process owns the foreground window, asked once per report rather
// than per frame. An out-of-focus window has now been read as a performance
// finding three separate times in one day of log analysis: the game throttles,
// the bridge's share of a frame moves, and the number looks like a regression
// that is not there. A figure that cannot be read in isolation should not be
// printed as though it can.
static bool ForegroundIsThisProcess()
{
    HWND fg = GetForegroundWindow();
    if (fg == nullptr) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static void TimingTick(LONGLONG entry, LONGLONG exit)
{
    if (g_bridge.qpf == 0)
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_bridge.qpf = f.QuadPart;
        g_bridge.span_start = entry;
    }

    g_bridge.cpu_ticks += (exit - entry);

    // The gap between one frame's arrival and the next. Averaged over 600
    // frames this says nothing about smoothness; the widest and narrowest do.
    if (g_bridge.last_entry != 0)
    {
        const LONGLONG iv = entry - g_bridge.last_entry;
        if (g_bridge.iv_min == 0 || iv < g_bridge.iv_min) g_bridge.iv_min = iv;
        if (iv > g_bridge.iv_max) g_bridge.iv_max = iv;
    }
    g_bridge.last_entry = entry;

    if (++g_bridge.timed_frames < 600) return;

    const double span_ms = 1000.0 * double(exit - g_bridge.span_start) / double(g_bridge.qpf);
    const double cpu_ms  = 1000.0 * double(g_bridge.cpu_ticks) / double(g_bridge.qpf);
    const double n       = double(g_bridge.timed_frames);

    // How far behind the D3D12 side is running. The game's queue is made to wait
    // for these values, so a gap that grows instead of staying at a frame or two
    // is the transport stalling -- and the last line before a log stops dead is
    // then evidence rather than an inference.
    const UINT64 done = g_bridge.fence12 != nullptr
                      ? g_bridge.fence12->GetCompletedValue() : 0;
    if (done == UINT64_MAX)
    {
        Log("[bridge] %.0f frames: bridge CPU %.2f ms/frame | frame interval %.2f ms "
            "(%.1f fps) | spread %.2f-%.2f ms | d3d12 DEVICE REMOVED",
            n, cpu_ms / n, span_ms / n, 1000.0 / (span_ms / n),
            1000.0 * double(g_bridge.iv_min) / double(g_bridge.qpf),
            1000.0 * double(g_bridge.iv_max) / double(g_bridge.qpf));
        g_bridge.iv_min = 0; g_bridge.iv_max = 0;
        g_bridge.cpu_ticks = 0; g_bridge.timed_frames = 0; g_bridge.span_start = exit;
        g_bridge.ofa_ticks = 0; g_bridge.ofa_frames = 0;
        return;
    }

    const double lo = 1000.0 * double(g_bridge.iv_min) / double(g_bridge.qpf);
    const double hi = 1000.0 * double(g_bridge.iv_max) / double(g_bridge.qpf);

    // The synthetic path's optical flow, as a clause that is empty whenever no
    // flow ran. In the nine shipped titles ofa_frames is always 0, so the line
    // below is byte-identical to every one already in a mirror log. It carries
    // its own denominator on purpose: if that count disagrees with the leading
    // frame count, flow ran on frames that were not delivered, and the line says
    // so rather than quietly averaging over the wrong number.
    char ofa[80] = "";
    if (g_bridge.ofa_frames != 0)
        _snprintf_s(ofa, sizeof(ofa), _TRUNCATE,
                    " | optical flow %.2f ms over %llu frames",
                    1000.0 * double(g_bridge.ofa_ticks) / double(g_bridge.qpf)
                           / double(g_bridge.ofa_frames),
                    g_bridge.ofa_frames);

    Log("[bridge] %.0f frames: bridge CPU %.2f ms/frame | frame interval %.2f ms (%.1f fps) "
        "| spread %.2f-%.2f ms | bridge is %.0f%% of the frame | d3d12 %llu/%llu (%lld behind)%s%s",
        n, cpu_ms / n, span_ms / n, 1000.0 / (span_ms / n), lo, hi,
        100.0 * cpu_ms / span_ms, done, g_bridge.fence_value,
        static_cast<long long>(g_bridge.fence_value) - static_cast<long long>(done),
        ofa,
        ForegroundIsThisProcess() ? "" : "  <== WINDOW NOT IN FOCUS, do not read as steady state");

    // This measures the gap between the game's own DLSS calls, so a loading
    // screen, an alt-tab or a paused game all land here as one enormous frame --
    // Baldur's Gate 3 produced 13370 ms and carried on for thousands of frames
    // afterwards. It is reported because Windows resets a GPU that stops
    // answering for about two seconds, and Escape from Tarkov recorded 2534 ms
    // eight seconds before losing its device. The figure alone does not say
    // which of the two happened.
    if (hi >= 1500.0)
        Log("[bridge]   one frame took %.0f ms. Usually that is a loading screen, "
            "an alt-tab or a pause, since this measures the gap between the game's "
            "own DLSS calls. It is noted because Windows resets a GPU that stops "
            "answering for about two seconds, so the same figure appears before a "
            "device removal.", hi);

    g_bridge.iv_min       = 0;
    g_bridge.iv_max       = 0;
    g_bridge.cpu_ticks    = 0;
    g_bridge.timed_frames = 0;
    g_bridge.ofa_ticks    = 0;
    g_bridge.ofa_frames   = 0;
    g_bridge.span_start   = exit;
}

// True only when skipping the game's own DLSS cannot leave a stale frame on
// screen: the bridge must be healthy and already have produced a valid output.
// This is only ever asked in order to suppress the game's own DLSS evaluate, so
// the frames it counts have to be frames that replace it. During a handover the
// flags below still read healthy while the frames behind them were produced by
// somebody else, and skipping the game on that evidence leaves the last mirrored
// image on screen under live audio and live input.
static bool BridgeWillDeliver(const NVSDK_NGX_Handle *handle)
{
    EnterCriticalSection(&g_bridge_cs);
    bool known = false;
    for (const auto &history : g_bridge.histories)
        if (history.feature != nullptr && history.game == handle) known = true;
    const bool deliver = known && g_source == SRC_MIRROR &&
           g_cfg.skip_game != 0 && g_cfg.mode == 2 && g_cfg.stage >= 3 &&
           !g_bridge.disabled && !g_bridge.stalled && g_bridge.session_ready && g_bridge.frame_ready &&
           !g_bridge.partial_output && g_bridge.frames_done > 0;
    LeaveCriticalSection(&g_bridge_cs);
    return deliver;
}

// The add-on was called dlss5-dx11-bridge up to and including 1.1.0 -- the rename
// landed in 1.2.0 -- so a user upgrading has a
// cfg under the old name with their settings in it. Take the new name when it
// exists, the old one when only that does, and the new name when neither does,
// so a fresh install writes the current one. Silently reverting somebody to
// defaults because a file was renamed is not an upgrade anyone forgives.
enum CfgWhich { CFG_NEW_NAME, CFG_OLD_NAME, CFG_NONE };

static CfgWhich CfgPathWhich(char *out)
{
    GetModuleFileNameA(g_self, out, MAX_PATH);
    char *s = strrchr(out, '\\');
    if (s == nullptr) return CFG_NONE;
    const size_t room = MAX_PATH - (s + 1 - out);

    strcpy_s(s + 1, room, "dlss5-bridge.cfg");
    if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return CFG_NEW_NAME;

    strcpy_s(s + 1, room, "dlss5-dx11-bridge.cfg");
    if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return CFG_OLD_NAME;

    strcpy_s(s + 1, room, "dlss5-bridge.cfg");
    return CFG_NONE;
}

static void CfgPath(char *out) { CfgPathWhich(out); }

// Which file is in force, said once. Three of this add-on's own messages name
// dlss5-bridge.cfg by hand -- the flags=107 substitution, its "set flags=-1"
// advice, and the panel's -- and an installation first made before 1.2.0 has
// only dlss5-dx11-bridge.cfg, which CfgWriteDefault will never replace because
// something matching CfgPath already exists. So the advice names a file that is
// not in that folder, and creating it by hand strands the other fifteen
// settings, because the new name then wins and the old file is ignored whole.
//
// Nothing here writes or renames anything. It states which file was read.
static void CfgSayWhich()
{
    static bool said = false;
    if (said) return;
    said = true;

    char path[MAX_PATH];
    const CfgWhich which = CfgPathWhich(path);
    if (which == CFG_NONE)
    {
        Log("[bridge] no configuration file beside this add-on; defaults are in force.");
        return;
    }
    Log("[bridge] settings read from %s", path);
    if (which == CFG_OLD_NAME)
    {
        Log("[bridge]   that is the name this add-on used before 1.2.0. It is read as it "
            "stands and nothing is rewritten -- but every message that names "
            "dlss5-bridge.cfg means this file. Renaming it is safe; creating a second "
            "file under the current name is not, because that one then wins and this "
            "one is ignored whole.");
        return;
    }

    char other[MAX_PATH];
    strcpy_s(other, path);
    char *sl = strrchr(other, '\\');
    if (sl == nullptr) return;
    strcpy_s(sl + 1, MAX_PATH - (sl + 1 - other), "dlss5-dx11-bridge.cfg");
    if (GetFileAttributesA(other) != INVALID_FILE_ATTRIBUTES)
        Log("[bridge]   dlss5-dx11-bridge.cfg is also present and is being ignored "
            "whole -- the current name wins. Anything set only in the old file has no "
            "effect.");
}

static void CfgReload()
{
    // Called from the frame path, which used to throttle on a frame counter that
    // stops moving the moment the bridge stops delivering. A game that never
    // delivers -- Watch Dogs 2 at MSAA -- then opened and parsed this file on the
    // render thread every single frame, inside the lock the game's own threads
    // contend for. The clock does not stop when the counter does.
    // ponytail: one second, fixed; a per-caller rate only if one ever earns it.
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG last_read = static_cast<ULONGLONG>(g_cfg_last_read);
    if (last_read != 0 && now - last_read < 1000) return;
    g_cfg_last_read = static_cast<LONGLONG>(now);

    char path[MAX_PATH];
    CfgPath(path);

    FILE *f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || f == nullptr) return;

    BridgeCfg next = g_cfg;
    char line[128];
    while (fgets(line, sizeof(line), f) != nullptr)
    {
        char key[64];
        int  val = 0;
        if (sscanf_s(line, "%63[^=]=%d", key, static_cast<unsigned>(sizeof(key)), &val) != 2)
        {
            // Only source= takes a word, so the numeric scan stays exactly as it
            // was and a line it rejects gets one more chance rather than a
            // rewritten parser -- every other key in this file is a number, and
            // the ones the nine shipped titles depend on are all in that set.
            char word[16];
            if (sscanf_s(line, "%63[^=]=%15s", key, static_cast<unsigned>(sizeof(key)),
                         word, static_cast<unsigned>(sizeof(word))) == 2 &&
                _stricmp(key, "source") == 0)
                next.source = ParseSourceWord(word, next.source);
            continue;
        }
        if      (_stricmp(key, "mode")        == 0) next.mode        = val;
        else if (_stricmp(key, "flags")       == 0) next.flags       = val;
        else if (_stricmp(key, "dred")        == 0) next.dred        = val;
        else if (_stricmp(key, "skip_exe")    == 0) next.skip_exe    = val;
        else if (_stricmp(key, "unwrap")      == 0) next.unwrap      = (val >= 0 && val <= 2) ? val : 1;
        else if (_stricmp(key, "subrects")    == 0) next.subrects    = val;
        else if (_stricmp(key, "reset_every") == 0) next.reset_every = val;
        else if (_stricmp(key, "pixels")      == 0) next.pixels      = val;
        else if (_stricmp(key, "stage")       == 0) next.stage       = val;
        else if (_stricmp(key, "skip_game")   == 0) next.skip_game   = val;
        else if (_stricmp(key, "synth_after") == 0) next.synth_after = val;
        else if (_stricmp(key, "synth")       == 0) next.synth       = val;
        else if (_stricmp(key, "hash_out")    == 0) next.hash_out    = val;
        else if (_stricmp(key, "ofa_grid")    == 0) next.ofa_grid    = val;
        else if (_stricmp(key, "ofa_perf")    == 0) next.ofa_perf    = val;
        else if (_stricmp(key, "vk_present")  == 0) next.vk_present  = (val >= 0 && val <= 2) ? val : 0;
        else if (_stricmp(key, "unwrap_list") == 0) next.unwrap_list = (val >= 0 && val <= 1) ? val : 0;
        else if (_stricmp(key, "ngx_loader")  == 0) next.ngx_loader  = (val >= 0 && val <= 2) ? val : 0;
        else if (_stricmp(key, "shape")       == 0) next.shape       = val == 1 ? 1 : 0;
        else if (_stricmp(key, "stall_test")  == 0) next.stall_test  = val > 0 ? val : 0;
        else if (_stricmp(key, "vk_sync")     == 0) next.vk_sync     = (val >= 0 && val <= 2) ? val : 0;
        // Only the sign is taken, because only the sign is unknown: the
        // magnitude is the back buffer size and this add-on measures that. A
        // number here that is not a sign would be a second way to say something
        // already derived, and the two would disagree.
        // Three-valued, not two. 0 means "use whatever the motion-vector provider
        // documents", which for vort_Shaders is a fact read out of MIT source rather
        // than a guess; -1 and +1 force, and forcing still wins, because a real
        // installation is where a documented convention gets contradicted.
        else if (_stricmp(key, "mv_sign_x")   == 0) next.mv_sign_x   = (val < 0) ? -1 : (val > 0 ? 1 : 0);
        else if (_stricmp(key, "mv_sign_y")   == 0) next.mv_sign_y   = (val < 0) ? -1 : (val > 0 ? 1 : 0);
    }
    fclose(f);
    if (next.stage < 0 || next.stage > 3) next.stage = g_cfg.stage;

    if (next.mode < 0 || next.mode > 2) next.mode = g_cfg.mode;

    // Same rule stage and mode already follow: a value that is not one of the
    // four the driver was measured with is not a request to change anything.
    if (next.ofa_grid != 0 && next.ofa_grid != 1 && next.ofa_grid != 2 && next.ofa_grid != 4)
        next.ofa_grid = g_cfg.ofa_grid;
    // Anything else is a typo rather than a request; keep what was working.
    if (next.ofa_perf != 5 && next.ofa_perf != 10 && next.ofa_perf != 20)
        next.ofa_perf = g_cfg.ofa_perf;

    // Every field, not a chosen few. dred, skip_exe and unwrap were absent here,
    // so editing one of them mid-session was applied and never reported -- the
    // config line then described a file it no longer matched, which is worse
    // than not printing one. A key worth reading is worth comparing.
    const bool rebuild = (next.flags != g_cfg.flags) || (next.subrects != g_cfg.subrects);
    const bool changed = rebuild || (next.mode != g_cfg.mode) ||
                         (next.reset_every != g_cfg.reset_every) || (next.pixels != g_cfg.pixels) ||
                         (next.stage != g_cfg.stage) || (next.skip_game != g_cfg.skip_game) ||
                         (next.dred != g_cfg.dred) || (next.skip_exe != g_cfg.skip_exe) ||
                         (next.unwrap != g_cfg.unwrap) || (next.source != g_cfg.source) ||
                         (next.synth_after != g_cfg.synth_after) ||
                         (next.synth != g_cfg.synth) || (next.hash_out != g_cfg.hash_out) ||
                         (next.mv_sign_x != g_cfg.mv_sign_x) ||
                         (next.mv_sign_y != g_cfg.mv_sign_y) ||
                         (next.ofa_grid != g_cfg.ofa_grid) ||
                         (next.ofa_perf != g_cfg.ofa_perf) ||
                         (next.vk_present != g_cfg.vk_present) ||
                         (next.vk_sync != g_cfg.vk_sync) ||
                         (next.unwrap_list != g_cfg.unwrap_list) ||
                         (next.ngx_loader != g_cfg.ngx_loader) ||
                         (next.shape != g_cfg.shape) ||
                         (next.stall_test != g_cfg.stall_test);
    // The first read reports whatever it found, changed or not. Until now the echo
    // fired only on a difference from the built-in defaults, so a session running on
    // defaults had no config line at all -- and "no config line" then meant either
    // "defaults" or "the file was never read", which are not the same thing to
    // anyone triaging a report.
    static bool said_once = false;
    if (!changed && said_once) return;
    said_once = true;

    g_cfg = next;
    Log("[bridge] config: stage=%d mode=%d flags=%d subrects=%d reset_every=%d pixels=%d "
        "skip_game=%d dred=%d skip_exe=%d unwrap=%d source=%s synth=%d hash_out=%d synth_after=%d "
        "mv_sign_x=%d mv_sign_y=%d ofa_grid=%d ofa_perf=%d vk_present=%d vk_sync=%d",
        g_cfg.stage, g_cfg.mode, g_cfg.flags, g_cfg.subrects, g_cfg.reset_every, g_cfg.pixels,
        g_cfg.skip_game, g_cfg.dred, g_cfg.skip_exe, g_cfg.unwrap,
        kCfgSourceName[g_cfg.source], g_cfg.synth, g_cfg.hash_out, g_cfg.synth_after,
        g_cfg.mv_sign_x, g_cfg.mv_sign_y, g_cfg.ofa_grid, g_cfg.ofa_perf, g_cfg.vk_present,
        g_cfg.vk_sync);

    // Creation-time values only take effect on a new feature, so invalidate the
    // current one and let the next frame build it again.
    if (rebuild) { Log("[bridge] rebuilding feature for new creation flags"); g_bridge.frame_ready = false; }
}

// Writes the file at path from g_cfg, unconditionally. The first line is the
// version that wrote it, which CfgRegenerateIfStale reads back.
static bool CfgWriteFile(const char *path)
{
    FILE *f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || f == nullptr) return false;
    // Written with the two keys a user actually has to find, and a sentence each
    // for the rest. The file used to be a bare list of sixteen numbers, and
    // vk_mirror -- which decides whether half this add-on exists on a Vulkan
    // game -- was not in it at all: it is read straight from the file, so a user
    // who never knew the name got nothing and no way to find out why.
    //
    // The comment lines are safe to write here. The parser matches key=value and
    // ignores anything else, and CfgKeyOn/CfgKeyInt match at the start of a line,
    // so a '#' line cannot switch anything on.
    fprintf(f,
        "# dlss5-bridge " BRIDGE_VERSION "\n"
        "# Settings. Re-read about once a second while the game runs, so most take\n"
        "# effect without a restart. Delete this file to get it back with defaults.\n"
        "#\n"
        "# The first line is the version that wrote this file. A different version\n"
        "# replaces the file with its own defaults the first time it runs, and says\n"
        "# so in the log; the same version never touches it. Change that line to\n"
        "# \"# dlss5-bridge keep\" to keep this file across versions.\n"
        "\n"
        "# THE TWO THAT MATTER\n"
        "#\n"
        "# vk_mirror  1 hooks the Vulkan NGX entry points as well, which is what\n"
        "#            lets a Vulkan game's own DLSS be mirrored. Harmless on a\n"
        "#            DirectX game, which has none to hook. Read once at launch.\n"
        "# synth      1 allows a SUBSTITUTE DLSS contract, built out of ReShade's\n"
        "#            depth and the NVIDIA driver's optical flow, when the game\n"
        "#            provides none of its own. Off by default, and it should stay\n"
        "#            off unless you want it: the result is visibly worse than real\n"
        "#            DLSS. Optical flow does not know what a glyph or a leaf is, so\n"
        "#            text goes soft and dense foliage smears. Red Dead Redemption 2\n"
        "#            looks like it is melting; Baldur's Gate 3 holds together but\n"
        "#            its text does not. That is the technique, not a bug.\n"
        "#\n"
        "#            It covers both cases: a game with no DLSS at all, and a game\n"
        "#            whose own DLSS you switch off in its menu.\n"
        "# synth_after  with synth=1, and ONLY for a game that has no DLSS of its\n"
        "#            own: seconds to wait before building the contract without\n"
        "#            ever having seen the game ask. Do not set it in a game that\n"
        "#            HAS DLSS -- arming before the game asks costs the mirror the\n"
        "#            whole session when it then does. Setting it also turns synth\n"
        "#            on, so a file that already had it keeps working.\n"
        "vk_mirror=%d\n"
        "synth=%d\n"
        "synth_after=%d\n"
        "\n"
        "# WHICH SOURCE MAY HOLD THE SESSION\n"
        "# auto lets the game's own DLSS win and falls back to the synthetic\n"
        "# contract; mirror pins the game's own and is how the fallback is turned\n"
        "# off; synth pins the synthetic one; off disables both.\n"
        "source=%s\n"
        "\n"
        "# THE DRIVER'S OPTICAL FLOW ENGINE, used by the synthetic contract only.\n"
        "# ofa_grid 0 off -- a ReShade motion-vector shader is then required -- or\n"
        "# 1, 2, 4 finest to cheapest. ofa_perf 5 SLOW, 10 MEDIUM, 20 FAST.\n"
        "ofa_grid=%d\n"
        "ofa_perf=%d\n"
        "# Force the motion-vector direction instead of the provider's own\n"
        "# convention. 0 leaves it alone; -1 or 1 forces it. For diagnosis.\n"
        "mv_sign_x=%d\n"
        "mv_sign_y=%d\n"
        "# How the Vulkan transport puts its result into the back buffer: 0 copies\n"
        "# where the image allows it and draws it otherwise, 1 copies only, 2 draws\n"
        "# always.\n"
        "vk_present=%d\n"
        "# How the substitute's Vulkan transport orders itself against the private\n"
        "# D3D12 device: 0 parks the game's queue on a Vulkan event while a worker\n"
        "# thread runs the optical flow and the evaluate, 1 waits the queue idle on\n"
        "# the CPU instead, which is slower and is the fallback if 0 misbehaves, and\n"
        "# 2 pipelines -- one frame's evaluate runs while the game renders the next,\n"
        "# so the wait the queue reaches is on work that started a frame ago, at the\n"
        "# price of showing the result one present later (about 17 ms at 60 fps) and\n"
        "# one more Output texture. 0 is the default and has neither cost.\n"
        "vk_sync=%d\n"
        "\n"
        "# THE REST. stage 0 switches the add-on off without removing the file.\n"
        "# flags -1 copies the create flags the game declared; any other value\n"
        "# forces one, except 107, which is treated as unset.\n"
        "stage=%d\n"
        "mode=%d\n"
        "skip_game=%d\n"
        "flags=%d\n"
        "subrects=%d\n"
        "reset_every=%d\n"
        "pixels=%d\n"
        "dred=%d\n"
        "skip_exe=%d\n"
        "unwrap=%d\n",
        1, g_cfg.synth, g_cfg.synth_after,
        kCfgSourceName[g_cfg.source], g_cfg.ofa_grid, g_cfg.ofa_perf,
        g_cfg.mv_sign_x, g_cfg.mv_sign_y, g_cfg.vk_present, g_cfg.vk_sync,
        g_cfg.stage, g_cfg.mode, g_cfg.skip_game, g_cfg.flags, g_cfg.subrects,
        g_cfg.reset_every, g_cfg.pixels, g_cfg.dred, g_cfg.skip_exe, g_cfg.unwrap);
    fclose(f);
    return true;
}

static void CfgWriteDefault()
{
    char path[MAX_PATH];
    CfgPath(path);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return;
    if (CfgWriteFile(path)) Log("[bridge] wrote default config to %s", path);
}

// Once at attach, before the file is read. A settings file stamped by this
// exact version is never touched. One stamped by any other version, one with
// no stamp, or one under the name used before 1.2.0 is replaced under the
// current name with this version's defaults, and the log says so.
//
// Nothing is carried over, on purpose. A file tuned for another build, or
// carrying keys this one no longer has, is exactly what this rules out: three
// releases in a row spent a sentinel on a value an older default had written
// into every file (flags=107, unwrap=1), and one report was an old file that
// nobody had looked at. A user who wants a file kept across versions writes
// "# dlss5-bridge keep" as its first line.
static void CfgRegenerateIfStale()
{
    char path[MAX_PATH];
    const CfgWhich which = CfgPathWhich(path);
    if (which == CFG_NONE) return;

    char first[96] = "";
    FILE *f = nullptr;
    if (fopen_s(&f, path, "r") == 0 && f != nullptr)
    {
        if (fgets(first, sizeof(first), f) == nullptr) first[0] = 0;
        fclose(f);
    }
    first[strcspn(first, "\r\n")] = 0;
    const char *stamp = strncmp(first, "# dlss5-bridge ", 15) == 0 ? first + 15 : nullptr;
    if (stamp != nullptr && (strcmp(stamp, BRIDGE_VERSION) == 0 || strcmp(stamp, "keep") == 0))
        return;

    const char *old_name = which == CFG_OLD_NAME ? "dlss5-dx11-bridge.cfg" : "dlss5-bridge.cfg";
    char *sl = strrchr(path, (char)92);
    if (sl == nullptr) return;
    strcpy_s(sl + 1, MAX_PATH - (sl + 1 - path), "dlss5-bridge.cfg");

    if (!CfgWriteFile(path))
    {
        Log("[bridge] %s was written by %s%s and could not be replaced (error %lu); "
            "it is read as it stands.", old_name,
            stamp != nullptr ? "" : "a version before 1.4.0",
            stamp != nullptr ? stamp : "", GetLastError());
        return;
    }
    Log("[bridge] %s was written by %s%s and has been replaced with the defaults of "
        BRIDGE_VERSION ". Nothing is carried over: a file from another version is "
        "what this rules out. \"# dlss5-bridge keep\" as its first line keeps a "
        "file across versions.", old_name,
        stamp != nullptr ? "" : "a version before 1.4.0",
        stamp != nullptr ? stamp : "");
    if (which == CFG_OLD_NAME)
        Log("[bridge]   dlss5-dx11-bridge.cfg is left in place and ignored; it can be deleted.");
}

// The status panel's only writer, and the only place in this add-on that writes
// a file a user owns. It rewrites ONE key and leaves every other byte --
// comments, keys this build has never heard of, ordering, CRLF -- exactly as it
// found them: the file is the single source of truth and this add-on is not its
// author. That is also why CfgWriteDefault is not reused as the writer; its one
// fprintf prints the struct and would delete everything else in the file.
//
// The read happens one instruction before the write, so an edit a user saved
// from a text editor two frames ago is in the buffer being rewritten rather than
// being overwritten by a snapshot taken at start-up. Nothing here touches g_cfg:
// CfgReload picks this up from the file on the next delivered frame, through the
// same parser, the same validators and the same change detector a hand edit goes
// through, which is what keeps one source of truth and one writer of g_cfg.
// ponytail: last writer wins inside this one call; no lock file, no watcher.
// The one fiddly part of the writer, kept pure so it can be run on its own: no
// file, no globals, in and out are buffers. Copies every line of in verbatim,
// replaces the value on the first line whose key matches, appends the key if no
// line matched, and returns the length written -- or 0 if the result would not
// fit, which the caller reports rather than truncating a user's file.
//
// The key comparison is the text before the first '=', trimmed of spaces, tabs
// and a CR, so a CRLF file and an indented line both match. Line endings are
// carried through byte for byte because a line is copied with its own newline;
// only the line this writes uses \n.
static size_t CfgRewriteLine(const char *in, size_t n, const char *key, const char *value,
                             char *out, size_t out_n)
{
    size_t o = 0;
    bool   done = false;
    const size_t klen = strlen(key);
    for (size_t i = 0; i < n; )
    {
        size_t e = i;
        while (e < n && in[e] != '\n') ++e;
        const size_t len = (e < n) ? (e - i + 1) : (e - i);   // the newline stays with the line

        size_t k0 = i;
        while (k0 < e && (in[k0] == ' ' || in[k0] == '\t')) ++k0;
        size_t k1 = k0;
        while (k1 < e && in[k1] != '=') ++k1;
        size_t k2 = k1;
        while (k2 > k0 && (in[k2 - 1] == ' ' || in[k2 - 1] == '\t' || in[k2 - 1] == '\r')) --k2;

        if (!done && k1 < e && (k2 - k0) == klen && _strnicmp(in + k0, key, klen) == 0)
        {
            const int w = _snprintf_s(out + o, out_n - o, _TRUNCATE, "%s=%s\n", key, value);
            if (w < 0) return 0;
            o += static_cast<size_t>(w);
            done = true;
        }
        else
        {
            if (o + len >= out_n) return 0;
            memcpy(out + o, in + i, len);
            o += len;
        }
        i = (e < n) ? e + 1 : e;
    }
    if (!done)
    {
        // A file whose last line has no newline of its own -- which a text
        // editor is perfectly happy to save -- would otherwise have the appended
        // key run onto the end of it, producing one line that parses as neither.
        if (o > 0 && out[o - 1] != '\n')
        {
            if (o + 1 >= out_n) return 0;
            out[o++] = '\n';
        }
        const int w = _snprintf_s(out + o, out_n - o, _TRUNCATE, "%s=%s\n", key, value);
        if (w < 0) return 0;
        o += static_cast<size_t>(w);
    }
    return o;
}

static bool CfgSetKey(const char *key, const char *value, char *why, size_t why_n)
{
    char path[MAX_PATH];
    CfgPath(path);

    // Normally a no-op, because it returns early when the file exists. If the
    // file was deleted mid-session it comes back with ALL sixteen keys at this
    // session's current values -- it prints g_cfg, not build defaults -- and
    // writing only our one line into a vanished file would instead leave fifteen
    // keys absent, which is invisible now and reverts a user's whole tuning on
    // the next launch.
    CfgWriteDefault();

    char   buf[4096];
    size_t n = 0;
    FILE  *f = nullptr;
    if (fopen_s(&f, path, "rb") == 0 && f != nullptr)
    {
        n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
    }
    // Most editors save by truncate-then-write, so a zero-byte read of a file
    // that exists means mid-save rather than empty. Refusing costs one click.
    if (n == 0)
    {
        strcpy_s(why, why_n, "the config file could not be read just now; try again");
        return false;
    }
    // A writer that silently truncates at its buffer would destroy an annotated
    // config. Refuse and say so.
    if (n == sizeof(buf))
    {
        strcpy_s(why, why_n,
                 "this file is larger than the panel will rewrite; change it in a text editor");
        return false;
    }
    buf[n] = 0;

    char   out[4352];
    const size_t o = CfgRewriteLine(buf, n, key, value, out, sizeof(out));
    if (o == 0)
    {
        strcpy_s(why, why_n,
                 "this file is larger than the panel will rewrite; change it in a text editor");
        return false;
    }

    // Temp plus rename, so a failure leaves the original byte-for-byte intact
    // and CfgReload on the other thread never sees a half-written file.
    char tmp[MAX_PATH + 8];
    _snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s.tmp", path);

    FILE *w = nullptr;
    if (fopen_s(&w, tmp, "wb") != 0 || w == nullptr)
    {
        _snprintf_s(why, why_n, _TRUNCATE,
                    "dlss5-bridge.cfg could not be written (error %lu)", GetLastError());
        return false;
    }
    const bool wrote = fwrite(out, 1, o, w) == o;
    fflush(w);
    fclose(w);
    if (!wrote)
    {
        DeleteFileA(tmp);
        strcpy_s(why, why_n, "dlss5-bridge.cfg could not be written in full");
        return false;
    }

    // CfgReload's own fopen can hold the target at this instant -- MSVC's fopen
    // does not ask for FILE_SHARE_DELETE -- so a sharing violation here is a
    // race with a reader rather than a broken folder. Three tries, no sleep.
    DWORD err = 0;
    for (int t = 0; t < 3; ++t)
    {
        if (MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
        {
            // Not a setting: the reload gate, so the next delivered frame reads
            // the file instead of waiting the one-second rate limit out.
            InterlockedExchange64(reinterpret_cast<volatile LONG64 *>(&g_cfg_last_read), 0);
            // Once per click, never per frame: Log takes g_log_cs and the render
            // thread takes it too.
            Log("[panel] wrote %s=%s to %s", key, value, path);
            return true;
        }
        err = GetLastError();
    }
    DeleteFileA(tmp);
    _snprintf_s(why, why_n, _TRUNCATE, "the config file was busy; try again (error %lu)", err);
    return false;
}

// Reads one pixel back to the CPU. Blocking and slow, so only used on the first
// few frames, but it settles what the buffers actually contain instead of
// guessing from what the screen looks like.
//
// The coordinate is a parameter and the value is returned rather than logged,
// because synth.inc asks the same question of a texture that is not the mirror's
// and at points that are not its centre. Two readbacks would be two sets of
// staging-texture and Map mistakes to make; this stays the only one.
static bool ReadPixelRaw(ID3D11DeviceContext *ctx, ID3D11Device *dev11,
                         ID3D11Texture2D *tex, DXGI_FORMAT fmt, UINT x, UINT y, UINT *out)
{
    if (tex == nullptr || ctx == nullptr || dev11 == nullptr || out == nullptr) return false;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = 1;
    td.Height           = 1;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = fmt;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_STAGING;
    td.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;

    ID3D11Texture2D *staging = nullptr;
    if (FAILED(dev11->CreateTexture2D(&td, nullptr, &staging)) || staging == nullptr) return false;

    D3D11_BOX box = { x, y, 0, x + 1, y + 1, 1 };
    ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, &box);

    bool got = false;
    D3D11_MAPPED_SUBRESOURCE ms = {};
    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &ms)) && ms.pData != nullptr)
    {
        *out = *static_cast<const UINT *>(ms.pData);
        got  = true;
        ctx->Unmap(staging, 0);
    }
    staging->Release();
    return got;
}

static void LogCenterPixel(ID3D11DeviceContext *ctx, ID3D11Device *dev11,
                           const char *label, ID3D11Texture2D *tex, DXGI_FORMAT fmt)
{
    UINT raw = 0;
    if (ReadPixelRaw(ctx, dev11, tex, fmt, g_bridge.width / 2, g_bridge.height / 2, &raw))
        Log("[bridge]   %-8s centre pixel raw = 0x%08X", label, raw);
}

// Every path that gives up goes through here, so the user is told once, in the
// overlay, that the bridge stopped -- otherwise there is simply no effect on
// screen and nothing to explain it without opening a file.
static void BridgeReleaseExposureFwd();

static void BridgeReleaseTextures();

// It is also the only shutdown this add-on has. It used to set a flag and
// nothing else, which left the DLSS feature, four shared textures and their NT
// handles resident for the rest of the session -- while telling the user the
// game was rendering normally.
// How much of the card is spoken for. Escape from Tarkov runs 128-bit-per-pixel
// buffers, which makes every mirror here four times the size of Baldur's Gate
// 3's, and running past the budget is one of the few things that produces a
// reset without a page fault.
static void LogVideoMemory(const char *when)
{
    if (g_bridge.adapter3 == nullptr) return;
    DXGI_QUERY_VIDEO_MEMORY_INFO vm = {};
    if (FAILED(g_bridge.adapter3->QueryVideoMemoryInfo(
            0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm)))
        return;
    Log("[bridge] video memory %s: %llu MB used of %llu MB budget%s", when,
        vm.CurrentUsage / (1024 * 1024), vm.Budget / (1024 * 1024),
        vm.CurrentUsage > vm.Budget ? "  -- over budget" : "");
}

// Device Removed Extended Data. When Windows resets a hung GPU, the only
// account of what it was doing lives in the driver, and DRED is how it is asked
// for: a per-command-list record of how many operations completed, and the
// allocation behind a bad address. Escape from Tarkov reports
// DXGI_ERROR_DEVICE_HUNG about twenty seconds in, and nothing else about it is
// known. This must be switched on before the device exists, so it is done here
// rather than where the removal is noticed.
// ReShade wraps every D3D12 device created through the export, this add-on's
// own included, and then broadcasts init_pipeline, init_sampler and the rest to
// every loaded add-on for everything built from that device. There is no filter
// saying which add-on asked for it.
//
// Luma is a DX11-only framework. Its OnInitDevice is the one handler that does
// return early for a non-D3D11 device, so ReShade never gives it per-device
// data -- and every other handler then dereferences that null. Prey 2017 does
// not opt into the compile-time API guard that would have stopped the rest, so
// NVSDK_NGX_D3D12_Init_Ext creating pipeline state on this device faulted at an
// address inside Luma-Prey.addon.
//
// IID_UnwrappedObject is ReShade's own way out of its proxy, from its
// com_utils.hpp; the D3D12 proxy answers it by AddRef-ing and returning the
// real device. RenoDX asks the same question of every command list before
// forwarding it to NGX, so a native pointer is the case NGX callers prefer.
//
// The proxy is deliberately not released. Nothing releases this device for the
// life of the process, and releasing it would announce destroy_device to those
// same add-ons for a device still in use.
static void UnwrapReShadeProxy(ID3D12Device **dev)
{
    static const GUID kUnwrapped =
        { 0x7f2c9a11, 0x3b4e, 0x4d6a, { 0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42 } };

    ID3D12Device *raw = nullptr;
    if (FAILED((*dev)->QueryInterface(kUnwrapped, reinterpret_cast<void **>(&raw))) ||
        raw == nullptr)
    {
        Log("[bridge] the D3D12 device is not wrapped; nothing to unwrap. On ReShade "
            "older than 6.6 this is expected and the session runs as before.");
        return;
    }
    Log("[bridge] the D3D12 device came back wrapped by ReShade: %p over %p. NGX is "
        "given the unwrapped one, so no other add-on is handed D3D12 objects it "
        "never asked for.", static_cast<void *>(*dev), static_cast<void *>(raw));
    *dev = raw;
}

// The same question asked of the COMMAND LIST rather than the device, and the
// reason it is a separate question. NGX is handed a device at Init and a list at
// every evaluate; ReShade's add-on build wraps both, and a neural snippet that
// takes one from the wrapped side and hands it to the other faults on the
// mismatch. That is the shape measured on 2026-09-04: D3D12Core reading a
// descriptor heap's own description out of what is a ReShade wrapper object,
// inside SetDescriptorHeaps, on NVIDIA 32.0.16.1664 whose loader had just begun
// routing feature 18 into nvngx_dlssnr.dll. Stripping the proxy from the DEVICE
// stops the fault and costs the neural pass, because that build needs the queue
// ReShade announces; this key asks whether stripping only the list is enough.
//
// Diagnostic, off by default, and it stays off until a session says otherwise:
// nothing here knows which of the two ReShade's own conversion expects.
static ID3D12GraphicsCommandList *UnwrapList(ID3D12GraphicsCommandList *list)
{
    static const GUID kUnwrapped =
        { 0x7f2c9a11, 0x3b4e, 0x4d6a, { 0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42 } };
    if (g_cfg.unwrap_list == 0 || list == nullptr) return list;

    ID3D12GraphicsCommandList *raw = nullptr;
    if (FAILED(list->QueryInterface(kUnwrapped, reinterpret_cast<void **>(&raw))) ||
        raw == nullptr)
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            Log("[bridge] unwrap_list=%d, but the command list is not wrapped and there is "
                "nothing to strip. ReShade wraps a list only on its add-on build.",
                g_cfg.unwrap_list);
        }
        return list;
    }
    static bool said = false;
    if (!said)
    {
        said = true;
        Log("[bridge] unwrap_list=%d: NGX is handed the command list underneath ReShade's "
            "proxy, %p rather than %p, while the device it was initialised with keeps "
            "whatever unwrap decided. This is a diagnostic: if it is what makes a session "
            "work, that is worth a report.", g_cfg.unwrap_list,
            static_cast<void *>(raw), static_cast<void *>(list));
    }
    return raw;
}

// Whether to strip ReShade's proxy, decided once for both device paths.
//
// unwrap=1 strips it, and is the shipped default for the reason UnwrapReShadeProxy
// gives: a D3D11-only add-on that skipped init_device for this D3D12 device then
// faulted on the pipeline events the proxy fires. unwrap=0 keeps it.
//
// A DLSS 5 add-on build measured to NEED the proxy overrides the default, once,
// and says so. That build recycles its scratch buffers off the queue submissions
// ReShade announces, and underneath the proxy it sees none: it attached, reported
// active and wrote nothing, on both backends, until the proxy was kept. The
// conflict this creates -- that build beside a D3D11-only add-on that cannot take
// D3D12 events -- has no known instance and no knob; if it ever has one, the two
// lines this logs are where to start.
static void MaybeUnwrap(ID3D12Device **dev)
{
    if (g_cfg.unwrap == 0) return;
    // 2 strips the proxy whatever the two overrides below would have said. Both
    // of them were measured on one driver against one add-on build, and neither
    // can be re-measured while it is unconditional: a user who is told to try
    // unwrap=0 for a build on the needs-proxy list is being told to set the
    // value that is already in force. After NVIDIA 32.0.16.1664 the question
    // reopened -- the wrapped and unwrapped device are two different objects,
    // and a neural snippet that creates its descriptor heaps against one and
    // sets them on the other is the shape of the fault seen there -- so the
    // question needs an answer that is not a rebuild of this add-on.
    if (g_cfg.unwrap == 2)
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            Log("[bridge] unwrap=2: ReShade's proxy is stripped from the D3D12 device even "
                "though something here would otherwise have kept it. This is the diagnostic "
                "value; if it is what makes a session work, say so in a report, because the "
                "rule it overrides is what this add-on believes about your DLSS 5 add-on.");
        }
        UnwrapReShadeProxy(dev);
        return;
    }
    // ReShade loaded under the name d3d12.dll IS the D3D12 runtime this device
    // was created through, not a wrapper it acquired along the way. Stripped
    // there, the device NGX gets is one ReShade never saw, and the create
    // faulted inside D3D12Core on every attempt: Arknights: Endfield (#17),
    // where unwrap=0 was the difference between working and not, with either
    // add-on build. A D3D11 game with ReShade as d3d12.dll is unusual, and
    // the one measured needs the proxy kept.
    {
        const wchar_t *leaf = wcsrchr(g_reshade_path, L'\\');
        leaf = leaf != nullptr ? leaf + 1 : g_reshade_path;
        if (_wcsicmp(leaf, L"d3d12.dll") == 0)
        {
            static bool said = false;
            if (!said)
            {
                said = true;
                Log("[bridge] keeping ReShade's proxy on the D3D12 device: ReShade is loaded "
                    "as d3d12.dll, so it is the runtime this device came from. unwrap=1 in "
                    "the file is overridden here.");
            }
            return;
        }
    }
    if (g_consumer_needs_proxy)
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            Log("[bridge] keeping ReShade's proxy on the D3D12 device: the DLSS 5 add-on "
                "beside this one is a build measured to need the queue it announces. "
                "unwrap=1 in the file is overridden for this build only.");
        }
        return;
    }
    UnwrapReShadeProxy(dev);
}

// The fault here was reported inside D3D12Core.dll on an older release; device
// creation now succeeds and what follows is where Prey stops.
static HRESULT SafeCreateDevice(PFN_D3D12CreateDevice_ fn, IDXGIAdapter *ad,
                                ID3D12Device **out, DWORD *code)
{
    *code = 0;
    HRESULT hr = E_FAIL;
    // The highest feature level the adapter grants, 12_1 down to 11_0. The
    // device used to ask for 11_0 outright, which every NVIDIA driver serves
    // identically -- the level is a floor, not a mode -- but a D3D12 runtime
    // that is not NVIDIA's can gate capabilities on it: vkd3d-proton under
    // Proton faulted inside the neural add-on's evaluate on this device while
    // the same add-on ran for hours on a game's own 12_x device (#22). Which
    // level was granted is logged, so a report carries it.
    static const D3D_FEATURE_LEVEL kLevels[] = { D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    for (size_t i = 0; i < _countof(kLevels) && (FAILED(hr) || *out == nullptr); ++i)
    {
        got = kLevels[i];
        __try { hr = fn(ad, kLevels[i], __uuidof(ID3D12Device),
                        reinterpret_cast<void **>(out)); }
        __except (CaptureFault(GetExceptionInformation()))
        { *code = GetExceptionCode(); return E_FAIL; }
    }

    if (SUCCEEDED(hr) && *out != nullptr)
    {
        Log("[bridge] D3D12 device created at feature level %d_%d.", (got >> 12) & 0xF, (got >> 8) & 0xF);
        MaybeUnwrap(out);
        LogAddonBases();
    }
    return hr;
}

static bool EnableDredGuarded(HMODULE d3d12, DWORD *code)
{
    *code = 0;
    __try
    {
        auto get_debug = reinterpret_cast<HRESULT (WINAPI *)(REFIID, void **)>(
            GetProcAddress(d3d12, "D3D12GetDebugInterface"));
        if (get_debug == nullptr) return false;

        ID3D12DeviceRemovedExtendedDataSettings *set = nullptr;
        if (FAILED(get_debug(__uuidof(ID3D12DeviceRemovedExtendedDataSettings),
                             reinterpret_cast<void **>(&set))) || set == nullptr)
            return false;

        set->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        set->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        set->Release();
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *code = GetExceptionCode();
        return false;
    }
}

static void EnableDred(HMODULE d3d12)
{
    DWORD code = 0;
    if (EnableDredGuarded(d3d12, &code))
    {
        Log("[bridge] DRED is on: if the GPU is reset, what it was executing will be "
            "recorded here.");
        return;
    }
    if (code != 0)
        Log("[bridge] enabling DRED raised 0x%08X. It is only a diagnostic, so it is "
            "left off and the session continues without it.", code);
    else
        Log("[bridge] DRED is unavailable on this Windows and driver combination.");
}

// The breadcrumb history is a list of these, and a bare number in a report
// costs whoever reads it a trip to d3d12.h. Names taken from the enum itself.
static const char *BreadcrumbOpName(unsigned op)
{
    static const char *k[] = {
        "SetMarker", "BeginEvent", "EndEvent", "DrawInstanced",
        "DrawIndexedInstanced", "ExecuteIndirect", "Dispatch", "CopyBufferRegion",
        "CopyTextureRegion", "CopyResource", "CopyTiles", "ResolveSubresource",
        "ClearRenderTargetView", "ClearUnorderedAccessView", "ClearDepthStencilView",
        "ResourceBarrier", "ExecuteBundle", "Present", "ResolveQueryData",
        "BeginSubmission", "EndSubmission", "DecodeFrame", "ProcessFrames",
        "AtomicCopyBufferUint", "AtomicCopyBufferUint64", "ResolveSubresourceRegion",
        "WriteBufferImmediate", "DecodeFrame1", "SetProtectedResourceSession",
        "DecodeFrame2", "ProcessFrames1", "BuildRaytracingAccelerationStructure",
        "EmitRaytracingAccelerationStructurePostbuildInfo",
        "CopyRaytracingAccelerationStructure", "DispatchRays",
        "InitializeMetaCommand", "ExecuteMetaCommand", "EstimateMotion",
        "ResolveMotionVectorHeap", "SetPipelineState1",
    };
    return op < _countof(k) ? k[op] : "unknown";
}

// Reads back what DRED collected. Only meaningful after the device is gone.
static void BridgeDisable(const char *why)
{
    if (g_bridge.disabled) return;
    g_bridge.disabled = true;
    Warn("stopped: %s. The game renders normally. See dlss5-bridge.log "
         "in the game folder for the detail.", why);

    // Before anything else. The game's queue may be waiting on a fence value
    // this side will never reach, and a fence can be signalled from the CPU.
    // Releasing it first also lets the drain below complete, because the D3D12
    // queue is waiting on a signal that the blocked queue owes it.
    if (g_bridge.pending_out != 0 && g_bridge.fence12 != nullptr)
    {
        Log("[bridge] releasing the game's queue from fence %llu on the way out.",
            g_bridge.pending_out);
        g_bridge.fence12->Signal(g_bridge.pending_out);
        g_bridge.pending_out = 0;
    }
    BridgeReleaseTextures();
    if (g_bridge.adapter3 != nullptr)
    { g_bridge.adapter3->Release(); g_bridge.adapter3 = nullptr; }
}

// The reason reaches the user unchanged. It used to be replaced with "the NGX
// session could not be opened", which is false at all three call sites: every
// one is downstream of the session_ready check, so the session had already
// opened. The message pointed every bug report at the init code.
// NGX answers in hex and nothing in this log said what the hex meant, so every
// report cost a trip to nvsdk_ngx_defs.h -- the same tax the DRED breadcrumbs
// charged until they were given names. Values are NVSDK_NGX_Result_Fail | n,
// taken from that header.
static const char *NgxResultName(NVSDK_NGX_Result r)
{
    if (r == NGX_SUCCESS) return "Success";
    if ((static_cast<unsigned>(r) & 0xFFF00000u) != 0xBAD00000u) return "not an NGX result";
    static const char *k[] = {
        "Fail", "FeatureNotSupported", "PlatformError", "FeatureAlreadyExists",
        "FeatureNotFound", "InvalidParameter", "ScratchBufferTooSmall",
        "NotInitialized", "UnsupportedInputFormat", "RWFlagMissing", "MissingInput",
        "UnableToInitializeFeature", "OutOfDate", "OutOfGPUMemory",
        "UnsupportedFormat", "UnableToWriteToAppDataPath", "UnsupportedParameter",
        "Denied", "NotImplemented",
    };
    const unsigned n = static_cast<unsigned>(r) & 0xFFFFFu;
    return n < _countof(k) ? k[n] : "unknown";
}

static void BridgeFail(const char *what)
{
    // Clearing this on the first failure, not the third, is what keeps the
    // promise BridgeWillDeliver makes. That decision is taken at the top of the
    // evaluate, before this frame runs, so a frame_ready left standing through
    // two more failures suppresses the game's own DLSS on frames into which
    // nothing is then written. The counter still governs when to stop; it no
    // longer governs whether the game may draw.
    g_bridge.frame_ready = false;

    if (++g_bridge.consecutive_fails >= 3)
    {
        Log("[bridge] disabled after repeated failures (%s). Game rendering is untouched.", what);
        BridgeDisable(what);
    }
}

static bool   BeginCommands();
static UINT64 EndCommands();
static void   BridgeEnterStall(UINT64 done, UINT64 wanted, const char *who);

// Defined with the frame path below; BridgeHashOutput records one copy and
// needs both, and it sits above them so it can be called from the evaluate.
static bool BeginCommands();
static void Barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *res,
                    D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);

typedef HRESULT (WINAPI *PFN_D3DCompile_)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
                                          ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT,
                                          ID3DBlob **, ID3DBlob **);

// LoadLibraryW searches the executable's directory first, so a game shipping
// its own d3dcompiler_47.dll hands us that one -- whatever it was built from,
// years ago. The Elder Scrolls Online ships one, and renaming it away is what
// made ESO work with this add-on loaded. Microsoft's copy in System32 is the
// only one we have any reason to want: these two shaders are ours, compiled
// once per session, and nothing about them needs a game's compiler.
static PFN_D3DCompile_ LoadSystemD3DCompiler()
{
    static PFN_D3DCompile_ cached;
    static bool            tried;
    if (tried) return cached;
    tried = true;

    HMODULE m = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr,
                               LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (m == nullptr)
    {
        // Older Windows without LOAD_LIBRARY_SEARCH_SYSTEM32 support falls back
        // to the plain search, which is what this add-on did everywhere until
        // now. Say which one was taken either way.
        m = LoadLibraryW(L"d3dcompiler_47.dll");
        if (m != nullptr)
            Log("[bridge] d3dcompiler_47.dll loaded by the default search order; "
                "a copy in the game folder wins over the one in System32.");
    }
    if (m == nullptr) return nullptr;

    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(m, path, MAX_PATH);
    Log("[bridge] shader compiler: %ls", path);

    cached = reinterpret_cast<PFN_D3DCompile_>(GetProcAddress(m, "D3DCompile"));
    return cached;
}


static bool BridgeMakeDepthShader(ID3D11Device *dev11)
{
    if (g_bridge.depth_cs != nullptr) return true;

    static const char kSrc[] =
        "Texture2D<float>   src : register(t0);\n"
        "RWTexture2D<float> dst : register(u0);\n"
        "[numthreads(8,8,1)]\n"
        "void main(uint3 id : SV_DispatchThreadID)\n"
        "{\n"
        "    uint w, h; dst.GetDimensions(w, h);\n"
        "    if (id.x < w && id.y < h) dst[id.xy] = src[id.xy];\n"
        "}\n";

    PFN_D3DCompile_ compile = LoadSystemD3DCompiler();
    if (compile == nullptr) { Log("[bridge] d3dcompiler_47.dll unavailable"); return false; }

    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = compile(kSrc, sizeof(kSrc) - 1, "depthblit", nullptr, nullptr,
                         "main", "cs_5_0", 0, 0, &code, &err);
    if (FAILED(hr) || code == nullptr)
    {
        Log("[bridge] depth shader compile failed 0x%08X: %s", hr,
            err != nullptr ? static_cast<const char *>(err->GetBufferPointer()) : "");
        if (err != nullptr) err->Release();
        return false;
    }
    if (err != nullptr) err->Release();

    hr = dev11->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                    &g_bridge.depth_cs);
    code->Release();
    if (FAILED(hr)) { Log("[bridge] CreateComputeShader failed 0x%08X", hr); return false; }

    Log("[bridge] depth conversion shader ready (cs_5_0)");
    return true;
}

// Converts the game's R24G8_TYPELESS depth into the shared R32_FLOAT texture.
// This runs on the immediate context in the middle of the game's frame, so the
// compute stage bindings it touches are saved and put back afterwards.
static bool BridgeBlitDepth(ID3D11DeviceContext *ctx, ID3D11Device *dev11, ID3D11Texture2D *src)
{
    if (g_bridge.depth_cs == nullptr || g_bridge.depth_uav == nullptr) return false;

    if (g_bridge.depth_src != src)
    {
        if (g_bridge.depth_srv != nullptr) { g_bridge.depth_srv->Release(); g_bridge.depth_srv = nullptr; }
        if (g_bridge.depth_src != nullptr) { g_bridge.depth_src->Release(); g_bridge.depth_src = nullptr; }

        // A depth buffer is typeless, and which view format is legal depends on
        // which typeless family it belongs to. Hardcoding one only ever works
        // for the game it was written against.
        D3D11_TEXTURE2D_DESC sdesc = {};
        src->GetDesc(&sdesc);

        DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
        switch (sdesc.Format)
        {
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:      view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:              view = DXGI_FORMAT_R32_FLOAT; break;
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:   view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_D16_UNORM:              view = DXGI_FORMAT_R16_UNORM; break;
        default:                                 view = sdesc.Format; break;  // already typed
        }

        // The numbers go alongside the names: a format this build has no name
        // for is exactly the one a report needs to identify.
        Log("[bridge] game depth is %s (%u), reading it as %s (%u)",
            FormatName(sdesc.Format), sdesc.Format, FormatName(view), view);

        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format              = view;
        sd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        HRESULT hr = dev11->CreateShaderResourceView(src, &sd, &g_bridge.depth_srv);
        if (FAILED(hr))
        {
            // Repeating this every frame produces hundreds of megabytes of log
            // and hides everything else, so say it once and stop trying.
            Log("[bridge] cannot read this game's depth buffer (format %u); depth will not "
                "reach DLSS. Please report this format.", sdesc.Format);
            Log("  Depth will not reach DLSS. Image quality will suffer, especially");
            Log("  around moving objects. Please report this format.");
            g_bridge.depth_converted = false;
            return false;
        }
        Log("[bridge] depth view: resource format %u read as %u", sdesc.Format, view);

        g_bridge.depth_src = src;
        src->AddRef();  // keep the pointer from being recycled under the cache
    }

    ID3D11ComputeShader       *old_cs  = nullptr;
    ID3D11ShaderResourceView  *old_srv = nullptr;
    ID3D11UnorderedAccessView *old_uav = nullptr;
    ctx->CSGetShader(&old_cs, nullptr, nullptr);
    ctx->CSGetShaderResources(0, 1, &old_srv);
    ctx->CSGetUnorderedAccessViews(0, 1, &old_uav);

    UINT keep = static_cast<UINT>(-1);
    ctx->CSSetShader(g_bridge.depth_cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &g_bridge.depth_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &g_bridge.depth_uav, &keep);
    ctx->Dispatch((g_bridge.slot_w[SLOT_DEPTH] + 7) / 8,
                  (g_bridge.slot_h[SLOT_DEPTH] + 7) / 8, 1);

    // Unbind before restoring, so the shared texture is not left bound as a UAV
    // while the game's own views go back on the same slots.
    ID3D11ShaderResourceView  *no_srv = nullptr;
    ID3D11UnorderedAccessView *no_uav = nullptr;
    ctx->CSSetShaderResources(0, 1, &no_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &no_uav, &keep);

    ctx->CSSetShader(old_cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &old_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &old_uav, &keep);
    if (old_cs  != nullptr) old_cs->Release();
    if (old_srv != nullptr) old_srv->Release();
    if (old_uav != nullptr) old_uav->Release();
    return true;
}

// Converts the game's motion vectors into the shared R16G16_FLOAT texture, used
// when the driver refuses to share the game's own MV format. Same shape as the
// depth conversion: save the compute bindings, dispatch, put everything back.
static bool BridgeMakeMVShader(ID3D11Device *dev11)
{
    if (g_bridge.mv_cs != nullptr) return true;

    static const char kSrc[] =
        "Texture2D<float2>   src : register(t0);\n"
        "RWTexture2D<float2> dst : register(u0);\n"
        "[numthreads(8,8,1)]\n"
        "void main(uint3 id : SV_DispatchThreadID)\n"
        "{\n"
        "    uint w, h; dst.GetDimensions(w, h);\n"
        "    if (id.x < w && id.y < h) dst[id.xy] = src[id.xy];\n"
        "}\n";

    PFN_D3DCompile_ compile = LoadSystemD3DCompiler();
    if (compile == nullptr) { Log("[bridge] d3dcompiler_47.dll unavailable"); return false; }

    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = compile(kSrc, sizeof(kSrc) - 1, "mvblit", nullptr, nullptr,
                         "main", "cs_5_0", 0, 0, &code, &err);
    if (FAILED(hr) || code == nullptr)
    {
        Log("[bridge] MV shader compile failed 0x%08X: %s", hr,
            err != nullptr ? static_cast<const char *>(err->GetBufferPointer()) : "");
        if (err != nullptr) err->Release();
        return false;
    }
    if (err != nullptr) err->Release();

    hr = dev11->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                    &g_bridge.mv_cs);
    code->Release();
    if (FAILED(hr)) { Log("[bridge] MV CreateComputeShader failed 0x%08X", hr); return false; }

    Log("[bridge] MV conversion shader ready (cs_5_0)");
    return true;
}

static bool BridgeBlitMV(ID3D11DeviceContext *ctx, ID3D11Device *dev11, ID3D11Texture2D *src)
{
    if (g_bridge.mv_cs == nullptr || g_bridge.mv_uav == nullptr) return false;

    if (g_bridge.mv_src != src)
    {
        if (g_bridge.mv_srv != nullptr) { g_bridge.mv_srv->Release(); g_bridge.mv_srv = nullptr; }
        if (g_bridge.mv_src != nullptr) { g_bridge.mv_src->Release(); g_bridge.mv_src = nullptr; }

        // A null descriptor takes the resource's own format, which is invalid
        // for a typeless resource, and this branch is reachable with one: a game
        // supplying R32G32_TYPELESS motion vectors resolves through TypedFormat
        // to R32G32_FLOAT, fails to share, and enters the fallback. Name the
        // view format the same way BridgeBlitDepth names its depth view.
        D3D11_TEXTURE2D_DESC sdesc = {};
        src->GetDesc(&sdesc);

        DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
        switch (sdesc.Format)
        {
        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R32G32_FLOAT:           view = DXGI_FORMAT_R32G32_FLOAT; break;
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R16G16_FLOAT:           view = DXGI_FORMAT_R16G16_FLOAT; break;
        case DXGI_FORMAT_R16G16_UNORM:           view = DXGI_FORMAT_R16G16_UNORM; break;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:      view = DXGI_FORMAT_R10G10B10A2_UNORM; break;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:     view = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
        default:                                 view = sdesc.Format; break;  // already typed
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format              = view;
        sd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        HRESULT hr = dev11->CreateShaderResourceView(src, &sd, &g_bridge.mv_srv);
        if (FAILED(hr))
        {
            // Like the depth case, say it once. Continuing would mean DLSS reads
            // a motion-vector texture nobody ever writes, and the image smears.
            //
            // Unlike depth, this stops the bridge rather than degrading to a
            // placeholder: depth's fallback quietly drops one input, but an MV
            // texture nobody writes smears every frame, and zero is not a
            // neutral motion value. Stopping returns rendering to the game's
            // own working DLSS instead. Deliberate divergence from BridgeBlitDepth.
            Log("[bridge] cannot read this game's motion vectors (format %u); the "
                "bridge is stopping rather than feed it stale ones. Please report "
                "this format.", sdesc.Format);
            BridgeDisable("the game's MV format cannot be read");
            return false;
        }
        Log("[bridge] MV view: resource format %u read as %u", sdesc.Format, view);

        g_bridge.mv_src = src;
        src->AddRef();
    }

    ID3D11ComputeShader       *old_cs  = nullptr;
    ID3D11ShaderResourceView  *old_srv = nullptr;
    ID3D11UnorderedAccessView *old_uav = nullptr;
    ctx->CSGetShader(&old_cs, nullptr, nullptr);
    ctx->CSGetShaderResources(0, 1, &old_srv);
    ctx->CSGetUnorderedAccessViews(0, 1, &old_uav);

    UINT keep = static_cast<UINT>(-1);
    ctx->CSSetShader(g_bridge.mv_cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &g_bridge.mv_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &g_bridge.mv_uav, &keep);
    ctx->Dispatch((g_bridge.slot_w[SLOT_MV] + 7) / 8,
                  (g_bridge.slot_h[SLOT_MV] + 7) / 8, 1);

    ID3D11ShaderResourceView  *no_srv = nullptr;
    ID3D11UnorderedAccessView *no_uav = nullptr;
    ctx->CSSetShaderResources(0, 1, &no_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &no_uav, &keep);

    ctx->CSSetShader(old_cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &old_srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &old_uav, &keep);
    if (old_cs  != nullptr) old_cs->Release();
    if (old_srv != nullptr) old_srv->Release();
    if (old_uav != nullptr) old_uav->Release();
    return true;
}

// D3D12 leaves resource lifetime to the caller: releasing a texture or an NGX
// feature while a submitted command list still references it corrupts driver
// state and surfaces later as an access violation inside NGX. Every teardown
// therefore drains the queue first. Rebuilds are rare, so the stall is free.
static void BridgeDrainGpu()
{
    if (g_bridge.queue == nullptr || g_bridge.fence12 == nullptr) return;

    const UINT64 v = ++g_bridge.fence_value;
    g_bridge.queue->Signal(g_bridge.fence12, v);
    if (g_bridge.fence12->GetCompletedValue() < v && g_bridge.fence_event != nullptr)
    {
        g_bridge.fence12->SetEventOnCompletion(v, g_bridge.fence_event);
        // The wake is not the answer; the fence is. SetEventOnCompletion cannot
        // be cancelled, so a wait that timed out earlier leaves a live
        // registration, and one auto-reset event serves all three waits here --
        // a stale signal would otherwise satisfy this one for the wrong value
        // and let the release below run while command lists still reference the
        // resources, which is exactly what this drain exists to prevent.
        if (WaitForSingleObject(g_bridge.fence_event, 5000) != WAIT_OBJECT_0 ||
            g_bridge.fence12->GetCompletedValue() < v)
            Log("[bridge] timed out draining the queue before teardown");
    }
    // Every allocator is now retired, so none of them needs waiting on again.
    for (int i = 0; i < Bridge::kFrames; ++i) g_bridge.alloc_fence[i] = 0;
}

// The D3D12 output texture, read back once and reduced to one number.
//
// FNV-1a over every byte of every row, with the row pitch honoured: a readback
// buffer is padded to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT and hashing the padding
// would make the number depend on the alignment rather than on the pixels.
//
// It is a hash and not a comparison because the thing it has to detect is
// "identical", and two runs of a deterministic host produce byte-identical
// output when nothing in between did anything. A neighbour add-on that ran
// changes it; one that attached and wrote nothing does not.
static void BridgeReadbackStats(int slot, UINT64 fence_value)
{
    if (g_bridge.dev12 == nullptr || g_bridge.tex12[slot] == nullptr) return;

    const D3D12_RESOURCE_DESC rd = g_bridge.tex12[slot]->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT   rows = 0;
    UINT64 row_bytes = 0, total = 0;
    g_bridge.dev12->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_bytes, &total);
    if (total == 0 || total > (64ull << 20)) return;

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource *rb = nullptr;
    if (FAILED(g_bridge.dev12->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            __uuidof(ID3D12Resource), reinterpret_cast<void **>(&rb))) || rb == nullptr)
        return;

    if (!BeginCommands()) { rb->Release(); return; }
    Barrier(g_bridge.list, g_bridge.tex12[slot],
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    src.pResource = g_bridge.tex12[slot];
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    g_bridge.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(g_bridge.list, g_bridge.tex12[slot],
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    const UINT64 v = EndCommands();
    (void)fence_value;

    if (g_bridge.fence12 != nullptr && g_bridge.fence_event != nullptr &&
        g_bridge.fence12->GetCompletedValue() < v)
    {
        g_bridge.fence12->SetEventOnCompletion(v, g_bridge.fence_event);
        if (WaitForSingleObject(g_bridge.fence_event, 4000) != WAIT_OBJECT_0 ||
            g_bridge.fence12->GetCompletedValue() < v)
        { rb->Release(); Log("[bridge] output hash: the copy did not complete"); return; }
    }

    void *mapped = nullptr;
    D3D12_RANGE all = { 0, static_cast<SIZE_T>(total) };
    if (FAILED(rb->Map(0, &all, &mapped)) || mapped == nullptr)
    { rb->Release(); return; }

    unsigned long long hash = 1469598103934665603ull;
    const BYTE *base = static_cast<const BYTE *>(mapped);
    for (UINT r = 0; r < rows; ++r)
    {
        const BYTE *row = base + static_cast<size_t>(r) * fp.Footprint.RowPitch;
        for (UINT64 i = 0; i < row_bytes; ++i)
        { hash ^= row[i]; hash *= 1099511628211ull; }
    }

    // The mean of each channel, decoded per format, and the luma of the means.
    // The hash answers "did anything change"; this answers "by how much, in
    // brightness" -- a neural pass that overexposes an HDR frame moves this by
    // a factor while the hash only says it moved (#10). Decoded for the four
    // formats a swapchain is likely to be; anything else prints no mean.
    {
        double sum[3] = { 0, 0, 0 };
        UINT64 n = 0;
        const DXGI_FORMAT f = rd.Format;
        auto half = [](unsigned short v) -> float {
            const unsigned s = (v >> 15) & 1, e = (v >> 10) & 0x1F, m = v & 0x3FF;
            float out;
            if (e == 0)       out = static_cast<float>(m) / 1024.0f / 16384.0f;
            else if (e == 31) out = m ? 0.0f : 65504.0f;
            else              out = (1.0f + static_cast<float>(m) / 1024.0f) *
                                    static_cast<float>(pow(2.0, static_cast<int>(e) - 15));
            return s ? -out : out;
        };
        const bool fp16   = f == DXGI_FORMAT_R16G16B16A16_FLOAT;
        const bool rgb10  = f == DXGI_FORMAT_R10G10B10A2_UNORM;
        const bool rgba8  = f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                            f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        const bool bgr    = f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        const bool rg11   = f == DXGI_FORMAT_R11G11B10_FLOAT;
        if (fp16 || rgb10 || rgba8 || rg11)
        {
            for (UINT r = 0; r < rows; ++r)
            {
                const BYTE *row = base + static_cast<size_t>(r) * fp.Footprint.RowPitch;
                for (UINT x = 0; x < rd.Width; ++x)
                {
                    float c[3];
                    if (fp16)
                    {
                        const unsigned short *px = reinterpret_cast<const unsigned short *>(row) + x * 4;
                        c[0] = half(px[0]); c[1] = half(px[1]); c[2] = half(px[2]);
                    }
                    else if (rgb10)
                    {
                        const unsigned px32 = reinterpret_cast<const unsigned *>(row)[x];
                        c[0] = (px32 & 0x3FF) / 1023.0f; c[1] = ((px32 >> 10) & 0x3FF) / 1023.0f; c[2] = ((px32 >> 20) & 0x3FF) / 1023.0f;
                    }
                    else if (rg11)
                    {
                        // 11-bit and 10-bit floats: 5-bit exponent, bias 15, no sign.
                        const unsigned px32 = reinterpret_cast<const unsigned *>(row)[x];
                        auto f11 = [](unsigned b, unsigned mbits) -> float {
                            const unsigned e = (b >> mbits) & 0x1F, m = b & ((1u << mbits) - 1);
                            if (e == 0) return static_cast<float>(m) / (1u << mbits) / 16384.0f;
                            if (e == 31) return 65504.0f;
                            return (1.0f + static_cast<float>(m) / (1u << mbits)) * static_cast<float>(pow(2.0, static_cast<int>(e) - 15));
                        };
                        c[0] = f11(px32 & 0x7FF, 6); c[1] = f11((px32 >> 11) & 0x7FF, 6); c[2] = f11((px32 >> 22) & 0x3FF, 5);
                    }
                    else
                    {
                        const BYTE *px = row + x * 4;
                        c[0] = px[bgr ? 2 : 0] / 255.0f; c[1] = px[1] / 255.0f; c[2] = px[bgr ? 0 : 2] / 255.0f;
                    }
                    sum[0] += c[0]; sum[1] += c[1]; sum[2] += c[2]; ++n;
                }
            }
            if (n != 0)
            {
                const double mr = sum[0] / n, mg = sum[1] / n, mb = sum[2] / n;
                // The create flags this feature was built with, from the block
                // that built it, so two builds on the same format -- float with
                // and without IsHDR -- read as two different presentations.
                int flags = -1;
                if (g_bridge.params != nullptr) g_bridge.params->Get("DLSS.Feature.Create.Flags", &flags);
                const double luma = 0.2126 * mr + 0.7152 * mg + 0.0722 * mb;
                Log("[bridge] %s mean after evaluate %llu: R=%.5f G=%.5f B=%.5f luma=%.5f (%s flags %d)",
                    slot == SLOT_OUTPUT ? "output" : "input", g_bridge.frames_done, mr, mg, mb, luma,
                    FormatName(f), flags);
                if (slot == SLOT_OUTPUT) g_bright_out = static_cast<float>(luma);
                else                     g_bright_in  = static_cast<float>(luma);
            }
        }
    }
    D3D12_RANGE none = { 0, 0 };
    rb->Unmap(0, &none);
    rb->Release();

    if (slot == SLOT_OUTPUT)
        Log("[bridge] output hash after evaluate %llu: %016llX  (%ux%u %s, %llu rows of %llu "
            "bytes). Two runs that differ only in which neighbour add-ons were present "
            "must not produce the same number.",
            g_bridge.frames_done, hash, static_cast<unsigned>(rd.Width),
            static_cast<unsigned>(rd.Height), FormatName(rd.Format),
            static_cast<unsigned long long>(rows), row_bytes);
}

// Input and output, once per feature build, and the one number a brightness
// report needs: how much brighter or darker the output came back. DLSS keeps
// luminance to within a few percent; a neural pass that does not shows here
// without a second run with it switched off.
static void BridgeReadback(UINT64 fence_value)
{
    g_bright_in = g_bright_out = 0.0f;
    BridgeReadbackStats(SLOT_COLOR,  fence_value);
    BridgeReadbackStats(SLOT_OUTPUT, fence_value);
    if (g_bright_in > 0.0f && g_bright_out > 0.0f)
        Log("[bridge] brightness out/in after evaluate %llu: %.2f", g_bridge.frames_done,
            g_bright_out / g_bright_in);
}

// An HDR10 presentation with a UNORM back buffer: the surface carries PQ code
// values, not light. Handing those to the evaluate as SDR colour is what made
// Baldur's Gate 3's synthetic contract come out dimmer and less saturated than
// the game's own DLSS, whose input is linear R11G11B10_FLOAT with IsHDR set
// (measured 2026-09-03, D3D11 and Vulkan alike). A float back buffer on the
// same presentation is scRGB and already linear, so it is left alone.
static bool PqBackBuffer(DXGI_FORMAT f)
{
    if (InterlockedCompareExchange(&g_present_space, 0, 0) != 3 /* HDR10 (PQ) */) return false;
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return false;
    default:
        return true;
    }
}

// The two textures only. The heap, the root signature and the pipeline live as
// long as the device does, and that is not tidiness: the DLSS 5 add-on records
// the descriptor heaps it sees set on the command list and sets them again
// when it intercepts a create, so a heap released at a rebuild came back as a
// dangling pointer inside ReShade's SetDescriptorHeaps (measured 2026-09-03).
// The descriptors are rewritten in place instead.
//
// The budget is the caller's, for the same reason Synth12EvaluateShared's is:
// this wait can run on a thread the game's Vulkan queue is parked behind, and a
// hard-coded four seconds there is twice the Windows TDR window. Four seconds is
// still the default, which is what every D3D11 and D3D12 caller wants -- nothing
// is parked behind those.
static bool PqPassWait(PqPass *p, UINT64 value, DWORD wait_ms = 4000)
{
    if (p->fence == nullptr) return true;
    const UINT64 completed = p->fence->GetCompletedValue();
    if (completed == UINT64_MAX) return false;
    if (completed >= value) return true;
    if (FAILED(p->fence->SetEventOnCompletion(value, p->ev))) return false;
    if (WaitForSingleObject(p->ev, wait_ms) != WAIT_OBJECT_0) return false;
    const UINT64 after = p->fence->GetCompletedValue();
    return after != UINT64_MAX && after >= value;
}

static bool PqPassRelease(PqPass *p)
{
    if (!PqPassWait(p, p->fv)) {
        Log("[bridge] colour-pass completion unproven; retaining its resources.");
        return false;
    }
    if (p->in  != nullptr) { p->in->Release();  p->in  = nullptr; }
    if (p->out != nullptr) { p->out->Release(); p->out = nullptr; }
    p->colour = p->output = p->output_b = nullptr;
    return true;
}

// One shader, one root constant choosing the direction: 0 decodes colour into
// in, 1 encodes out into output. The BT.2020 matrices are the ones an HDR10
// surface implies; the model side is BT.709.
static bool PqPassMake(PqPass *p, ID3D12Device *dev, ID3D12Resource *colour, ID3D12Resource *output,
                       ID3D12Resource *output_b = nullptr, bool srgb = false)
{
    if (!PqPassRelease(p)) return false;
    p->dev      = dev;
    p->srgb     = srgb;
    p->colour   = colour;
    p->output   = output;
    p->output_b = output_b;
    const D3D12_RESOURCE_DESC cd = colour->GetDesc(), od = output->GetDesc();
    p->cw = static_cast<UINT>(cd.Width); p->ch = cd.Height;
    p->ow = static_cast<UINT>(od.Width); p->oh = od.Height;
    const DXGI_FORMAT linear_format = srgb ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    // Wide-gamut cancellation can destroy dark HDR channels in FP16 intermediates.
    rd.Format           = linear_format;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource **pair[2] = { &p->in, &p->out };
    for (int i = 0; i < 2; ++i)
    {
        rd.Width  = i == 0 ? p->cw : p->ow;
        rd.Height = i == 0 ? p->ch : p->oh;
        const HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource),
            reinterpret_cast<void **>(pair[i]));
        if (FAILED(hr))
        {
            Log("[bridge] the %llux%u %s texture for colour %s could not be created: 0x%08X",
                rd.Width, rd.Height, FormatName(linear_format), i == 0 ? "decode" : "encode", hr);
            PqPassRelease(p);
            return false;
        }
    }

    if (p->heap == nullptr)
    {
        for (int i = 0; i < 2; ++i)
        {
            if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                    __uuidof(ID3D12CommandAllocator), reinterpret_cast<void **>(&p->alloc[i]))) ||
                FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, p->alloc[i], nullptr,
                    __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&p->list[i]))))
            { Log("[bridge] the HDR10 pass could not get a command list"); PqPassRelease(p); return false; }
            p->list[i]->Close();
        }
        if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                    reinterpret_cast<void **>(&p->fence))))
        { Log("[bridge] the HDR10 pass could not get a fence"); PqPassRelease(p); return false; }
        p->ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        p->fv = p->done[0] = p->done[1] = 0;

        static const char kSrc[] =
            "Texture2D<float4>   src : register(t0);\n"
            "RWTexture2D<float4> dst : register(u0);\n"
            "cbuffer C : register(b0) { uint mode; };\n"
            "static const float m1 = 0.1593017578125, m2 = 78.84375;\n"
            "static const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;\n"
            "static const float3x3 k2020to709 = { 1.6604910021, -0.5876411388, -0.0728498633,\n"
            "                                    -0.1245504745, 1.1328998971, -0.0083494226,\n"
            "                                    -0.0181507634, -0.1005788980, 1.1187296614 };\n"
            "static const float3x3 k709to2020 = { 0.6274038959, 0.3292830384, 0.0433130657,\n"
            "                                     0.0690972894, 0.9195403951, 0.0113623156,\n"
            "                                     0.0163914389, 0.0880133079, 0.8955952532 };\n"
            "float3 pq_to_nits(float3 e)\n"
            "{\n"
            "    float3 p = pow(max(e, 0.0), 1.0 / m2);\n"
            "    return pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1) * 10000.0;\n"
            "}\n"
            "float3 nits_to_pq(float3 n)\n"
            "{\n"
            "    float3 y = pow(saturate(n / 10000.0), m1);\n"
            "    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);\n"
            "}\n"
            "float3 srgb_decode(float3 v) { return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }\n"
            "float3 srgb_encode(float3 v) { v = saturate(v); return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055; }\n"
            "[numthreads(8,8,1)]\n"
            "void main(uint3 id : SV_DispatchThreadID)\n"
            "{\n"
            "    uint w, h; dst.GetDimensions(w, h);\n"
            "    if (id.x >= w || id.y >= h) return;\n"
            "    float4 v = src[id.xy];\n"
            "    if (mode == 2)\n"
            "        dst[id.xy] = float4(srgb_decode(v.rgb), v.a);\n"
            "    else if (mode == 3)\n"
            "        dst[id.xy] = float4(srgb_encode(v.rgb), v.a);\n"
            "    else if (mode == 0)\n"
            "        dst[id.xy] = float4(mul(k2020to709, pq_to_nits(v.rgb)) / 80.0, v.a);\n"
            "    else\n"
            "        dst[id.xy] = float4(nits_to_pq(mul(k709to2020, v.rgb * 80.0)), v.a);\n"
            "}\n";

        PFN_D3DCompile_ compile = LoadSystemD3DCompiler();
        if (compile == nullptr) { Log("[bridge] d3dcompiler_47.dll unavailable for the HDR10 pass"); PqPassRelease(p); return false; }
        ID3DBlob *code = nullptr, *err = nullptr;
        HRESULT hr = compile(kSrc, sizeof(kSrc) - 1, "pq", nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &err);
        if (FAILED(hr) || code == nullptr)
        {
            Log("[bridge] the HDR10 shader would not compile: %s",
                err != nullptr ? static_cast<const char *>(err->GetBufferPointer()) : "");
            if (err != nullptr) err->Release();
            PqPassRelease(p);
            return false;
        }
        if (err != nullptr) err->Release();

        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors                    = 1;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors                    = 1;
        ranges[1].OffsetInDescriptorsFromTableStart = 1;
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 2;
        params[0].DescriptorTable.pDescriptorRanges   = ranges;
        params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.Num32BitValues            = 1;
        params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 2;
        rsd.pParameters   = params;

        HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
        auto serialize = d3d12 != nullptr
            ? reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(d3d12, "D3D12SerializeRootSignature"))
            : nullptr;
        ID3DBlob *rsblob = nullptr, *rserr = nullptr;
        hr = serialize != nullptr ? serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsblob, &rserr) : E_FAIL;
        if (FAILED(hr) || rsblob == nullptr)
        {
            Log("[bridge] the HDR10 root signature would not serialize 0x%08X: %s", hr,
                rserr != nullptr ? static_cast<const char *>(rserr->GetBufferPointer()) : "");
            if (rserr != nullptr) rserr->Release();
            code->Release();
            PqPassRelease(p);
            return false;
        }
        if (rserr != nullptr) rserr->Release();
        hr = dev->CreateRootSignature(0, rsblob->GetBufferPointer(), rsblob->GetBufferSize(),
                                      __uuidof(ID3D12RootSignature), reinterpret_cast<void **>(&p->rs));
        rsblob->Release();
        if (FAILED(hr)) { Log("[bridge] the HDR10 root signature failed 0x%08X", hr); code->Release(); PqPassRelease(p); return false; }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature     = p->rs;
        pd.CS.pShaderBytecode = code->GetBufferPointer();
        pd.CS.BytecodeLength  = code->GetBufferSize();
        hr = dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void **>(&p->pso));
        code->Release();
        if (FAILED(hr)) { Log("[bridge] the HDR10 pipeline failed 0x%08X", hr); PqPassRelease(p); return false; }

        // Six descriptors, three tables: [colour SRV, in UAV] for the decode,
        // [out SRV, output UAV] for the encode, and [out SRV, output_b UAV] for
        // the encode into the substitute's second Output. Six UNCONDITIONALLY,
        // and that is the point rather than slack: this block runs only inside
        // "if (p->heap == nullptr)" and the note above PqPassRelease says the heap
        // is deliberately never rebuilt, so a size that depended on whether
        // output_b was given would be four on a session that opened at vk_sync=0
        // and would be written past the first time that session rebuilt into
        // vk_sync=2. The last two go unwritten and unread when there is no second
        // Output, which costs 64 bytes of a heap the device holds for its life.
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 6;
        hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void **>(&p->heap));
        if (FAILED(hr)) { Log("[bridge] the HDR10 descriptor heap failed 0x%08X", hr); PqPassRelease(p); return false; }
    }

    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = p->heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels     = 1;
    sd.Format = cd.Format;
    dev->CreateShaderResourceView(colour, &sd, cpu);
    cpu.ptr += inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    ud.Format        = linear_format;
    dev->CreateUnorderedAccessView(p->in, nullptr, &ud, cpu);
    cpu.ptr += inc;
    sd.Format = linear_format;
    dev->CreateShaderResourceView(p->out, &sd, cpu);
    cpu.ptr += inc;
    ud.Format = od.Format;
    dev->CreateUnorderedAccessView(output, nullptr, &ud, cpu);
    // The second encode table, written only when the caller gave a second Output.
    // Its SRV is the same p->out the first one names -- the linear result NGX
    // wrote is one texture whichever Output it is encoded into -- so only the UAV
    // differs, which is why this is six descriptors and not eight.
    if (output_b != nullptr)
    {
        const D3D12_RESOURCE_DESC bd = output_b->GetDesc();
        cpu.ptr += inc;
        sd.Format = linear_format;
        dev->CreateShaderResourceView(p->out, &sd, cpu);
        cpu.ptr += inc;
        ud.Format = bd.Format;
        dev->CreateUnorderedAccessView(output_b, nullptr, &ud, cpu);
    }

    Log("[bridge] %s conversion ready: %s -> linear BT.709 %s -> original encoding.",
        srgb ? "sRGB" : "HDR10 PQ", FormatName(cd.Format),
        srgb ? "(normalized SDR)" : "(1.0 = 80 nits)");
    return true;
}

// The colour is expected in NON_PIXEL_SHADER_RESOURCE and the output in
// UNORDERED_ACCESS, which is where both sessions hold them around the evaluate.
static void PqPassDispatch(PqPass *p, ID3D12GraphicsCommandList *list, int mode, int set)
{
    const UINT inc = p->dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ID3D12DescriptorHeap *heaps[] = { p->heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(p->rs);
    list->SetPipelineState(p->pso);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = p->heap->GetGPUDescriptorHandleForHeapStart();
    // Table 0 decodes; table 1 encodes into output and table 2 into output_b. The
    // set is read on the encode alone, because the decode reads colour, and the
    // pipeline does not double colour.
    gpu.ptr += static_cast<UINT64>(mode == 0 ? 0 : 2 + 2 * set) * inc;
    list->SetComputeRootDescriptorTable(0, gpu);
    list->SetComputeRoot32BitConstant(1, static_cast<UINT>(mode + (p->srgb ? 2 : 0)), 0);
    const UINT w = mode == 0 ? p->cw : p->ow, h = mode == 0 ? p->ch : p->oh;
    list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
}

// One of the two lists, recorded, executed on the session's queue and fenced.
// Every texture goes in and out in COMMON except the two NGX is about to use:
// in is left in NON_PIXEL_SHADER_RESOURCE and out in UNORDERED_ACCESS by the
// decode, and the encode takes them back to COMMON. The session's own barriers
// on its colour and output are unaffected, since both are returned to COMMON.
static bool PqPassRun(PqPass *p, ID3D12CommandQueue *queue, int mode, int set,
                      DWORD wait_ms = 4000)
{
    ID3D12GraphicsCommandList *list = p->list[mode];
    // Which Output this frame's evaluate wrote into, resolved ONCE and used by
    // both the barriers and the dispatch. Set 1 arrives only from the
    // substitute's pipelined transport and only after Synth12Build made the
    // texture; every other caller reads the single one it always did. Passing the
    // caller's raw set to PqPassDispatch left the two disagreeing about the same
    // condition -- the barriers naming p->output while the shader wrote through a
    // table whose UAV is a view over a texture that no longer exists.
    const int s = (set == 1 && p->output_b != nullptr) ? 1 : 0;
    ID3D12Resource *const outp = s == 1 ? p->output_b : p->output;
    if (!PqPassWait(p, p->done[mode], wait_ms)) return false;
    if (FAILED(p->alloc[mode]->Reset()) || FAILED(list->Reset(p->alloc[mode], nullptr))) return false;
    if (mode == 0)
    {
        Barrier(list, p->colour, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(list, p->in, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        PqPassDispatch(p, list, 0, 0);
        Barrier(list, p->in, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(list, p->colour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        Barrier(list, p->out, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    else
    {
        Barrier(list, p->out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(list, outp, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        PqPassDispatch(p, list, 1, s);
        Barrier(list, outp, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        Barrier(list, p->out, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        Barrier(list, p->in, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
    if (FAILED(list->Close())) return false;
    ID3D12CommandList *lists[] = { list };
    queue->ExecuteCommandLists(1, lists);
    const UINT64 value = ++p->fv;
    if (FAILED(queue->Signal(p->fence, value))) return false;
    p->done[mode] = value;
    return true;
}

// Before the NGX list is recorded: the decode, executed now, runs ahead of it.
static bool PqPassBefore(PqPass *p, ID3D12CommandQueue *queue, DWORD wait_ms = 4000)
{
    return PqPassRun(p, queue, 0, 0, wait_ms);
}

// After the NGX list was executed: the encode runs behind it, and the session's
// fence is signalled once more so whoever waits on the value returned here
// waits for the encoded result and not only for the evaluate.
static UINT64 PqPassAfter(PqPass *p, ID3D12CommandQueue *queue, ID3D12Fence *session_fence,
                          UINT64 *session_value, UINT64 fallback, int set = 0,
                          DWORD wait_ms = 4000)
{
    if (!PqPassRun(p, queue, 1, set, wait_ms)) return fallback;
    const UINT64 v = ++*session_value;
    if (FAILED(queue->Signal(session_fence, v))) return fallback;
    return v;
}

static void BridgeReleaseTextures()
{
    PqPassRelease(&g_bridge.pq);
    BridgeDrainGpu();

    // The exposure mirror goes with the rest. Leaving it out kept a shared NT
    // handle and a D3D12 resource alive for the life of the process on a
    // session that had already told the user it was disabled.
    BridgeReleaseExposureFwd();

    // The four resource keys, cleared before the textures they name are freed.
    // Same rule as the exposure one above and the same reason: NGX does not
    // AddRef what a parameter block names, this block is allocated once per
    // session and never Reset, so a rebuild left it naming four freed
    // ID3D12Resources -- and the next call handed it is CreateFeature.
    //
    // renodx-dlss5 detours NVSDK_NGX_D3D12_CreateFeature and faulted 0xC0000005
    // at +0xD006 reading them, then threw out through ReShade and fastfailed the
    // process. Measured on the D3D11 host, and the identical defect on the
    // Vulkan path took two scenarios from 3 passes in 8 to 12 in 12.
    if (g_bridge.params != nullptr)
    {
        g_bridge.params->Set("Color",         static_cast<ID3D12Resource *>(nullptr));
        g_bridge.params->Set("Output",        static_cast<ID3D12Resource *>(nullptr));
        g_bridge.params->Set("Depth",         static_cast<ID3D12Resource *>(nullptr));
        g_bridge.params->Set("MotionVectors", static_cast<ID3D12Resource *>(nullptr));
    }

    if (g_bridge.depth_uav != nullptr) { g_bridge.depth_uav->Release(); g_bridge.depth_uav = nullptr; }
    if (g_bridge.depth_srv != nullptr) { g_bridge.depth_srv->Release(); g_bridge.depth_srv = nullptr; }
    if (g_bridge.depth_src != nullptr) { g_bridge.depth_src->Release(); g_bridge.depth_src = nullptr; }
    if (g_bridge.mv_uav    != nullptr) { g_bridge.mv_uav->Release();    g_bridge.mv_uav    = nullptr; }
    if (g_bridge.mv_srv    != nullptr) { g_bridge.mv_srv->Release();    g_bridge.mv_srv    = nullptr; }
    if (g_bridge.mv_src    != nullptr) { g_bridge.mv_src->Release();    g_bridge.mv_src    = nullptr; }
    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        if (g_bridge.tex11[i]  != nullptr) { g_bridge.tex11[i]->Release();  g_bridge.tex11[i]  = nullptr; }
        if (g_bridge.tex12[i]  != nullptr) { g_bridge.tex12[i]->Release();  g_bridge.tex12[i]  = nullptr; }
        if (g_bridge.shared[i] != nullptr) { CloseHandle(g_bridge.shared[i]); g_bridge.shared[i] = nullptr; }
    }
    bool current_owned = false;
    for (auto &history : g_bridge.histories)
    {
        if (history.feature != nullptr)
        {
            if (history.feature == g_bridge.feature) current_owned = true;
            if (g_bridge.release_feature != nullptr) g_bridge.release_feature(history.feature);
        }
        history = {};
    }
    if (!current_owned && g_bridge.feature != nullptr && g_bridge.release_feature != nullptr)
    {
        g_bridge.release_feature(g_bridge.feature);
    }
    g_bridge.feature = nullptr;
    g_bridge.frame_ready = false;
}

// The one place a D3D11 description is reduced to the four facts the frame path
// reads. A second backend fills the same TexFacts from its own desc type here,
// rather than teaching the dimension repairs a second struct to read.
static void TexFactsFromD3D11(const D3D11_TEXTURE2D_DESC &d, TexFacts *out)
{
    out->Width   = d.Width;
    out->Height  = d.Height;
    out->Format  = d.Format;
    out->Samples = d.SampleDesc.Count;
}

// Reads one texture slot out of the game's parameter block and returns its
// description, so the bridge can tell when BG3 has recreated its render targets.
static ID3D11Texture2D *GameTexture(const NVSDK_NGX_Parameter *p, const char *key,
                                    TexFacts *facts)
{
    ID3D11Resource *res = nullptr;
    if (p->Get(key, &res) != NGX_SUCCESS || res == nullptr) return nullptr;
    ID3D11Texture2D *tex = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex))) ||
        tex == nullptr)
        return nullptr;
    if (facts != nullptr)
    {
        D3D11_TEXTURE2D_DESC desc = {};
        tex->GetDesc(&desc);
        TexFactsFromD3D11(desc, facts);
    }
    return tex;  // caller releases
}

// D3D12_ERROR_INVALID_REDIST, 0x887E0003: D3D12 refuses every device in the
// process, because the executable declares an Agility SDK -- exported
// D3D12SDKVersion and D3D12SDKPath -- and the D3D12Core.dll that declaration
// names cannot be validated. A D3D11 game never notices: this add-on's private
// device is the first thing in the process to ask. Tainted Grail: The Fall of
// Avalon (#24), a Unity title, showed exactly this on a 4090 with a D3D12 folder
// that had already been renamed away, which changes nothing when the declared
// path is the executable's own folder. So say what is declared and what is at
// the path, and the report carries its own answer.
static void ExplainInvalidRedist()
{
    HMODULE exe = GetModuleHandleW(nullptr);
    const UINT *ver = reinterpret_cast<const UINT *>(GetProcAddress(exe, "D3D12SDKVersion"));
    const char *const *pp = reinterpret_cast<const char *const *>(GetProcAddress(exe, "D3D12SDKPath"));
    const char *path = pp != nullptr && *pp != nullptr ? *pp : nullptr;

    if (ver == nullptr && path == nullptr)
    {
        Log("[bridge]   0x887E0003 is D3D12_ERROR_INVALID_REDIST: the process's D3D12 SDK "
            "configuration is invalid. This executable exports neither D3D12SDKVersion nor "
            "D3D12SDKPath, so the declaration comes from somewhere else in the process.");
    }
    else
    {
        Log("[bridge]   0x887E0003 is D3D12_ERROR_INVALID_REDIST: the executable declares an "
            "Agility SDK -- D3D12SDKVersion %u, D3D12SDKPath \"%s\" -- and D3D12 could not "
            "validate the D3D12Core.dll that names. A D3D11 game never notices; this add-on's "
            "private device is the first thing in the process to ask.",
            ver != nullptr ? *ver : 0u, path != nullptr ? path : "(not exported)");

        // The path is relative to the executable's folder, by Microsoft's rule.
        wchar_t core[MAX_PATH] = {};
        GetModuleFileNameW(exe, core, MAX_PATH);
        wchar_t *sl = wcsrchr(core, (wchar_t)92);
        if (sl != nullptr)
        {
            wchar_t rel[MAX_PATH] = {};
            MultiByteToWideChar(CP_ACP, 0, path != nullptr ? path : ".\\", -1, rel, MAX_PATH);
            const size_t rl = wcslen(rel);
            const bool slashed = rl > 0 && (rel[rl - 1] == (wchar_t)92 || rel[rl - 1] == L'/');
            _snwprintf_s(sl + 1, MAX_PATH - (sl + 1 - core), _TRUNCATE, L"%ls%lsD3D12Core.dll",
                         rel, slashed || rl == 0 ? L"" : L"\\");

            if (GetFileAttributesW(core) == INVALID_FILE_ATTRIBUTES)
            {
                Log("[bridge]   %ls is MISSING. That alone produces this error. Verifying the "
                    "game's files is the first thing to try; a tool that removed or moved it "
                    "does exactly this.", core);
            }
            else
            {
                char fv[48] = "";
                FileVersionString(core, fv, sizeof(fv));
                // The redist exports the version it implements; the loader compares
                // it with the executable's. Mapped without running it, read, unmapped.
                unsigned int exported = 0;
                HMODULE m = LoadLibraryExW(core, nullptr, DONT_RESOLVE_DLL_REFERENCES);
                if (m != nullptr)
                {
                    const UINT *cv = reinterpret_cast<const UINT *>(GetProcAddress(m, "D3D12SDKVersion"));
                    if (cv != nullptr) exported = *cv;
                    FreeLibrary(m);
                }
                Log("[bridge]   %ls is present: file version %s, exports D3D12SDKVersion %u%s. "
                    "Verifying the game's files is the first thing to try; a copy another tool "
                    "replaced does exactly this.", core, fv[0] != 0 ? fv : "unknown", exported,
                    ver != nullptr && exported != *ver ? " -- which does NOT match the executable's declaration" : "");
            }
        }
    }

    HMODULE loaded = GetModuleHandleW(L"D3D12Core.dll");
    if (loaded != nullptr)
    {
        wchar_t lp[MAX_PATH] = {};
        GetModuleFileNameW(loaded, lp, MAX_PATH);
        char lv[48] = "";
        FileVersionString(lp, lv, sizeof(lv));
        Log("[bridge]   D3D12Core.dll already in the process: %ls, file version %s.", lp,
            lv[0] != 0 ? lv : "unknown");
    }
}

static bool BridgeInitSession(ID3D11Device *dev11, ID3D11DeviceContext *ctx)
{
    Log("");
    Breadcrumb("opening the D3D12 session");
    Log("################ bridge: opening D3D12 session ################");
    CfgReload();
    // Once-latched, so a second call site is free -- and it needs one. Its only
    // caller was the synthetic path's effects callback, which SynthRegisterEvents
    // does not register at all when stage=0 or source=off, so exactly the sessions
    // most likely to have been configured by hand were the ones that never got the
    // warning. By the time a D3D12 session opens, every add-on has loaded.
    WarnIfOldCopyLoaded();
    // The full set is echoed by CfgReload above, on the first read and on every
    // change. This used to print five of the eighteen keys under the same
    // "[bridge] config:" prefix, so a reader grepping for that prefix got whichever
    // line came first and a key set that stopped being complete in 1.0.17.
    CfgSayWhich();

    IDXGIDevice  *dxgi_dev = nullptr;
    IDXGIAdapter *adapter  = nullptr;
    if (SUCCEEDED(dev11->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi_dev))) &&
        dxgi_dev != nullptr)
    {
        dxgi_dev->GetAdapter(&adapter);
        dxgi_dev->Release();
    }

    // Which GPU and which driver: NGX feature availability depends on both, and
    // a report without them cannot be told apart from a bug in here.
    if (adapter != nullptr)
    {
        DXGI_ADAPTER_DESC ad = {};
        adapter->GetDesc(&ad);
        // The adapter, its ids and the driver are recorded at the first DLSS
        // call, which every game reaches before this point. Repeating them here
        // is four lines of nothing.

        // Windows resets a GPU that has not answered within TdrDelay seconds.
        // The default is 2, but it is a registry value and people change it, so
        // a stall figure in this log cannot be read against a guess.
        {
            DWORD delay = 0, ddi = 0, cb = sizeof(DWORD);
            const LSTATUS a1 = RegGetValueW(HKEY_LOCAL_MACHINE,
                L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
                L"TdrDelay", RRF_RT_REG_DWORD, nullptr, &delay, &cb);
            cb = sizeof(DWORD);
            const LSTATUS a2 = RegGetValueW(HKEY_LOCAL_MACHINE,
                L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
                L"TdrDdiDelay", RRF_RT_REG_DWORD, nullptr, &ddi, &cb);
            Log("[bridge] GPU reset timeout: TdrDelay %s, TdrDdiDelay %s",
                a1 == ERROR_SUCCESS ? "set" : "default 2 s",
                a2 == ERROR_SUCCESS ? "set" : "default 5 s");
            if (a1 == ERROR_SUCCESS) Log("[bridge]   TdrDelay = %lu s", delay);
            if (a2 == ERROR_SUCCESS) Log("[bridge]   TdrDdiDelay = %lu s", ddi);
        }

        adapter->QueryInterface(__uuidof(IDXGIAdapter3),
                                reinterpret_cast<void **>(&g_bridge.adapter3));
        LogVideoMemory("at session open");

    }

    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    auto create_device = d3d12 != nullptr
        ? reinterpret_cast<PFN_D3D12CreateDevice_>(GetProcAddress(d3d12, "D3D12CreateDevice"))
        : nullptr;
    if (create_device == nullptr) { Log("[bridge] no D3D12CreateDevice"); goto fail; }

    // Prey 2017 faults inside D3D12Core.dll during this call, on the same driver
    // that four other titles open a session on without trouble. It is also the
    // only reported process carrying two third-party NGX exporters besides the
    // driver and the snippets -- a winmm proxy injector and the Luma add-on --
    // and an injector that has hooked device creation would fault here exactly
    // like this. Whether the function has been patched before us is one line to
    // establish and was never printed.
    LogPrologue("D3D12CreateDevice", reinterpret_cast<const BYTE *>(create_device));
    Log("[bridge]   ReShade hooks this function by design, so a jump here is normal "
        "and Baldur's Gate 3 shows one while working. What is worth comparing "
        "between reports is the bytes, not the fact of a jump: a second hook on "
        "top of ReShade's is what a game-side injector would leave.");
    if (g_cfg.dred != 0) EnableDred(d3d12);
    else Log("[bridge] DRED is off by config; a GPU reset will not be explained.");

    {
        DWORD dcode = 0;
        HRESULT hr = SafeCreateDevice(create_device, adapter, &g_bridge.dev12, &dcode);
        // A retry lived here that announced "turning DRED off and trying once
        // more" and then called the identical function under the identical
        // process-global setting: D3D12_DRED_ENABLEMENT_DISABLED is written
        // nowhere in this add-on. Had the second attempt happened to succeed it
        // would have reported DRED as the cause of a configuration that never
        // existed. dred=0 is the only thing that produces a DRED-off attempt.
        // One second attempt on the DEFAULT adapter, and only after a FAULT on a
        // named one. Asked for in issue #11: Final Fantasy XV faults inside
        // D3D12Core.dll at the same module offset on two different drivers, with
        // the adapter taken from the game's own D3D11 device. This add-on's other
        // D3D12 path -- the synthetic transport in synth.inc -- has always passed
        // nullptr here, so the two were inconsistent and only one of them had ever
        // been tried on that title.
        //
        // Not a retry of a FAILURE: an HRESULT is an answer and is left alone. A
        // fault is the runtime losing its footing, and the only variable this side
        // controls is which adapter it was handed. On one GPU the two are the same
        // device and this costs a call; where they are not, it is the difference
        // between a session and no session at all.
        if (dcode != 0 && adapter != nullptr)
        {
            Log("[bridge] D3D12CreateDevice raised 0x%08X on the game's own adapter. "
                "Trying once on the default adapter, which is a different argument "
                "and not a different setting.", dcode);
            DWORD dcode2 = 0;
            HRESULT hr2 = SafeCreateDevice(create_device, nullptr, &g_bridge.dev12, &dcode2);
            if (dcode2 == 0 && SUCCEEDED(hr2) && g_bridge.dev12 != nullptr)
            {
                Log("[bridge] the default adapter worked where the game's own faulted. "
                    "The two are the same GPU on a single-GPU machine, so if this line "
                    "appears the fault was in what was passed rather than in the device.");
                dcode = 0; hr = hr2;
            }
            else if (dcode2 != 0)
                Log("[bridge] the default adapter faulted too, 0x%08X. The adapter is "
                    "not the variable.", dcode2);
            else
                Log("[bridge] the default adapter answered 0x%08X without faulting.", hr2);
        }
        if (dcode != 0)
        {
            {
                wchar_t owner[MAX_PATH] = {};
                if (NameFaultOwner(owner, MAX_PATH))
                    Log("[bridge]   the fault was at %p, inside %ls",
                        g_last_fault_at, owner);
            }
            Log("[bridge] D3D12CreateDevice raised 0x%08X inside the D3D12 runtime. "
                "Nothing this add-on can do about that, so it stops here and the "
                "game keeps its own rendering.%s", dcode,
                g_cfg.dred != 0
                    ? " DRED was requested for this session; set dred=0 in "
                      "dlss5-bridge.cfg to make an attempt without it."
                    : " DRED was off for this attempt.");
            goto fail;
        }
        if (FAILED(hr) || g_bridge.dev12 == nullptr)
        {
            Log("[bridge] D3D12CreateDevice failed 0x%08X", hr);
            if (hr == static_cast<HRESULT>(0x887E0003)) ExplainInvalidRedist();
            goto fail;
        }

        // The D3D11 synthetic transport comes through here rather than through
        // Synth12Open, so the bootstrap has to be at both call sites. A game with
        // no DLSS of its own has never loaded NGX -- nothing had a reason to --
        // and this is where Skyrim Special Edition stopped on 2026-08-30 with the
        // contract already built. Guarded on the source: on the mirror path,
        // nothing exporting NGX means the game never called DLSS, and that is a
        // fact to report rather than one to paper over by loading NGX for it.
        HMODULE ngx = FindNgxLoader();
        if (ngx == nullptr && g_source == SRC_SYNTH && EnsureNgxLoaderLoaded())
            ngx = FindNgxLoader();
        if (ngx == nullptr) { Log("[bridge] NGX D3D12 exports not present"); goto fail; }

        auto init_ext = reinterpret_cast<PFN_Init_Ext>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_Ext"));
        g_bridge.alloc_params = reinterpret_cast<PFN_AllocateParameters>(
            GetProcAddress(ngx, "NVSDK_NGX_D3D12_AllocateParameters"));
        g_bridge.create_feature = reinterpret_cast<PFN_D3D12CreateFeature>(
            GetProcAddress(ngx, "NVSDK_NGX_D3D12_CreateFeature"));
        g_bridge.eval_feature = reinterpret_cast<PFN_D3D12EvaluateFeature>(
            GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature"));
        g_bridge.release_feature = reinterpret_cast<PFN_D3D12ReleaseFeature>(
            GetProcAddress(ngx, "NVSDK_NGX_D3D12_ReleaseFeature"));

        LogEntryBytes("NVSDK_NGX_D3D12_CreateFeature",
                      reinterpret_cast<void *>(g_bridge.create_feature));
        LogEntryBytes("NVSDK_NGX_D3D12_EvaluateFeature",
                      reinterpret_cast<void *>(g_bridge.eval_feature));

        if (init_ext == nullptr || g_bridge.alloc_params == nullptr ||
            g_bridge.create_feature == nullptr || g_bridge.eval_feature == nullptr)
        { Log("[bridge] required NGX exports missing"); goto fail; }

        // NGX is told where the application data lives. This was the add-on's own
        // folder unconditionally, which is the game's folder in every title reported
        // so far -- but ReShade can load add-ons from elsewhere, and Phantasy Star
        // Online 2 is the first report where it does. There the NGX snippets sit
        // beside the executable, and that session came up with every capability
        // reading zero on an RTX 5080, which that card does not otherwise do.
        //
        // Whether the data path causes that is not established. What is established
        // is that pointing NGX at a folder holding none of its files was never
        // intended, so the executable's folder wins when the two differ, and both
        // are logged so a report can settle it.
        wchar_t data_path[MAX_PATH] = {};
        wchar_t self_dir[MAX_PATH] = {};
        GetModuleFileNameW(g_self, self_dir, MAX_PATH);
        if (wchar_t *sd = wcsrchr(self_dir, (wchar_t)92)) *(sd + 1) = 0;

        wchar_t exe_dir[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe_dir, MAX_PATH);
        if (wchar_t *ed = wcsrchr(exe_dir, (wchar_t)92)) *(ed + 1) = 0;

        wcscpy_s(data_path, _wcsicmp(self_dir, exe_dir) == 0 ? self_dir : exe_dir);
        Log("[bridge] NGX data path: %ls", data_path);
        if (_wcsicmp(self_dir, exe_dir) != 0)
            Log("[bridge]   the add-on itself is in %ls. The game's folder is used "
                "because that is where the NGX snippets are.", self_dir);

        // The SDK version constant is not documented and has moved between NGX
        // releases, so it is negotiated rather than assumed. Hardcoding the one
        // value that worked on the machine this was written on made the whole
        // session fail on a different driver. Init_ProjectID is tried as well:
        // some drivers accept one entry point and fault on the other.
        auto init_project = reinterpret_cast<PFN_Init_ProjectID>(
            GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_ProjectID"));

        // Nothing may patch a module between here and the end of the init: NGX
        // loads its own snippets inside these calls, and the loader notification
        // that fires for them arrives on this very thread.
        g_ngx_init_in_flight = true;

        Breadcrumb("initialising NGX on D3D12");
        DWORD code    = 0;
        bool  inited  = false;
        bool  faulted = false;

        // "Refused" and "faulted" are not the same answer and must not be
        // treated the same. A refusal is NGX declining a version constant, and
        // trying the next one is the whole point of this loop. A fault means NGX
        // died inside itself, and everything it owns is now in an unknown state
        // -- calling back in is what turned a disabled add-on into a killed
        // process on Final Fantasy VII Remake. Stop at the first one.
        for (int ver = 0x13; ver <= 0x16 && !inited && !faulted; ++ver)
        {
            if (init_ext != nullptr)
            {
                NVSDK_NGX_Result r = SafeInitExt(init_ext, 0x1000000ULL, data_path,
                                                 g_bridge.dev12, ver, &code);
                if (code != 0)
                {
                    {
                        wchar_t owner[MAX_PATH] = {};
                        if (NameFaultOwner(owner, MAX_PATH))
                            Log("[bridge] Init_Ext(version=0x%02X) raised 0x%08X at %p, inside %ls",
                                ver, code, g_last_fault_at, owner);
                        else
                            Log("[bridge] Init_Ext(version=0x%02X) raised 0x%08X", ver, code);
                    }
                    faulted = true;
                }
                else
                {
                    Log("[bridge] Init_Ext(version=0x%02X) -> %s", ver,
                        r == NGX_SUCCESS ? "ok" : "refused");
                    if (r == NGX_SUCCESS) inited = true;
                }
            }
            if (!inited && !faulted && init_project != nullptr)
            {
                NVSDK_NGX_Result r = SafeInitProjectID(init_project,
                                                       "a0f57b54-1daf-4934-90ae-c4035c19df04",
                                                       data_path, g_bridge.dev12, ver, &code);
                if (code != 0)
                {
                    {
                        wchar_t owner[MAX_PATH] = {};
                        if (NameFaultOwner(owner, MAX_PATH))
                            Log("[bridge] Init_ProjectID(version=0x%02X) raised 0x%08X at %p, inside %ls",
                                ver, code, g_last_fault_at, owner);
                        else
                            Log("[bridge] Init_ProjectID(version=0x%02X) raised 0x%08X", ver, code);
                    }
                    faulted = true;
                }
                else
                {
                    Log("[bridge] Init_ProjectID(version=0x%02X) -> %s", ver,
                        r == NGX_SUCCESS ? "ok" : "refused");
                    if (r == NGX_SUCCESS) inited = true;
                }
            }
        }
        g_ngx_init_in_flight = false;

        if (faulted && g_ngx_proxy[0] != 0)
        {
            Log("[bridge] %ls exports the NGX entry points as well and is not the "
                "driver's loader, so an NGX proxy is running in this process. Such a "
                "proxy patches code inside the driver's NGX module and opens its own "
                "session on it before this one. Prey 2017 with OptiScaler installed as "
                "WINMM.dll is the reported case, and it is the difference from titles "
                "where this same driver and module initialise. Removing the proxy is "
                "the test that would settle it.", g_ngx_proxy);
        }
        if (faulted)
        {
            Log("[bridge] NGX raised an exception opening its D3D12 session, so the");
            Log("  bridge stops here rather than calling into it again. The game keeps");
            Log("  rendering on its own; nothing else in this add-on runs this session.");
            goto fail;
        }
        if (!inited)
        { Log("[bridge] no NGX D3D12 init entry point was accepted by this driver"); goto fail; }
        Log("[bridge] NGX D3D12 session initialised");

        // What this GPU and driver will actually agree to run. Without it a
        // report of "nothing happens" cannot be told apart from the feature
        // simply not being available on the hardware.
        if (auto get_caps = reinterpret_cast<PFN_GetCapabilityParameters>(
                GetProcAddress(ngx, "NVSDK_NGX_D3D12_GetCapabilityParameters")))
        {
            NVSDK_NGX_Parameter *caps = nullptr;
            NVSDK_NGX_Result rc = SafeGetCaps(get_caps, &caps, &code);
            if (code != 0)              Log("[bridge] capability query raised 0x%08X", code);
            else if (rc != NGX_SUCCESS)
                Log("[bridge] capability query failed 0x%08X, %s", rc, NgxResultName(rc));
            else if (caps != nullptr)   { Log("[bridge] NGX capabilities:"); DumpCapability(caps); }

            // Whether this driver's loader drives the neural snippet itself, and
            // whether the snippet beside us is one it faults on. Asked of the
            // loader rather than of its file version: DLSSNR.Available exists in
            // 32.0.16.1664's _nvngx.dll and in no earlier one, so its presence is
            // the fact rather than a number that could be renumbered. See
            // NrSnippetIsOlderThanLoader for what the pairing does.
            if (caps != nullptr && code == 0)
            {
                // A loader that has no such name answers UnsupportedParameter,
                // 0xBAD00010; one that has it answers success whatever the value.
                // The value itself is not the question -- the NAME existing is.
                unsigned int nr_avail = 0;
                const NVSDK_NGX_Result nrq = caps->Get("DLSSNR.Available", &nr_avail);
                const bool loader_drives_nr = nrq == NGX_SUCCESS;
                char nrver[48] = "";
                const bool snippet_old = NrSnippetIsOlderThanLoader(nrver, sizeof(nrver));
                if (loader_drives_nr && snippet_old && !g_ngx_route_closed)
                {
                    Log("[bridge] *** This driver's NGX loader drives neural rendering itself -- it "
                        "answers DLSSNR.Available -- and nvngx_dlssnr.dll beside the game is %s, "
                        "which that route faults on: the snippet hands D3D12 a descriptor heap "
                        "whose description reads back as garbage, D3D12 faults inside "
                        "SetDescriptorHeaps, and the game then terminates or stops presenting. "
                        "Neither this add-on nor the DLSS 5 add-on is between the two in the fault "
                        "stack. Measured 2026-09-04 on 32.0.16.1664 against 32.0.16.1656. ***",
                        nrver[0] != 0 ? nrver : "an older build");
                    Log("[bridge]   That route is normally closed at attach: this add-on writes "
                        "an empty name over entry 18 of the loader's feature table, in memory, "
                        "so the loader loads no snippet for it and the DLSS 5 add-on drives "
                        "neural rendering itself. The line logged at attach says whether that "
                        "worked. It did not here, so either ngx_loader is set to 1 or the table "
                        "did not match this driver -- ngx_loader=2 then falls back to the "
                        "previous driver's loader if the driver store still holds one.");
                }
            }
        }

        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        g_bridge.dev12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue),
                                           reinterpret_cast<void **>(&g_bridge.queue));
        for (int i = 0; i < Bridge::kFrames; ++i)
            g_bridge.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   __uuidof(ID3D12CommandAllocator),
                                                   reinterpret_cast<void **>(&g_bridge.alloc[i]));
        if (g_bridge.alloc[0] != nullptr)
            g_bridge.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_bridge.alloc[0],
                                              nullptr, __uuidof(ID3D12GraphicsCommandList),
                                              reinterpret_cast<void **>(&g_bridge.list));
        if (g_bridge.list != nullptr) g_bridge.list->Close();
        g_bridge.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        // One fence, visible to both APIs, is what orders the two queues.
        HANDLE fh = nullptr;
        hr = g_bridge.dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence),
                                         reinterpret_cast<void **>(&g_bridge.fence12));
        if (SUCCEEDED(hr))
            hr = g_bridge.dev12->CreateSharedHandle(g_bridge.fence12, nullptr, GENERIC_ALL,
                                                    nullptr, &fh);
        ID3D11Device5 *dev5 = nullptr;
        if (SUCCEEDED(hr) &&
            SUCCEEDED(dev11->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void **>(&dev5))) &&
            dev5 != nullptr)
        {
            hr = dev5->OpenSharedFence(fh, __uuidof(ID3D11Fence),
                                       reinterpret_cast<void **>(&g_bridge.fence11));
            dev5->Release();
        }
        if (fh != nullptr) CloseHandle(fh);
        if (FAILED(hr) || g_bridge.fence11 == nullptr)
        { Log("[bridge] shared fence setup failed 0x%08X", hr); goto fail; }
        // Signalled by the queue only, never from the CPU, so it can be read
        // after fence12 has been pushed forward by hand. See BridgeRescueStalledQueue.
        if (FAILED(g_bridge.dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                               reinterpret_cast<void **>(&g_bridge.fence_progress))) ||
            FAILED(g_bridge.dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                               reinterpret_cast<void **>(&g_bridge.stall_fence))))
        { Log("[bridge] the progress fence could not be created"); goto fail; }

        if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext4),
                                       reinterpret_cast<void **>(&g_bridge.ctx4))) ||
            g_bridge.ctx4 == nullptr)
        { Log("[bridge] ID3D11DeviceContext4 unavailable"); goto fail; }

        Log("[bridge] context: %s",
            ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE ? "immediate" : "DEFERRED");

        if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11Multithread),
                                          reinterpret_cast<void **>(&g_bridge.mt))) &&
            g_bridge.mt != nullptr)
            Log("[bridge] multithread protection: %s",
                g_bridge.mt->GetMultithreadProtected() ? "on (locking per frame)" : "off");

        NVSDK_NGX_Result rp = SafeAllocParams(g_bridge.alloc_params, &g_bridge.params, &code);
        if (code != 0 || rp != NGX_SUCCESS || g_bridge.params == nullptr)
        { Log("[bridge] AllocateParameters failed 0x%08X, %s", rp, NgxResultName(rp)); goto fail; }

        // DRED reports a hung GPU by naming the command list and queue that
        // stopped. Unnamed objects come back as blanks, which is useless in a
        // process holding several of each.
        if (g_bridge.queue != nullptr) g_bridge.queue->SetName(L"dlss5-bridge queue");
        if (g_bridge.list  != nullptr) g_bridge.list->SetName(L"dlss5-bridge list");

        Log("[bridge] session ready: queue=%p list=%p fence12=%p fence11=%p (%d frames in flight)",
            static_cast<void *>(g_bridge.queue), static_cast<void *>(g_bridge.list),
            static_cast<void *>(g_bridge.fence12), static_cast<void *>(g_bridge.fence11),
            Bridge::kFrames);
        Log("############# bridge: session open #############");
        Log("");
        if (adapter != nullptr) adapter->Release();
        // The queue, the list and the event were created without checking any
        // of them, and every one is dereferenced on the acting path: the queue
        // in ExecuteCommandLists on the very first frame, the list in Reset, and
        // a null event is worse than a crash -- SetEventOnCompletion on a null
        // handle blocks the render thread with no timeout. The teardown path
        // already null-checks exactly these three; the guard existed where
        // nothing acts and was missing where everything does.
        if (g_bridge.queue == nullptr || g_bridge.list == nullptr ||
            g_bridge.fence_event == nullptr)
        {
            Log("[bridge] the command queue, list or fence event could not be "
                "created, so the session is not opened.");
            goto fail;
        }

        g_bridge.session_ready = true;
        return true;
    }

fail:
    if (adapter != nullptr) adapter->Release();
    BridgeDisable("the D3D12 session failed to start");
    Log("[bridge] disabled. Game rendering is untouched.");
    Log("############# bridge: session failed #############");
    return false;
}

// Creates the four shared textures on the D3D12 side and opens D3D11 aliases for
// them. The game's own textures have MiscFlags = 0 and cannot be shared, so the
// bridge owns its copies and moves data with CopyResource.
// What the D3D12 side actually received. A texture created on D3D11 and opened
// on D3D12 keeps its own idea of format and flags, and DLSS writes the output
// through a UAV: if ALLOW_UNORDERED_ACCESS did not survive the crossing, that
// write is undefined and the result accumulates as garbage rather than image.
static void LogD3D12Desc(int i)
{
    if (g_bridge.tex12[i] == nullptr) return;
    D3D12_RESOURCE_DESC rd = g_bridge.tex12[i]->GetDesc();
    Log("[bridge]   d3d12 desc: %llux%u fmt=%u flags=0x%X%s%s",
        rd.Width, rd.Height, rd.Format, rd.Flags,
        (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ? " UAV" : "",
        (rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) ? " SIMULTANEOUS" : "");
    if (i == SLOT_OUTPUT && !(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
        Log("[bridge]   *** Output has no UAV flag on the D3D12 side: DLSS cannot write it ***");

    if (g_bridge.dev12 != nullptr)
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {};
        fs.Format = rd.Format;
        if (SUCCEEDED(g_bridge.dev12->CheckFeatureSupport(
                D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))))
        {
            const bool sample = (fs.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) != 0;
            const bool uav    = (fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0;
            const bool load   = (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0;
            const bool store  = (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
            Log("[bridge]   fmt %u support: sample %s, typed UAV %s, UAV load %s, UAV store %s",
                rd.Format, sample ? "yes" : "NO", uav ? "yes" : "NO",
                load ? "yes" : "NO", store ? "yes" : "NO");
            if (i == SLOT_OUTPUT && !store)
                Log("[bridge]   *** the driver does not support typed UAV stores in this "
                    "format, and DLSS writes its result through one ***");
        }
    }
}

// One texture visible to both APIs. Two ways exist to get one and neither is
// guaranteed on a given driver, so both are attempted and the log records which
// one this machine accepts.
//
//   A. create on D3D12 (HEAP_FLAG_SHARED + ALLOW_SIMULTANEOUS_ACCESS, which D3D11
//      needs because it cannot perform D3D12 state transitions), open on D3D11.
//   B. create on D3D11 (SHARED_NTHANDLE | SHARED), open on D3D12.
static bool MakeSharedPair(ID3D11Device *dev11, ID3D11Device1 *dev1, int i,
                           UINT w, UINT h, DXGI_FORMAT fmt, bool uav)
{
    // --- attempt A -------------------------------------------------------
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = w;
    rd.Height           = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
                          (uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                               : D3D12_RESOURCE_FLAG_NONE);

    HRESULT hr = g_bridge.dev12->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void **>(&g_bridge.tex12[i]));
    if (SUCCEEDED(hr))
        hr = g_bridge.dev12->CreateSharedHandle(g_bridge.tex12[i], nullptr, GENERIC_ALL,
                                                nullptr, &g_bridge.shared[i]);
    if (SUCCEEDED(hr))
        hr = dev1->OpenSharedResource1(g_bridge.shared[i], __uuidof(ID3D11Texture2D),
                                       reinterpret_cast<void **>(&g_bridge.tex11[i]));
    if (SUCCEEDED(hr))
    {
    {
        wchar_t n[64];
        _snwprintf_s(n, _TRUNCATE, L"dlss5-bridge %hs", kSlotName[i]);
        g_bridge.tex12[i]->SetName(n);
    }
        Log("[bridge] %-6s %ux%u fmt=%u  via D3D12->D3D11  d3d12=%p d3d11=%p",
            kSlotName[i], w, h, fmt, static_cast<void *>(g_bridge.tex12[i]),
            static_cast<void *>(g_bridge.tex11[i]));
        return true;
    }
    Log("[bridge] %s: D3D12->D3D11 path failed 0x%08X, trying the other direction",
        kSlotName[i], hr);

    if (g_bridge.tex11[i]  != nullptr) { g_bridge.tex11[i]->Release();  g_bridge.tex11[i]  = nullptr; }
    if (g_bridge.tex12[i]  != nullptr) { g_bridge.tex12[i]->Release();  g_bridge.tex12[i]  = nullptr; }
    if (g_bridge.shared[i] != nullptr) { CloseHandle(g_bridge.shared[i]); g_bridge.shared[i] = nullptr; }

    // --- attempt B -------------------------------------------------------
    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = w;
    td.Height           = h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = fmt;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE |
                          (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
    td.MiscFlags        = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

    hr = dev11->CreateTexture2D(&td, nullptr, &g_bridge.tex11[i]);
    if (FAILED(hr))
    { Log("[bridge] %s: CreateTexture2D failed 0x%08X", kSlotName[i], hr); return false; }

    IDXGIResource1 *dxgi_res = nullptr;
    hr = g_bridge.tex11[i]->QueryInterface(__uuidof(IDXGIResource1),
                                           reinterpret_cast<void **>(&dxgi_res));
    if (SUCCEEDED(hr))
    {
        hr = dxgi_res->CreateSharedHandle(nullptr,
                                          DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                          nullptr, &g_bridge.shared[i]);
        dxgi_res->Release();
    }
    if (SUCCEEDED(hr))
        hr = g_bridge.dev12->OpenSharedHandle(g_bridge.shared[i], __uuidof(ID3D12Resource),
                                              reinterpret_cast<void **>(&g_bridge.tex12[i]));
    if (FAILED(hr))
    {
        Log("[bridge] %s: D3D11->D3D12 path failed 0x%08X", kSlotName[i], hr);
        // The caller may retry this slot with another format, and the retry
        // overwrites both pointers. Let go of them here or they are lost.
        if (g_bridge.tex11[i]  != nullptr) { g_bridge.tex11[i]->Release();  g_bridge.tex11[i]  = nullptr; }
        if (g_bridge.shared[i] != nullptr) { CloseHandle(g_bridge.shared[i]); g_bridge.shared[i] = nullptr; }
        return false;
    }

    {
        wchar_t n[64];
        _snwprintf_s(n, _TRUNCATE, L"dlss5-bridge %hs", kSlotName[i]);
        g_bridge.tex12[i]->SetName(n);
    }
    Log("[bridge] %-6s %ux%u fmt=%u  via D3D11->D3D12  d3d12=%p d3d11=%p",
        kSlotName[i], w, h, fmt, static_cast<void *>(g_bridge.tex12[i]),
        static_cast<void *>(g_bridge.tex11[i]));
    LogD3D12Desc(i);
    return true;
}

// Sizes and the NGX contract are taken from g_bridge, which BridgeFrame fills in
// from the game's own parameter block. Nothing here is derived or assumed: in an
// upscaling mode the Output texture is larger than the inputs, and the rendered
// area is smaller than the texture that holds it.
static void CopyUIntParam(const NVSDK_NGX_Parameter *src, const char *key, unsigned int fallback);
static bool BridgeCreatedUInt(const char *key, unsigned int *value);
static bool BridgeCreateUInt(const NVSDK_NGX_Parameter *gp, const char *key, unsigned int *value)
{
    return BridgeCreatedUInt(key, value) || (gp != nullptr && gp->Get(key, value) == NGX_SUCCESS);
}

// One field, three meanings: g_cfg.flags carries the user's forced value, -1 for
// "copy whoever is driving", and 107 for the default this add-on shipped before
// 1.0.16 and now treats as unset. A second contract source would be a fourth
// meaning in the same int, and the log line that reports the value would have no
// way left to say where it came from -- which is the failure the note above
// NoteAbsentParam names. So the origin is returned alongside the value and the
// label is read off it, rather than reconstructed from the config at the point
// of printing.
enum FlagsFrom { FLAGS_NONE, FLAGS_GAME, FLAGS_SYNTH, FLAGS_CONFIG };

static const char *kFlagsFromLabel[] = {
    // FLAGS_NONE is 107 with nobody behind it -- no source and no config -- and
    // says "forced by config" because that is what it said before this function
    // existed, and a log line that changes meaning breaks every comparison
    // against an older session.
    "sent to D3D12, forced by config",
    "sent to D3D12, copied from the game",
    "sent to D3D12, built by the synthetic contract",
    "sent to D3D12, forced by config",
};

// What actually went into the parameter block NGX was handed, and which of the
// four origins above put it there. Both are locals at the two places that Set
// the key -- BridgeBuildResources here and Synth12Build in synth.inc -- and the
// status panel has to name them from ReShade's UI thread, where dereferencing
// either parameter block to ask again is not allowed. So they are published
// here instead of read back there. -1 means no create has happened yet, which
// is a different answer from flags 0 and the panel says so.
//
// Written by whichever thread built the feature and read by the UI thread with
// no lock: a LONG and a pointer to a string literal, both aligned, so a reader
// sees one value or the other and never half of one. The literal is static
// storage that outlives the process, so the pointer cannot go stale.
static volatile LONG  g_panel_flags = -1;
static const char    *g_panel_flags_from;

// Precedence: an explicit config value, else the active source's own, else 107.
// FLAGS_SYNTH has no producer in this build; it is declared so the value and its
// label arrive together the first time one exists, instead of being retrofitted
// into a field that already carries too much.
static unsigned int ResolveCreateFlags(const NVSDK_NGX_Parameter *gp, FlagsFrom *from)
{
    if (g_cfg.flags >= 0 && g_cfg.flags != 107)
    {
        *from = FLAGS_CONFIG;
        return static_cast<unsigned int>(g_cfg.flags);
    }

    // A source that is present is the source, whether or not it declared this
    // particular key: 107 stands in for the key, not for the source. Whether the
    // game declared it is a separate question, and the line above this one --
    // "the game's create flags" -- is what answers it.
    if (gp != nullptr)
    {
        unsigned int v = 0;
        // Which source, asked of g_source rather than reconstructed from the
        // value. A synthetic block answers this Get and would otherwise be
        // labelled "copied from the game" in a session where there is no game
        // to copy from -- a log line stating something it did not derive, and
        // the one thing the flags storage was split per source to prevent.
        // Absence still means 107 here, so the synthetic block being complete
        // is the self-check's business, not this line's.
        *from = (g_source == SRC_SYNTH) ? FLAGS_SYNTH : FLAGS_GAME;
        return gp->Get("DLSS.Feature.Create.Flags", &v) == NGX_SUCCESS ? v : 107u;
    }

    *from = FLAGS_NONE;
    return 107u;
}

static bool BridgeCreateViewFeature(const DXGI_FORMAT *fmt, const NVSDK_NGX_Parameter *gp);

static bool BridgeBuildResources(ID3D11Device *dev11, const DXGI_FORMAT *fmt,
                                 const NVSDK_NGX_Parameter *gp)
{
    Breadcrumb("building shared textures");
    const UINT w = g_bridge.width, h = g_bridge.height;
    BridgeReleaseTextures();

    ID3D11Device1 *dev1 = nullptr;
    if (FAILED(dev11->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void **>(&dev1))) ||
        dev1 == nullptr)
    { Log("[bridge] ID3D11Device1 unavailable"); return false; }

    bool ok = true;
    g_bridge.depth_placeholder = false;
    g_bridge.depth_converted   = false;
    g_bridge.mv_converted      = false;
    for (int i = 0; i < SLOT_COUNT && ok; ++i)
    {
        // Depth crosses as R32_FLOAT, written by a compute pass, because its
        // native R24G8_TYPELESS cannot be shared. It therefore needs a UAV too.
        const bool        is_depth   = (i == SLOT_DEPTH);
        DXGI_FORMAT       share_fmt  = is_depth ? DXGI_FORMAT_R32_FLOAT
                                                : TypedFormat(fmt[i]);
        if (!is_depth && share_fmt != fmt[i])
            Log("[bridge] %s is %s; the shared copy uses %s, because nothing can view "
                "a typeless texture", kSlotKey[i], FormatName(fmt[i]), FormatName(share_fmt));
        // MV needs a UAV only on the fallback path, where a compute pass
        // writes the shared copy instead of CopyResource.
        const bool        needs_uav  = is_depth || (i == SLOT_OUTPUT) || (i == SLOT_MV);
        // Every mirror is sized from the texture it mirrors. Depth and motion
        // vectors used to be sized from Color, which is only correct while all
        // three agree: a game supplying display-resolution motion vectors -- the
        // configuration the MVLowRes create flag exists to distinguish -- got a
        // Color-sized mirror, CopyResource silently refused the mismatch, and
        // DLSS then read a motion-vector texture nobody had ever written.
        const UINT        tw = g_bridge.slot_w[i];
        const UINT        th = g_bridge.slot_h[i];

        bool made = MakeSharedPair(dev11, dev1, i, tw, th, share_fmt, needs_uav);

        // The game's own MV format can be one the driver will not share --
        // R32G32_FLOAT is absent from the kernel's shared-format list, and
        // CreateTexture2D then rejects it in every flag combination. R16G16_FLOAT,
        // the DLSS-recommended motion-vector format, is in every list, so retry
        // with that and convert per frame, the way depth is converted. TypedFormat
        // has already had its pass by here, so a typeless game MV that shares fine
        // never reaches this; a retried mirror is the only one that needs converting.
        bool mv_fallback = false;
        if (!made && i == SLOT_MV)
        {
            share_fmt = DXGI_FORMAT_R16G16_FLOAT;
            Log("[bridge] MV: format %u cannot be shared on this driver; retrying as R16G16_FLOAT",
                fmt[i]);
            made = MakeSharedPair(dev11, dev1, i, tw, th, share_fmt, needs_uav);
            mv_fallback = true;
        }

        if (made)
        {
            if (i == SLOT_MV)
            {
                // A retried mirror is R16G16_FLOAT while the game's own texture is
                // not, so CopyResource cannot fill it: the conversion pass does.
                if (mv_fallback)
                {
                    // The conversion pass writes the shared MV through a UAV.
                    D3D11_UNORDERED_ACCESS_VIEW_DESC mu = {};
                    mu.Format        = share_fmt;
                    mu.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
                    if (SUCCEEDED(dev11->CreateUnorderedAccessView(g_bridge.tex11[i], &mu, &g_bridge.mv_uav)) &&
                        g_bridge.mv_uav != nullptr &&
                        BridgeMakeMVShader(dev11))
                    {
                        g_bridge.mv_converted = true;
                        Log("[bridge] MV will be converted fmt=%u -> R16G16_FLOAT by a compute pass",
                            fmt[i]);
                        continue;
                    }
                    // Without the conversion pass there is no way to fill the
                    // shared MV, and DLSS reading a texture nobody writes smears
                    // the image. Stop cleanly rather than deliver that.
                    Breadcrumb("setting up the MV conversion pass");
                    Log("[bridge] MV conversion setup failed; this game's MV format %u cannot be bridged",
                        fmt[i]);
                    if (g_bridge.mv_uav    != nullptr) { g_bridge.mv_uav->Release();    g_bridge.mv_uav    = nullptr; }
                    if (g_bridge.tex11[i]  != nullptr) { g_bridge.tex11[i]->Release();  g_bridge.tex11[i]  = nullptr; }
                    if (g_bridge.tex12[i]  != nullptr) { g_bridge.tex12[i]->Release();  g_bridge.tex12[i]  = nullptr; }
                    if (g_bridge.shared[i] != nullptr) { CloseHandle(g_bridge.shared[i]); g_bridge.shared[i] = nullptr; }
                    ok = false;
                }
                else
                    continue;
            }
            if (!is_depth) continue;

            D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
            ud.Format        = DXGI_FORMAT_R32_FLOAT;
            ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            HRESULT hr = dev11->CreateUnorderedAccessView(g_bridge.tex11[i], &ud,
                                                          &g_bridge.depth_uav);
            if (SUCCEEDED(hr) && BridgeMakeDepthShader(dev11))
            {
                g_bridge.depth_converted = true;
                // The source format is not known here -- it is read from the
                // game's texture at blit time, and that is where it is logged.
                // This line used to name R24_UNORM_X8_TYPELESS unconditionally,
                // which read as a wrong conversion on every game whose depth is
                // not BG3's.
                Log("[bridge] depth will be converted to R32_FLOAT for DLSS");
                continue;
            }
            Log("[bridge] depth UAV/shader setup failed 0x%08X, falling back", hr);
            if (g_bridge.depth_uav != nullptr) { g_bridge.depth_uav->Release(); g_bridge.depth_uav = nullptr; }
            if (g_bridge.tex11[i]  != nullptr) { g_bridge.tex11[i]->Release();  g_bridge.tex11[i]  = nullptr; }
            if (g_bridge.tex12[i]  != nullptr) { g_bridge.tex12[i]->Release();  g_bridge.tex12[i]  = nullptr; }
            if (g_bridge.shared[i] != nullptr) { CloseHandle(g_bridge.shared[i]); g_bridge.shared[i] = nullptr; }
        }

        // Depth is allowed to degrade rather than take the whole bridge down:
        // a D3D12-only texture still satisfies the NGX contract. Committed
        // resources on a DEFAULT heap are zero-filled, and with DepthInverted
        // set in the create flags that reads as the far plane rather than noise.
        if (i == SLOT_DEPTH)
        {
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width            = w;
            rd.Height           = h;
            rd.DepthOrArraySize = 1;
            rd.MipLevels        = 1;
            rd.Format           = DXGI_FORMAT_R32_FLOAT;
            rd.SampleDesc.Count = 1;
            rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            HRESULT hr = g_bridge.dev12->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                __uuidof(ID3D12Resource), reinterpret_cast<void **>(&g_bridge.tex12[i]));
            if (SUCCEEDED(hr))
            {
                g_bridge.depth_placeholder = true;
                Log("[bridge] Depth  %ux%u  PLACEHOLDER (R32_FLOAT, zero-filled, not shared)", w, h);
                Log("[bridge] the game's real depth is not crossing; image quality will suffer");
                continue;
            }
            Log("[bridge] Depth placeholder failed 0x%08X", hr);
        }
        ok = false;
    }
    dev1->Release();
    if (!ok) { BridgeReleaseTextures(); return false; }
    return BridgeCreateViewFeature(fmt, gp);
}

static bool BridgeCreateViewFeature(const DXGI_FORMAT *fmt, const NVSDK_NGX_Parameter *gp)
{
    const UINT w = g_bridge.width, h = g_bridge.height;
    // The feature is created with the game's own render and output sizes, not
    // with the texture sizes: in an upscaling mode those are not the same thing.
    g_bridge.params->Set("Width", g_bridge.render_w);
    g_bridge.params->Set("Height", g_bridge.render_h);
    g_bridge.params->Set("OutWidth", g_bridge.ngx_out_w);
    g_bridge.params->Set("OutHeight", g_bridge.ngx_out_h);
    // The quality preset is the game's statement of which DLSS model it asked
    // for, and it was hardcoded to 2. That is Baldur's Gate 3's own answer
    // again -- the same mistake as the create flags, in the line above them.
    // Assetto Corsa and Gallipoli both ask for 1 and were given 2.
    unsigned int quality = 2;
    BridgeCreateUInt(gp, "PerfQualityValue", &quality);
    g_bridge.params->Set("PerfQualityValue", quality);
    {
        unsigned int q = 0, asked_q = 0;
        g_bridge.params->Get("PerfQualityValue", &q);
        static const char *kQ[] = { "MaxPerf", "Balanced", "MaxQuality",
                                    "UltraPerformance", "UltraQuality", "DLAA" };
        const bool have_q =
            BridgeCreateUInt(gp, "PerfQualityValue", &asked_q);
        Log("[bridge] quality preset %u (%s), %s", q,
            q < _countof(kQ) ? kQ[q] : "unknown",
            have_q ? "copied from the game" : "the game set none, so 2 is used");
    }

    // The create flags describe the game's own buffers: whether the colour is
    // HDR, whether the depth is reversed, whether the motion vectors are at
    // render resolution. Forcing a value here invents that description. The
    // default 107 was Baldur's Gate 3's own answer, so it was right there and
    // nowhere else: Assetto Corsa asks for 2, and was being told its SDR colour
    // is HDR and its normal depth is inverted. Copy it, and let the config
    // force a value only when it is set to something other than -1.
    // 107 was this add-on's own default up to and including 1.0.14, so a configuration file
    // carrying it is repeating that default rather than stating a choice --
    // and it keeps every existing installation on the old behaviour, where the
    // correction reaches only whoever reads the log. Trails beyond the Horizon
    // showed the cost: the game asks for 75, 107 adds DoSharpening, and the
    // game had set Sharpness to 1.0 without ever enabling it. The old default
    // is treated as unset, and the substitution is announced.
    const bool old_default = (g_cfg.flags == 107);

    FlagsFrom          flags_from = FLAGS_NONE;
    unsigned int resolved = ResolveCreateFlags(gp, &flags_from);
    if ((g_cfg.flags == -1 || old_default) && BridgeCreatedUInt("DLSS.Feature.Create.Flags", &resolved))
        flags_from = FLAGS_GAME;
    const bool         copying    = (flags_from == FLAGS_GAME);

    g_bridge.params->Set("DLSS.Feature.Create.Flags", resolved);
    {
        unsigned int used = 0;
        g_bridge.params->Get("DLSS.Feature.Create.Flags", &used);
        unsigned int asked = 0;
        const bool have_asked =
            BridgeCreateUInt(gp, "DLSS.Feature.Create.Flags", &asked);

        if (have_asked) LogCreateFlags("the game's create", asked);
        LogCreateFlags(kFlagsFromLabel[flags_from], used);
        // Published for the panel from the same value the line above prints, so
        // the two can never disagree about what was sent.
        // Origin first, then the value: the interlocked write is the barrier, so
        // a reader that sees a value has already been able to see the label that
        // goes with it.
        g_panel_flags_from = kFlagsFromLabel[flags_from];
        InterlockedExchange(&g_panel_flags, static_cast<LONG>(used));

        if (old_default && copying)
            Log("[bridge]   the configuration file says flags=107, which was this "
                "add-on's default up to and including 1.0.14 rather than a description of this "
                "game. It is being ignored in favour of the game's own value. To "
                "force it, use flags=108 -- any value that is not 107 or -1 is "
                "taken as deliberate.");

        if (!copying && have_asked && asked != used)
            Log("[bridge]   these differ. Set flags=-1 in dlss5-bridge.cfg to "
                "copy the game's own value; the file is re-read while playing.");

        // DLSS applies its sharpening pass only when DoSharpening is set, and
        // reads its strength from Sharpness whether or not the game meant it to
        // be used. Adding the flag over a game that set one and not the other
        // turns on a pass it deliberately left off.
        if ((used & 0x20) != 0 && have_asked && (asked & 0x20) == 0)
        {
            float sharp = 0.0f;
            gp->Get("Sharpness", &sharp);
            Log("[bridge]   DoSharpening is set here but not by the game, and "
                "Sharpness is %.2f. DLSS will sharpen at that strength; the game's "
                "own DLSS would not have.", sharp);
        }

        // IsHDR is the flag the DLSS 5 add-on reads to choose between its HDR and
        // its SDR codec, so asserting it over an 8-bit buffer sends it down the
        // wrong path without any call failing.
        if ((used & 0x01) != 0 && FormatIsLowPrecisionUnorm(g_bridge.fmt[SLOT_COLOR]))
            Log("[bridge]   IsHDR is set, but the game's colour buffer is 8-bit "
                "normalised (fmt=%u) and cannot hold a value above 1.0. The DLSS 5 "
                "add-on selects its codec from this flag.",
                static_cast<unsigned>(g_bridge.fmt[SLOT_COLOR]));
    }
    // Mirror the game's own answer where it gave one. Deus Ex: Mankind Divided
    // asks for 0 here while BG3 asks for 1, and forcing either on the other is
    // inventing part of a contract that is meant to be copied.
    unsigned int output_subrects = static_cast<unsigned int>(g_cfg.subrects);
    BridgeCreateUInt(gp, "DLSS.Enable.Output.Subrects", &output_subrects);
    g_bridge.params->Set("DLSS.Enable.Output.Subrects", output_subrects);
    // The render preset picks which DLSS model weights run, and the D3D12 feature
    // was getting NGX's default while the game's own D3D11 feature ran whatever
    // it asked for -- so the two sides upscaled with different models and the
    // neural-rendering add-on worked from ours. Prey 2017 reaches DLSS through
    // Luma, which sets these; so do DLSSTweaks and OptiScaler, and so do plenty
    // of games directly.
    //
    // nvsdk_ngx_helpers.h does not write these keys at all, so unlike MV.Scale
    // there is no neutral to fall back on: absence means NVSDK_NGX_DLSS_Hint_
    // Render_Preset_Default, which is what an unset key already gives. Mirror
    // only what the game set. Key strings are from nvsdk_ngx_defs.h.
    if (gp != nullptr)
    {
        static const char *preset_keys[] = {
            "DLSS.Hint.Render.Preset.DLAA",
            "DLSS.Hint.Render.Preset.Quality",
            "DLSS.Hint.Render.Preset.Balanced",
            "DLSS.Hint.Render.Preset.Performance",
            "DLSS.Hint.Render.Preset.UltraPerformance",
            "DLSS.Hint.Render.Preset.UltraQuality",
        };
        for (const char *k : preset_keys)
        {
            unsigned int v = 0;
            const bool present = BridgeCreateUInt(gp, k, &v);
            g_bridge.params->Set(k, v);
            if (present && !g_bridge.presets_reported)
                Log("[bridge] %s = %u, copied from the game", k, v);
        }
        g_bridge.presets_reported = true;
    }


    // The four resource keys, set to the textures that were just built, BEFORE
    // the create rather than only at the first evaluate.
    //
    // They used to arrive here holding the PREVIOUS build's pointers, dangling
    // since BridgeReleaseTextures freed them, and a neighbour add-on that reads
    // this block at CreateFeature read freed memory -- the 0xC0000005 fixed
    // earlier today. Clearing them to null fixed that and left a second problem
    // in its place: a DLSS 5 add-on captures the contract at the create, and on
    // every REBUILD it now captured one whose four resources are null. On the
    // first build of a session they were unset either way, which is why this only
    // shows after a resolution, preset, HDR or mode change.
    //
    // Neither state is a description of anything. These are the exact resources
    // the evaluates will use, they exist by this line, and naming them is the
    // only answer here that is true.
    // The synthetic contract on an HDR10 presentation; see PqBackBuffer. The
    // mirror copies a game's own contract and never sees a PQ surface as colour.
    if (InterlockedCompareExchange(&g_source, 0, 0) == SRC_SYNTH &&
        PqBackBuffer(TypedFormat(fmt[SLOT_COLOR])) &&
        !PqPassMake(&g_bridge.pq, g_bridge.dev12, g_bridge.tex12[SLOT_COLOR], g_bridge.tex12[SLOT_OUTPUT]))
    {
        Log("[bridge] the HDR10 conversion could not be built, so the synthetic contract is not "
            "built either: an SDR evaluate over a PQ surface is the picture this exists to fix.");
        return false;
    }

    g_bridge.params->Set("Color",         g_bridge.pq.in  != nullptr ? g_bridge.pq.in  : g_bridge.tex12[SLOT_COLOR]);
    g_bridge.params->Set("Output",        g_bridge.pq.out != nullptr ? g_bridge.pq.out : g_bridge.tex12[SLOT_OUTPUT]);
    g_bridge.params->Set("Depth",         g_bridge.tex12[SLOT_DEPTH]);
    g_bridge.params->Set("MotionVectors", g_bridge.tex12[SLOT_MV]);

    g_bridge.params->Set("CreationNodeMask", 1u);
    g_bridge.params->Set("VisibilityNodeMask", 1u);
    g_bridge.params->Set("RTXValue", 0);

    if (!BeginCommands()) { Log("[bridge] could not start a command list"); return false; }

    DWORD code = 0;
    Breadcrumb("creating the D3D12 DLSS feature");
    NVSDK_NGX_Result rf = SafeCreateFeature(g_bridge.create_feature, UnwrapList(g_bridge.list),
                                            g_bridge.params, &g_bridge.feature, &code);
    const UINT64 v = EndCommands();

    // Feature creation is a one-off, so block until it has actually run -- but
    // still on the game's render thread, so not for long enough to be mistaken
    // for a hang.
    if (g_bridge.fence12->GetCompletedValue() < v)
    {
        g_bridge.fence12->SetEventOnCompletion(v, g_bridge.fence_event);
        if (WaitForSingleObject(g_bridge.fence_event, 4000) != WAIT_OBJECT_0 ||
            g_bridge.fence12->GetCompletedValue() < v)
        {
            Log("[bridge] feature creation did not complete within 4 s; giving up.");
            BridgeDisable("creating the DLSS feature crashed");
            return false;
        }
    }

    if (code != 0)
    {
        // What was observed, first, and on its own line: SafeCreateFeature runs
        // under CaptureFault, so the faulting address is known and the module
        // that owns it can be named. Which module it is decides the diagnosis --
        // the driver's own loader, a snippet sitting in the game folder, or
        // another add-on's detour in front of the call are three different
        // problems -- and none of them can be told apart from a sentence that
        // names a cause instead of an address.
        wchar_t who[MAX_PATH + 64] = {};
        Warn("creating the DLSS feature raised exception 0x%08X.", code);
        if (NameFaultOwner(who, _countof(who)))
            Log("[bridge]   it faulted in %ls", who);
        else
            Log("[bridge]   no faulting address was recorded, so the module cannot "
                "be named.");

        // Offered as something to try, not as the cause. A reporter on 2026-08-31
        // made every nvngx_*.dll on the machine the same 310.8.0.0 and the create
        // still faulted, so a version mismatch is one explanation among several
        // rather than the answer.
        Log("[bridge]   NGX loads its feature snippets from the game folder before the");
        Log("[bridge]   driver's own copy, so an nvngx_*.dll dropped in there is worth");
        Log("[bridge]   removing as a test. It is not established as the cause: the same");
        Log("[bridge]   fault has been reported with every copy on the machine matching.");
        BridgeDisable("the DLSS feature could not be created");
        return false;
    }
    if (rf != NGX_SUCCESS || g_bridge.feature == nullptr)
    {
        Log("[bridge] CreateFeature failed with result 0x%08X, %s", rf, NgxResultName(rf));
        if (rf == static_cast<NVSDK_NGX_Result>(0xBAD0000B))
            Log("[bridge]   NGX could not build the feature from the create "
                "parameters above. Those are the width, height, output size, "
                "creation flags and quality preset logged just before this line.");
        return false;
    }

    Log("[bridge] feature ready: render %ux%u -> output %ux%u  (textures in %ux%u out %ux%u) handle=%p",
        g_bridge.render_w, g_bridge.render_h, g_bridge.ngx_out_w, g_bridge.ngx_out_h,
        w, h, g_bridge.out_width, g_bridge.out_height, static_cast<void *>(g_bridge.feature));
    // One readback per feature, 60 frames in, so a scenario that changes the
    // swapchain format three times measures each of them.
    g_bridge.hashed     = false;
    g_bridge.hash_after = g_bridge.frames_done + 60;
    {
        int fl = -1;
        if (g_bridge.params != nullptr) g_bridge.params->Get("DLSS.Feature.Create.Flags", &fl);
        InterlockedExchange(&g_present_flags, fl);
        Log("[bridge] presentation: output %s, colour space %s, contract %s%s.",
            FormatName(fmt[SLOT_OUTPUT]),
            ColorSpaceName(InterlockedCompareExchange(&g_present_space, 0, 0)),
            (fl >= 0 && (fl & 1)) ? "HDR (IsHDR)" : "SDR",
            g_present_expo > 0.0f ? ", exposure texture supplied" : "");
    }

    // A DLSS 5 add-on captures its contract by detouring
    // NVSDK_NGX_D3D12_CreateFeature and reading the create it sees. It arms when
    // NGX appears -- and on the synthetic path NGX appears because THIS add-on
    // loaded it, so the two race and the create can win. Skyrim Special Edition
    // on 2026-08-30: create at 22:37:44.856, the other add-on's hook at .883.
    // Twenty-seven milliseconds, and the neural pass never saw a contract.
    //
    // The prologue is taken here so a later change can be seen. A detour is
    // written over the first bytes of the export, so those bytes changing is
    // that add-on arriving -- which is a fact about memory rather than a guess
    // about how long it takes.
    if (!g_bridge.create_prologue_taken && g_bridge.create_feature != nullptr)
    {
        memcpy(g_bridge.create_prologue,
               reinterpret_cast<const void *>(g_bridge.create_feature),
               sizeof(g_bridge.create_prologue));
        g_bridge.create_prologue_taken = true;
    }

    // What a DLSS 5 add-on will ask of the textures the bridge hands it, asked
    // here first. Escape from Tarkov failed on exactly this and said so only
    // from the other add-on's side, as "output format 1 is not a supported typed
    // codec format" -- by which point the bridge looked healthy and the fault
    // looked like someone else's. These textures are the bridge's own, so a
    // refusal here is a bug here.
    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        if (i != SLOT_OUTPUT && i != SLOT_COLOR) continue;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {};
        fs.Format = (i == SLOT_DEPTH) ? DXGI_FORMAT_R32_FLOAT : TypedFormat(fmt[i]);
        if (FAILED(g_bridge.dev12->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                                                       &fs, sizeof(fs))))
            continue;
        const bool can_sample = (fs.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) != 0;
        const bool can_uav    = (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0 &&
                                (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
        const bool needs_uav  = (i == SLOT_OUTPUT);
        if (can_sample && (can_uav || !needs_uav)) continue;
        Warn("the bridge's %s texture is %s, which this GPU cannot %s. A DLSS 5 "
             "add-on needs that and will refuse the frame.",
             kSlotKey[i], FormatName(fs.Format),
             !can_sample ? "sample in a shader" : "write through a typed UAV");
    }
    for (int i = 0; i < SLOT_COUNT; ++i) g_bridge.fmt[i] = fmt[i];
    g_bridge.need_reset  = true;
    g_bridge.frame_ready = true;
    return true;
}

// Takes the next allocator in the ring, waiting first if the GPU has not yet
// finished with the frame that last used it.
static bool BeginCommands()
{
    const int slot = g_bridge.frame_slot;
    const UINT64 retire = g_bridge.alloc_fence[slot];
    // The progress fence, where there is one: fence12 is pushed forward from
    // the CPU when a stall releases the game, and an allocator handed back on
    // that evidence would be reset under work the GPU is still executing.
    ID3D12Fence *truth = g_bridge.fence_progress != nullptr ? g_bridge.fence_progress
                                                            : g_bridge.fence12;
    if (retire != 0 && truth->GetCompletedValue() < retire)
    {
        // This blocks the thread the game renders on, so it has to give up long
        // before a render-thread watchdog would fire. Half a second is the same
        // budget the rescue gives the game's own wait, and a timeout here is the
        // same stall seen from the render thread rather than from the game's
        // queue: stand aside and let the queue catch up, do not stop.
        //
        // Until the value is reached or the deadline passes, not one wake: the
        // event is auto-reset, and a completion nobody was waiting for leaves it
        // signalled, so the next wait returns at once with the value still
        // short. Read as a verdict, that was a second stall 51 ms after the
        // first had resumed, in ngxGym's stall-resume.
        const ULONGLONG deadline = GetTickCount64() + 500;
        while (truth->GetCompletedValue() < retire)
        {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline)
            {
                BridgeEnterStall(truth->GetCompletedValue(), retire, "the render thread");
                return false;
            }
            truth->SetEventOnCompletion(retire, g_bridge.fence_event);
            WaitForSingleObject(g_bridge.fence_event, static_cast<DWORD>(deadline - now));
        }
    }
    if (g_bridge.alloc[slot] == nullptr) return false;
    if (FAILED(g_bridge.alloc[slot]->Reset())) return false;
    return SUCCEEDED(g_bridge.list->Reset(g_bridge.alloc[slot], nullptr));
}

// Frame N makes the game's queue wait for the D3D12 side to reach v_out. That
// wait cannot be cancelled and has no timeout, so if the work behind it never
// completes the game presents nothing ever again -- while its own loop, audio
// and input carry on, which is what a frozen image with a live character is.
// The 2 s check in BeginCommands cannot catch it: that code runs on the render
// thread, and the render thread is what the block puts to sleep inside Present.
//
// So the value is remembered and checked on the frames that follow, while the
// game is still submitting. Being behind by a frame or two is ordinary
// pipelining and is what keeps the CPU free; being behind by half a second is
// not. An ID3D12Fence can be signalled from the CPU, and that releases the
// game's queue whether or not the work ever arrives.
// ID3D12Fence::GetCompletedValue returns UINT64_MAX for exactly one reason: the
// device has been removed. Escape from Tarkov produced it mid-session, and
// nothing noticed -- the bridge went on copying its last good Output over the
// game's every frame, so the world froze while the interface, drawn after that
// copy, kept updating. Opening the inventory appeared to fix it.
static void LogDred()
{
    if (g_bridge.dev12 == nullptr) return;

    ID3D12DeviceRemovedExtendedData *dred = nullptr;
    if (FAILED(g_bridge.dev12->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedData),
                                              reinterpret_cast<void **>(&dred))) ||
        dred == nullptr)
    {
        Log("  DRED did not attach, so what the GPU was executing cannot be recovered.");
        return;
    }

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc = {};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc)))
    {
        int shown = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE *node = bc.pHeadAutoBreadcrumbNode;
             node != nullptr && shown < 8; node = node->pNext, ++shown)
        {
            const UINT done = node->pLastBreadcrumbValue != nullptr
                            ? *node->pLastBreadcrumbValue : 0;
            Log("  breadcrumb: list %ls on queue %ls finished %u of %u operations",
                node->pCommandListDebugNameW != nullptr
                    ? node->pCommandListDebugNameW : L"(unnamed)",
                node->pCommandQueueDebugNameW != nullptr
                    ? node->pCommandQueueDebugNameW : L"(unnamed)",
                done, node->BreadcrumbCount);

            // The operation at the completed count is the one still running when
            // the GPU stopped, which is the whole point of asking.
            if (node->pCommandHistory != nullptr && done < node->BreadcrumbCount)
            {
                Log("    stopped inside operation %u, a %s.", done,
                    BreadcrumbOpName(static_cast<unsigned>(node->pCommandHistory[done])));

                const UINT from = done > 12 ? done - 12 : 0;
                const UINT to   = done + 4 < node->BreadcrumbCount
                                ? done + 4 : node->BreadcrumbCount;
                Log("    operations %u to %u:", from, to - 1);
                for (UINT k = from; k < to; ++k)
                    Log("      %3u %s%s", k,
                        BreadcrumbOpName(static_cast<unsigned>(node->pCommandHistory[k])),
                        k == done ? "   <== stopped here" : "");
            }
        }
        if (bc.pHeadAutoBreadcrumbNode != nullptr)
            Log("  This add-on records four barriers, hands the list to NGX between "
                "a BeginEvent and an EndEvent, then records four more. An operation "
                "that is neither among the first five nor the last five belongs to "
                "NGX rather than to this add-on.");

        if (bc.pHeadAutoBreadcrumbNode == nullptr)
            Log("  no breadcrumbs were recorded, so the reset did not happen inside "
                "a command list this add-on submitted.");
    }

    D3D12_DRED_PAGE_FAULT_OUTPUT pf = {};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)) && pf.PageFaultVA != 0)
    {
        Log("  a page fault at GPU virtual address 0x%llX caused the reset.",
            static_cast<unsigned long long>(pf.PageFaultVA));
        int shown = 0;
        for (const D3D12_DRED_ALLOCATION_NODE *a = pf.pHeadExistingAllocationNode;
             a != nullptr && shown < 6; a = a->pNext, ++shown)
            Log("    inside live allocation %ls",
                a->ObjectNameW != nullptr ? a->ObjectNameW : L"(unnamed)");
        shown = 0;
        for (const D3D12_DRED_ALLOCATION_NODE *a = pf.pHeadRecentFreedAllocationNode;
             a != nullptr && shown < 6; a = a->pNext, ++shown)
            Log("    inside recently freed allocation %ls -- freed while the GPU "
                "was still reading it", a->ObjectNameW != nullptr ? a->ObjectNameW : L"(unnamed)");
    }
    dred->Release();
}

static bool BridgeDeviceLost()
{
    if (g_bridge.fence12 == nullptr) return false;
    if (g_bridge.fence12->GetCompletedValue() != UINT64_MAX) return false;

    LogVideoMemory("at the moment the device was lost");

    const HRESULT why = g_bridge.dev12 != nullptr
                      ? g_bridge.dev12->GetDeviceRemovedReason() : 0;
    Log("[bridge] the D3D12 device has been removed (reason 0x%08X).", why);
    switch (why)
    {
    case DXGI_ERROR_DEVICE_HUNG:
        Log("  The GPU stopped responding to work submitted on it -- a TDR."); break;
    case DXGI_ERROR_DEVICE_REMOVED:
        Log("  The adapter was removed or reset underneath the process."); break;
    case DXGI_ERROR_DEVICE_RESET:
        Log("  The device was reset after an invalid command."); break;
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
        Log("  The driver reported an internal error."); break;
    default: break;
    }
    LogDred();
    Log("  Nothing can be recovered from here, and continuing would copy a frozen");
    Log("  image over the game's own every frame. Stopping.");
    BridgeDisable("the D3D12 device was removed");
    return true;
}

// Release the game and stand aside, rather than stop for good. Baldur's Gate 3
// on a 4060 Ti (#16) ran 1200 frames and then the private queue completed
// nothing for half a second, once; the device was not removed and the game
// carried on, so the work was slow, not dead. Stopping on that turned every
// such hiccup into "DLSS 5 gets disabled after a minute". Now the game's wait
// is released from the CPU, its own DLSS runs meanwhile, and delivery resumes
// when the progress fence shows the queue has caught up with everything that
// was submitted. A queue that never does is still stopped, from
// BridgeFrameInner, four seconds on. Two callers: the rescue, which watches
// the game's wait, and BeginCommands, which is the render thread's own wait
// for an allocator and used to stop the bridge by itself after two seconds.
static void BridgeEnterStall(UINT64 done, UINT64 wanted, const char *who)
{
    if (g_bridge.stalled) return;
    // Six in a minute is a GPU that is not keeping up; six in three hours is a
    // GPU that hiccups, which is what this exists to survive.
    const ULONGLONG t = GetTickCount64();
    if (t - g_bridge.stall_last > 60000) g_bridge.stalls = 0;
    g_bridge.stall_last = t;
    ++g_bridge.stalls;
    Log("[bridge] the D3D12 side has completed nothing for 500 ms while %s waits on", who);
    Log("  fence %llu (completed %llu). Releasing the game's wait from the CPU so it can",
        wanted, done);
    Log("  present again, and standing aside until the D3D12 side catches up: its work");
    Log("  is still queued, and the neural pass runs inside it. The game's own DLSS runs");
    Log("  meanwhile. Stall %d in the last minute.", g_bridge.stalls);
    if (g_bridge.pending_out != 0) g_bridge.fence12->Signal(g_bridge.pending_out);
    g_bridge.stalled       = true;
    g_bridge.stall_since   = GetTickCount64();
    // Everything submitted so far, not only what the game waits on: the
    // allocators behind the newest value are what BeginCommands must not reuse.
    // Submitted, not taken: fence_value is already one ahead on the frame that
    // stalls inside BeginCommands, and that value is never signalled.
    g_bridge.stall_release = g_bridge.last_submitted;
    g_bridge.pending_out   = 0;
    if (g_bridge.stalls > 5)
    {
        Log("[bridge] six stalls in one minute: this GPU is not keeping up with this work, "
            "and a stutter every few seconds is worse than no neural pass.");
        BridgeDisable("the D3D12 side keeps stalling");
    }
}

static void BridgeRescueStalledQueue()
{
    // Once the bridge has stopped there is nothing left to rescue, and without
    // this the device-removed diagnosis below repeats on every later frame.
    if (g_bridge.disabled) return;
    if (g_bridge.fence12 == nullptr) return;
    if (BridgeDeviceLost()) return;

    const ULONGLONG now = GetTickCount64();
    if (g_bridge.stall_release_at != 0 && now >= g_bridge.stall_release_at)
    {
        g_bridge.stall_release_at = 0;
        g_bridge.stall_fence->Signal(g_bridge.stall_target);
        Log("[bridge] stall_test: the hold is released.");
    }

    // The progress fence, not fence12: the stall below pushes fence12 forward
    // from the CPU to release the game, and after that fence12 no longer says
    // what the GPU has done.
    const UINT64 done = g_bridge.fence_progress != nullptr
                            ? g_bridge.fence_progress->GetCompletedValue()
                            : g_bridge.fence12->GetCompletedValue();

    // The only honest measure of "the GPU stopped completing" is that the
    // completed value has stopped moving. Being a frame or two behind is
    // pipelining and is the state this bridge is built to keep; two earlier
    // attempts here timed the wrong quantity and were wrong in both directions
    // -- one that could never fire while frames arrived, then one that fired
    // while Baldur's Gate 3 was zero frames behind at 120 fps.
    if (done != g_bridge.last_completed)
    {
        g_bridge.last_completed = done;
        g_bridge.pending_since  = now;
        if (g_bridge.pending_out != 0 && done >= g_bridge.pending_out)
            g_bridge.pending_out = 0;
        return;
    }

    // Nothing outstanding means nothing to rescue, however long it has been.
    if (g_bridge.pending_out == 0 || done >= g_bridge.pending_out) return;
    if (now - g_bridge.pending_since < 500) return;

    BridgeEnterStall(done, g_bridge.pending_out, "the game's queue");
}

// Submits, then records which fence value retires the allocator just used.
static UINT64 EndCommands()
{
    g_bridge.list->Close();
    ID3D12CommandList *lists[] = { g_bridge.list };
    // stall_test: once, at the 60th submission, the queue is made to wait on a
    // fence nothing signals for that many milliseconds. Every submission behind
    // it stops completing, which is the shape of #16, produced on purpose.
    if (g_cfg.stall_test > 0 && !g_bridge.stall_test_done && g_bridge.fence_value >= 60 &&
        g_bridge.stall_fence != nullptr)
    {
        g_bridge.stall_test_done  = true;
        g_bridge.stall_target     = 1;
        g_bridge.stall_release_at = GetTickCount64() + static_cast<ULONGLONG>(g_cfg.stall_test);
        g_bridge.queue->Wait(g_bridge.stall_fence, g_bridge.stall_target);
        Log("[bridge] stall_test: the D3D12 queue is held for %d ms from here.", g_cfg.stall_test);
    }
    g_bridge.queue->ExecuteCommandLists(1, lists);

    const UINT64 v = ++g_bridge.fence_value;
    g_bridge.queue->Signal(g_bridge.fence12, v);
    if (g_bridge.fence_progress != nullptr) g_bridge.queue->Signal(g_bridge.fence_progress, v);
    g_bridge.last_submitted = v;
    g_bridge.alloc_fence[g_bridge.frame_slot] = v;
    g_bridge.frame_slot = (g_bridge.frame_slot + 1) % Bridge::kFrames;
    return v;
}

static void Barrier(ID3D12GraphicsCommandList *list, ID3D12Resource *res,
                    D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = res;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter  = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &b);
}

// Copies one float or int parameter straight across from the game's block, so the
// D3D12 evaluate sees the same jitter and exposure the game computed this frame.
// A failed Get is not required to leave the destination alone, so the result is
// checked rather than assumed. Silently forwarding a zeroed value is how a
// parameter the game never set becomes a wrong one the bridge invented.
static void CopyUIntParam(const NVSDK_NGX_Parameter *src, const char *key, unsigned int fallback)
{
    unsigned int v = 0;
    if (src->Get(key, &v) != NGX_SUCCESS) v = fallback;
    g_bridge.params->Set(key, v);
}


// A note for whoever adds a second source of contract to this file.
//
// Everything from here down to MirrorFloatParam exists to tell "the game
// declared X" apart from "the game declared nothing". That distinction is a
// signal only while absence is abnormal. A synthetic contract -- ReShade depth
// and estimated motion vectors, for a game that ships no DLSS of its own -- is a
// legitimate second source, but it has to arrive already complete: every key set
// explicitly by whatever builds it, and no call into the helpers below. Route one
// through them and absence becomes a normal operating mode, NoteAbsentParam turns
// into noise on half the frames, and the report that found the MV.Scale defect
// stops being able to find the next one.
//
// The same holds for the size fallback in BridgeFrame, where absent scalars are
// derived from the Color and Output descriptors. That is a last-resort repair for
// a game that under-declares, not a supply route.
//
// DLSS.Feature.Create.Flags needs storage per source rather than one field. It
// already carries two meanings -- the game's declared value, and 107 standing for
// unset -- and adding an authoritative third from a synthetic source or a user
// override to the same field is how this acquires a defect it cannot diagnose.

// A parameter the game did not set is not a parameter whose default this bridge
// gets to choose. Trails beyond the Horizon sets no MV.Scale, and the -1.0
// invented for it told DLSS to reverse every motion vector: the temporal
// history is then gathered from the wrong side of the movement, which is the
// smeared ground and foliage reported there. Whatever NGX does with an unset
// parameter is exactly what the game's own DLSS call gets, so leaving it unset
// is the only way to match. These mirror; they do not fill in.
//
// For the mirror an absent key is news about the game. For a synthetic contract
// it is news about this add-on: the note above requires such a contract to
// arrive already complete, so arriving here at all means the synthesiser left a
// key unset and the value that follows was chosen by nobody. Same call site,
// opposite meaning, so it cannot be the same sentence.
static void NoteAbsentParam(const char *key)
{
    static const char *seen[32];
    static int         count;
    static LONG        seen_source = SRC_NONE;

    // Thirty-two slots between every source there will ever be. One source that
    // runs first fills them and the next one's report comes out empty, which
    // reads exactly like a complete contract. Start the table again instead:
    // both accounts are wanted in full, and a repeat after a handover is the
    // point rather than noise.
    const LONG src = g_source;
    if (src != seen_source) { count = 0; seen_source = src; }

    for (int i = 0; i < count; ++i)
        if (strcmp(seen[i], key) == 0) return;
    if (count < static_cast<int>(_countof(seen))) seen[count++] = key;

    if (src == SRC_SYNTH)
    {
        Log("[synth] DEFECT: the synthetic contract sets no %s, and this path exists "
            "to describe a game that under-declares. A contract built here is built "
            "complete or not at all, so the neutral now being sent is standing in for "
            "a decision nothing made.", key);
        return;
    }
    Log("[bridge] the game sets no %s; the neutral from nvsdk_ngx_helpers.h is sent "
        "instead, which is what a game using the official helper would send.", key);
}

// Sends the game's value where it has one, and the neutral from
// nvsdk_ngx_helpers.h where it does not -- which is what every game using the
// official helper sends. Absence is still reported once per key, because that
// report is what found the MV.Scale defect in the first place.
static void NeutralFloatParam(const NVSDK_NGX_Parameter *src, const char *key,
                              float neutral)
{
    float v = 0.0f;
    if (src->Get(key, &v) != NGX_SUCCESS || v == 0.0f)
    {
        NoteAbsentParam(key);
        v = neutral;
    }
    g_bridge.params->Set(key, v);
}

static void NeutralUIntParam(const NVSDK_NGX_Parameter *src, const char *key,
                             unsigned int neutral)
{
    unsigned int v = 0;
    if (src->Get(key, &v) != NGX_SUCCESS) { NoteAbsentParam(key); v = neutral; }
    g_bridge.params->Set(key, v);
}

// Jitter and sharpness are the two the helper passes straight through with no
// substitution, so there is no neutral to fall back on and a wrong value is
// worse than none. Every game that runs DLSS sets jitter; sharpness matters
// only with DoSharpening, which such a game also sets.
static void MirrorFloatParam(const NVSDK_NGX_Parameter *src, const char *key)
{
    float v = 0.0f;
    if (src->Get(key, &v) != NGX_SUCCESS) { NoteAbsentParam(key); return; }
    g_bridge.params->Set(key, v);
}

// Mount & Blade II: Bannerlord creates its feature with flags 59 -- IsHDR,
// MVLowRes, DepthInverted, DoSharpening -- and no AutoExposure, then supplies a
// 1x1 ExposureTexture on every evaluate. The bridge copied the flags faithfully
// and mirrored four textures, so its D3D12 feature ran with auto exposure off
// and no exposure input at all: DLSS accumulated without knowing the exposure,
// which is ghosting. Before 1.0.16 the forced flags=107 carried AutoExposure and
// hid this; copying the game's flags removed that cover.
//
// Kept out of the slot array on purpose. Games that supply no exposure texture
// are the common case and every SLOT_COUNT loop would have to learn about a
// null.
static void BridgeReleaseExposure()
{
    // NGX does not AddRef the resources in a parameter block -- that is why
    // games must keep them alive across the call -- and this block is allocated
    // once per session and never Reset. Freeing the texture without clearing
    // the key leaves the driver a freed pointer on the very next evaluate.
    // The drain is this file's own rule: a submitted command list may still
    // reference the surface, and up to three are in flight.
    if (g_bridge.exp12 != nullptr)
    {
        BridgeDrainGpu();
        if (g_bridge.params != nullptr)
            g_bridge.params->Set("ExposureTexture", static_cast<ID3D12Resource *>(nullptr));
    }
    if (g_bridge.exp12   != nullptr) { g_bridge.exp12->Release();   g_bridge.exp12   = nullptr; }
    if (g_bridge.exp11   != nullptr) { g_bridge.exp11->Release();   g_bridge.exp11   = nullptr; }
    if (g_bridge.exp_shared != nullptr) { CloseHandle(g_bridge.exp_shared); g_bridge.exp_shared = nullptr; }
    g_bridge.exp_w = g_bridge.exp_h = 0;
}
static void BridgeReleaseExposureFwd() { BridgeReleaseExposure(); }


// Builds the mirror on first use and whenever the game's exposure texture
// changes shape. Returns false when it cannot be built, which is not fatal:
// the frame then runs exactly as it did before, without exposure.
static bool BridgeEnsureExposure(ID3D11Device *dev11, ID3D11Texture2D *src)
{
    D3D11_TEXTURE2D_DESC sd = {};
    src->GetDesc(&sd);

    if (g_bridge.exp12 != nullptr && g_bridge.exp_w == sd.Width &&
        g_bridge.exp_h == sd.Height && g_bridge.exp_fmt == sd.Format)
        return true;

    BridgeReleaseExposure();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = sd.Width;
    td.Height           = sd.Height;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    // Every other mirror runs the game's format through TypedFormat, and for a
    // recorded reason: a typeless mirror reaches the D3D12 add-on and it refuses
    // the surface while this bridge still looks healthy. PSO2 supplies a 1x1
    // R32_TYPELESS exposure texture, so skipping it here would have left PSO2
    // with the exact defect this mirror was written to fix.
    td.Format           = TypedFormat(sd.Format);
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags        = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

    HRESULT hr = dev11->CreateTexture2D(&td, nullptr, &g_bridge.exp11);
    if (SUCCEEDED(hr))
    {
        IDXGIResource1 *r = nullptr;
        hr = g_bridge.exp11->QueryInterface(__uuidof(IDXGIResource1),
                                            reinterpret_cast<void **>(&r));
        if (SUCCEEDED(hr))
        {
            hr = r->CreateSharedHandle(nullptr,
                                       DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                       nullptr, &g_bridge.exp_shared);
            r->Release();
        }
    }
    if (SUCCEEDED(hr))
        hr = g_bridge.dev12->OpenSharedHandle(g_bridge.exp_shared, __uuidof(ID3D12Resource),
                                              reinterpret_cast<void **>(&g_bridge.exp12));
    if (FAILED(hr))
    {
        Log("[bridge] the exposure texture could not be mirrored 0x%08X. The session "
            "continues without it, which is what every release before this one did.", hr);
        BridgeReleaseExposure();
        return false;
    }

    g_bridge.exp_w   = sd.Width;
    g_bridge.exp_h   = sd.Height;
    g_bridge.exp_fmt = sd.Format;
    g_bridge.exp12->SetName(L"dlss5-bridge Exposure");
    if (!g_bridge.exp_reported)
    {
        g_bridge.exp_reported = true;
        Log("[bridge] the game supplies an exposure texture (%ux%u fmt=%u); it is "
            "mirrored to D3D12 so both sides run the same contract.",
            sd.Width, sd.Height, static_cast<unsigned>(sd.Format));
        // Its value, read once per mirror. A 1x1 R32 is the shape every exposure
        // texture in evidence has had, and DLSS scales its input by it; a value
        // far from 1 is worth knowing when a picture comes back wrong.
        if (sd.Width == 1 && sd.Height == 1 && sd.Format == DXGI_FORMAT_R32_FLOAT)
        {
            ID3D11DeviceContext *ctx = nullptr;
            dev11->GetImmediateContext(&ctx);
            D3D11_TEXTURE2D_DESC st = sd;
            st.Usage = D3D11_USAGE_STAGING; st.BindFlags = 0; st.MiscFlags = 0;
            st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ID3D11Texture2D *stage = nullptr;
            if (ctx != nullptr && SUCCEEDED(dev11->CreateTexture2D(&st, nullptr, &stage)) && stage != nullptr)
            {
                ctx->CopyResource(stage, src);
                D3D11_MAPPED_SUBRESOURCE m = {};
                if (SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m)))
                {
                    g_present_expo = *static_cast<const float *>(m.pData);
                    ctx->Unmap(stage, 0);
                    Log("[bridge]   exposure texture value %.4f", g_present_expo);
                }
                stage->Release();
            }
            if (ctx != nullptr) ctx->Release();
        }
    }
    return true;
}

// The size a super-resolution handle was CREATED with, per handle. The four
// scalars in an evaluate block are whatever create last wrote into that block,
// which need not be this feature's: Baldur's Gate 3's character creator creates
// a preview feature through the same block, and from then on every evaluate of
// the main feature carries 1920x1080 -> 1280x720 on a 3413x960 -> 5120x1440
// session, for the rest of the session (#18). Read as a contract those scalars
// name a partial region, and the bridge declined every frame until restart. The
// Vulkan mirror has read the create since 1.3.0 (VkmNoteGameCreate); this is
// the same thing for D3D11, per handle because that game holds two.
//
// Written by the create hook after it has released the NGX locks, read by the
// frame path; both under g_bridge_cs. Eight entries, oldest replaced: a game
// holds one or two live handles and NGX reuses freed addresses, which is why a
// create at a known address replaces its entry rather than adding one.
static const char *kBridgeCreateKeys[] = {
    "DLSS.Feature.Create.Flags", "PerfQualityValue", "DLSS.Enable.Output.Subrects",
    "DLSS.Hint.Render.Preset.DLAA", "DLSS.Hint.Render.Preset.Quality",
    "DLSS.Hint.Render.Preset.Balanced", "DLSS.Hint.Render.Preset.Performance",
    "DLSS.Hint.Render.Preset.UltraPerformance", "DLSS.Hint.Render.Preset.UltraQuality",
};
struct BridgeCreateShape {
    const NVSDK_NGX_Handle *handle;
    unsigned int w, h, ow, oh;
    unsigned int values[_countof(kBridgeCreateKeys)], present;
};
static BridgeCreateShape        g_create_shape[8];
static int                      g_create_shape_next;
static const NVSDK_NGX_Handle  *g_eval_handle;   // the evaluate being mirrored, set by its hook

static bool BridgeCreatedUInt(const char *key, unsigned int *value)
{
    if (g_source != SRC_MIRROR || g_eval_handle == nullptr) return false;
    for (const auto &shape : g_create_shape)
        if (shape.handle == g_eval_handle)
            for (unsigned i = 0; i < _countof(kBridgeCreateKeys); ++i)
                if ((shape.present & (1u << i)) && strcmp(key, kBridgeCreateKeys[i]) == 0)
                { *value = shape.values[i]; return true; }
    return false;
}

static void BridgeNoteCreate(const NVSDK_NGX_Handle *handle, const NVSDK_NGX_Parameter *p)
{
    if (handle == nullptr) return;
    unsigned int w = 0, h = 0, ow = 0, oh = 0;
    if (p != nullptr)
    {
        p->Get("Width", &w);     p->Get("Height", &h);
        p->Get("OutWidth", &ow); p->Get("OutHeight", &oh);
    }
    // All four or none: a half-stated shape would fall back to the evaluate for
    // the missing half and the two would disagree.
    if (w == 0 || h == 0 || ow == 0 || oh == 0)
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            Log("[bridge] the game's create block carries no complete size (%ux%u -> %ux%u), "
                "so the contract comes from the evaluate. Not a fault: not every engine "
                "states it there.", w, h, ow, oh);
        }
    }
    EnterCriticalSection(&g_bridge_cs);
    int slot = -1;
    for (int i = 0; i < static_cast<int>(_countof(g_create_shape)); ++i)
        if (g_create_shape[i].handle == handle) slot = i;
    if (slot < 0) slot = g_create_shape_next++ % static_cast<int>(_countof(g_create_shape));
    g_create_shape[slot] = { handle, w, h, ow, oh };
    if (p != nullptr)
        for (unsigned i = 0; i < _countof(kBridgeCreateKeys); ++i)
            if (p->Get(kBridgeCreateKeys[i], &g_create_shape[slot].values[i]) == NGX_SUCCESS)
                g_create_shape[slot].present |= 1u << i;
    // The game replacing its feature invalidates the mirror's, whatever the
    // shape: the create flags can change with nothing else -- IsHDR when a game
    // switches its HDR on -- and until now only a size or format change
    // rebuilt here, so the mirror ran the game's HDR frames through a feature
    // built for SDR. The Vulkan mirror has followed the game's create since
    // 1.3.0. Measured in ngxGym's brightness scenario, 2026-09-02.
    if (g_bridge.frame_ready)
    {
        g_bridge.frame_ready = false;
        g_bridge.need_reset  = true;
        Log("[bridge] the game created its feature again; the mirror rebuilds on the next frame.");
    }
    LeaveCriticalSection(&g_bridge_cs);
    Log("[bridge] the game's create states %ux%u -> %ux%u for handle %p. That is the shape "
        "its evaluates mean, in preference to the four scalars they carry.",
        w, h, ow, oh, static_cast<const void *>(handle));
}

// Under g_bridge_cs.
static bool BridgeCreateShapeOf(const NVSDK_NGX_Handle *handle, unsigned int *w,
                                unsigned int *h, unsigned int *ow, unsigned int *oh)
{
    if (handle == nullptr) return false;
    for (int i = 0; i < static_cast<int>(_countof(g_create_shape)); ++i)
        if (g_create_shape[i].handle == handle)
        {
            if (!g_create_shape[i].w || !g_create_shape[i].h || !g_create_shape[i].ow || !g_create_shape[i].oh)
                return false;
            *w = g_create_shape[i].w;   *h = g_create_shape[i].h;
            *ow = g_create_shape[i].ow; *oh = g_create_shape[i].oh;
            return true;
        }
    return false;
}

// The block this add-on handed to the D3D12 evaluate, read back from it. On a
// refusal the class of error is known and the parameter is not: PSO2 (#8)
// answered 0xBAD00005 three times and the log named nothing it could have been.
static void DumpSentParams()
{
    static const char *kU[] = {
        "Width", "Height", "OutWidth", "OutHeight",
        "DLSS.Render.Subrect.Dimensions.Width", "DLSS.Render.Subrect.Dimensions.Height",
        "DLSS.Input.Color.Subrect.Base.X", "DLSS.Input.Color.Subrect.Base.Y",
        "DLSS.Input.Depth.Subrect.Base.X", "DLSS.Input.Depth.Subrect.Base.Y",
        "DLSS.Input.MV.Subrect.Base.X", "DLSS.Input.MV.Subrect.Base.Y",
        "DLSS.Output.Subrect.Base.X", "DLSS.Output.Subrect.Base.Y",
        "DLSS.Enable.Output.Subrects", "DLSS.Feature.Create.Flags", "PerfQualityValue", "Reset",
    };
    static const char *kF[] = {
        "MV.Scale.X", "MV.Scale.Y", "Jitter.Offset.X", "Jitter.Offset.Y", "Sharpness",
        "DLSS.Pre.Exposure", "DLSS.Exposure.Scale",
    };
    if (g_bridge.params == nullptr) return;
    Log("[bridge]   the block sent to D3D12, as it reads back (feature %ux%u -> %ux%u):",
        g_bridge.render_w, g_bridge.render_h, g_bridge.ngx_out_w, g_bridge.ngx_out_h);
    for (size_t i = 0; i < _countof(kU); ++i)
    {
        unsigned int v = 0;
        if (g_bridge.params->Get(kU[i], &v) == NGX_SUCCESS) Log("[bridge]     %-40s = %u", kU[i], v);
        else Log("[bridge]     %-40s   not set", kU[i]);
    }
    for (size_t i = 0; i < _countof(kF); ++i)
    {
        float v = 0.0f;
        if (g_bridge.params->Get(kF[i], &v) == NGX_SUCCESS) Log("[bridge]     %-40s = %f", kF[i], v);
        else Log("[bridge]     %-40s   not set", kF[i]);
    }
}

// Under g_bridge_cs. Textures and queue stay shared; NGX's temporal state does not.
static bool BridgeSelectHistory(const NVSDK_NGX_Parameter *gp)
{
    const auto *owner = g_source == SRC_MIRROR ? g_eval_handle : nullptr;
    int free_slot = -1;
    for (int i = 0; i < static_cast<int>(_countof(g_bridge.histories)); ++i)
    {
        const auto &history = g_bridge.histories[i];
        if (history.feature != nullptr && history.game == owner)
        {
            g_bridge.feature = history.feature;
            return true;
        }
        if (history.feature == nullptr && free_slot < 0) free_slot = i;
    }
    // ponytail: bounded like the create registry; additional views keep native DLSS.
    // A future larger registry should retire histories on the game's ReleaseFeature.
    if (free_slot < 0)
    {
        static bool reported = false;
        if (!reported) { reported = true; Log("[bridge] all eight temporal histories are in use; additional game features keep their own DLSS."); }
        return false;
    }
    // A resource build already created the first feature. Additional views need
    // another feature only once, without reallocating the transport or resetting peers.
    if (free_slot != 0)
    {
        g_bridge.feature = nullptr;
        if (!BridgeCreateViewFeature(g_bridge.fmt, gp))
        {
            BridgeFail("view feature creation");
            return false;
        }
    }
    if (g_bridge.feature == nullptr) return false;
    g_bridge.histories[free_slot] = { owner, g_bridge.feature,
        g_bridge.render_w, g_bridge.render_h, g_bridge.ngx_out_w, g_bridge.ngx_out_h };
    Log("[bridge] temporal history: game=%p private=%p slot=%d",
        static_cast<const void *>(owner), static_cast<void *>(g_bridge.feature), free_slot);
    return true;
}

static void BridgeFrameInner(ID3D11DeviceContext *ctx, const NVSDK_NGX_Parameter *gp)
{
    BridgeRescueStalledQueue();
    if (g_bridge.disabled || !g_bridge.session_ready || gp == nullptr) return;

    // Standing aside after a stall: nothing is recorded or copied until the
    // progress fence reaches the value the game was released from. The game's
    // own DLSS runs on these frames (BridgeWillDeliver says no), so the picture
    // stays live.
    if (g_bridge.stalled)
    {
        const UINT64 done = g_bridge.fence_progress != nullptr
                                ? g_bridge.fence_progress->GetCompletedValue() : 0;
        const ULONGLONG now = GetTickCount64();
        if (done >= g_bridge.stall_release)
        {
            g_bridge.stalled = false;
            Log("[bridge] the D3D12 side caught up after %llu ms; delivering again. The "
                "game's own DLSS was on screen meanwhile.", now - g_bridge.stall_since);
        }
        else if (now - g_bridge.stall_since > 4000)
        {
            Log("[bridge] the D3D12 side has not caught up 4 s after the stall: fence %llu "
                "wanted, %llu completed.", g_bridge.stall_release, done);
            BridgeDisable("the D3D12 side never caught up after a stall");
            return;
        }
        else
        {
            ++g_bridge.frames_done;
            return;
        }
    }

    if (g_bridge.paused_until != 0)
    {
        if (GetTickCount64() < g_bridge.paused_until) return;
        g_bridge.paused_until  = 0;
        g_bridge.rebuilds      = 0;
        g_bridge.rebuild_since = GetTickCount64();
        if (!g_bridge.resume_reported)
        {
            g_bridge.resume_reported = true;
            Log("[bridge] resuming: trying the render shape again. Further pauses and "
                "resumes are not reported; the frame counter shows them.");
        }
    }

    // Signal and Wait are immediate-context operations. On a deferred context
    // they are illegal, the fence is never signalled, and the D3D11 queue then
    // waits forever on it -- which a game's render thread watchdog reports as a
    // hang rather than as anything traceable to this add-on.
    if (ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
    {
        Log("[bridge] DLSS runs on a deferred context here; the bridge cannot work "
            "and has stopped. The game is unaffected.");
        Log("  This bridge synchronises two APIs with a shared fence, and that only");
        Log("  works on the immediate context. Continuing would deadlock the render");
        Log("  thread, so the bridge is stopping. The game is unaffected.");
        BridgeDisable("DLSS runs on a deferred context here");
        return;
    }

    CfgReload();   // rate limited inside, on the clock rather than on frames

    // Stage 0 touches nothing at all, not even a read of the game's parameters.
    if (g_cfg.stage <= 0) { ++g_bridge.frames_done; return; }

    TexFacts             d[SLOT_COUNT] = {};
    ID3D11Texture2D     *g[SLOT_COUNT] = {};
    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        g[i] = GameTexture(gp, kSlotKey[i], &d[i]);
        if (g[i] == nullptr)
        {
            for (int k = 0; k < i; ++k) g[k]->Release();
            return;  // nothing to do this frame
        }
    }

    // A multisampled texture cannot be copied into a single-sample one, and a
    // TEXTURE2D view over it is illegal. Watch Dogs 2 at MSAA 2x hits both: the
    // depth view fails on a format the table maps correctly, and the colour copy
    // quietly does nothing, so DLSS would be handed whatever the textures held
    // last. The game's own DLSS fails there too, before the bridge touches it.
    //
    // This waits rather than disabling for the session: turning MSAA off is the
    // usual fix, and clearing frame_ready means the rebuild happens by itself
    // when the sample count drops. A change of sample count alone does not move
    // any size or format, so nothing else would have noticed it.
    UINT samples = 1;
    for (int i = 0; i < SLOT_COUNT; ++i)
        if (d[i].Samples > samples) samples = d[i].Samples;
    if (samples > 1)
    {
        if (!g_bridge.msaa_reported)
        {
            g_bridge.msaa_reported = true;
            Warn("this game is rendering with %ux MSAA. The bridge cannot read "
                 "multisampled buffers; turn MSAA off in the game's settings to "
                 "use it. The game is unaffected.", samples);
        }
        g_bridge.frame_ready = false;
        for (int i = 0; i < SLOT_COUNT; ++i) g[i]->Release();
        return;
    }
    // This used to clear msaa_reported, so a game alternating sample counts
    // re-armed the warning every other frame and the two messages traded places
    // for as long as it lasted. Said once each, per session: the frame counter
    // shows the rest.
    if (g_bridge.msaa_reported && !g_bridge.msaa_cleared)
    {
        g_bridge.msaa_cleared = true;
        Log("[bridge] MSAA is off now; rebuilding. Further changes of sample count "
            "are not reported.");
    }

    ID3D11Device *dev11 = nullptr;
    ctx->GetDevice(&dev11);

    // The whole contract comes from the game, never from assumptions about it.
    // Switching between DLAA and an upscaling preset changes the render size
    // while the textures stay put, so both have to be watched.
    unsigned int gw = 0, gh = 0, gow = 0, goh = 0;
    gp->Get("Width", &gw);
    gp->Get("Height", &gh);
    gp->Get("OutWidth", &gow);
    gp->Get("OutHeight", &goh);
    if (gw == 0 || gh == 0) { gw = d[SLOT_COLOR].Width;  gh = d[SLOT_COLOR].Height; }
    if (gow == 0 || goh == 0) { gow = d[SLOT_OUTPUT].Width; goh = d[SLOT_OUTPUT].Height; }

    // The shape this handle was created with, when the create was seen -- see
    // BridgeNoteCreate. The repairs below still run on it: an engine that fills
    // the create in backwards fills the evaluate in backwards too. A create is
    // not always seen -- this add-on can attach after the game's create, and
    // skip_exe holds the hooks back a minute -- and then the evaluate's scalars
    // stand as before.
    // The game's contract only. The substitute's block is this add-on's own and
    // names its shape exactly; g_eval_handle still holds the game's last handle
    // when the substitute takes over, and its create shape -- Quality 2227x928
    // -> 3840x1600 in Baldur's Gate 3 -- was applied to a DLAA 3840x1600 ->
    // 3840x1600 contract: NGX refused three times and the bridge switched itself
    // off for the session. Measured 2026-09-02, D3D11.
    if (InterlockedCompareExchange(&g_source, 0, 0) == SRC_MIRROR)
    {
        unsigned int cw = 0, ch = 0, cow = 0, coh = 0;
        if (BridgeCreateShapeOf(g_eval_handle, &cw, &ch, &cow, &coh))
        {
            static bool said = false;
            if (!said && (cw != gw || ch != gh || cow != gow || coh != goh))
            {
                said = true;
                Log("[bridge] this evaluate declares %ux%u -> %ux%u where the game's create "
                    "stated %ux%u -> %ux%u for this handle. The create is used. Said once; "
                    "the periodic dump shows what is being mirrored.",
                    gw, gh, gow, goh, cw, ch, cow, coh);
            }
            gw = cw; gh = ch; gow = cow; goh = coh;
        }
    }

    // shape=1: the render region the game declares for THIS evaluate, rather
    // than the size the feature was created with. Phantasy Star Online 2 (#8)
    // creates 3840x2160 -> 3840x2160 and then evaluates a 2560x1440 region based
    // at 640,360 inside those textures: the game's own D3D11 feature accepts the
    // block and the mirrored D3D12 one answers 0xBAD00005. The bases are
    // mirrored below as the game states them, so the copies stay whole-texture
    // and the region is placed where the game placed it. Off by default until
    // measured in a game: a title that moves its region every frame would
    // rebuild every frame, and the storm guard below stands the bridge aside.
    if (g_cfg.shape == 1 && InterlockedCompareExchange(&g_source, 0, 0) == SRC_MIRROR)
    {
        unsigned int sw = 0, sh = 0;
        gp->Get("DLSS.Render.Subrect.Dimensions.Width", &sw);
        gp->Get("DLSS.Render.Subrect.Dimensions.Height", &sh);
        if (sw != 0 && sh != 0 && sw <= d[SLOT_COLOR].Width && sh <= d[SLOT_COLOR].Height &&
            (sw != gw || sh != gh))
        {
            static bool said = false;
            if (!said)
            {
                said = true;
                Log("[bridge] shape=1: the feature follows the game's render region, %ux%u, "
                    "rather than the %ux%u it was created with. Said once; the rebuild "
                    "lines show the rest.", sw, sh, gw, gh);
            }
            gw = sw; gh = sh;
        }
    }

    // DLSS never produces an image smaller than the one it is given, so an
    // output size below the render size is not a contract: the two pairs have
    // been written the wrong way round. Four engines do it -- Gallipoli, The
    // Elder Scrolls Online, Phantasy Star Online 2 and House Party's Unity
    // plugin -- and the right repair is to swap them back, not to patch one side
    // from a texture.
    //
    // Patching from the texture was what this code did, and it works only while
    // the allocation happens to equal the display size. Under dynamic resolution
    // it never does: House Party pads every texture to 5120x2048 while the true
    // output is 5120x1440 and the true render is a 2970x836 subrect, so the
    // texture fallback built a feature no evaluate could satisfy and NGX
    // answered InvalidParameter on every frame of gameplay.
    if (gow < gw || goh < gh)
    {
        static bool said_swap = false;
        if (!said_swap)
        {
            said_swap = true;
            Log("[bridge] the game reports an input of %ux%u and an output of %ux%u, "
                "which is the wrong way round: DLSS never returns an image smaller "
                "than the one it is given.", gw, gh, gow, goh);
            Log("  Reading them as output and render respectively, the contract is "
                "%ux%u -> %ux%u.", gow, goh, gw, gh);
        }
        const unsigned int tw = gw, th = gh;
        gw = gow; gh = goh;
        gow = tw; goh = th;
    }

    // What follows is the older repair, kept for a block that is inconsistent in
    // some way the swap above does not resolve. Gallipoli reports 2560x1440 in and 1485x835 out, which is the
    // Balanced render size written into the output fields; NGX answers
    // 0xBAD00005 and the bridge used to rebuild every texture, fail, and repeat
    // until three failures switched it off.
    //
    // The output texture is an allocation rather than a claim, so when the two
    // disagree this way the texture wins.
    if (gow < gw || goh < gh)
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            Log("[bridge] the game asks for an output of %ux%u from an input of %ux%u, "
                "which is smaller rather than larger.", gow, goh, gw, gh);
            Log("  Its output texture is %ux%u, and a texture is an allocation rather "
                "than a claim, so that is used instead.",
                d[SLOT_OUTPUT].Width, d[SLOT_OUTPUT].Height);
        }
        gow = d[SLOT_OUTPUT].Width;
        goh = d[SLOT_OUTPUT].Height;
    }

    // The same repair the other way round, and it has to come second: fixing the
    // input first would make the test above compare two already-corrected values
    // and never fire.
    //
    // A render size cannot exceed the texture holding it -- a subrect has to fit
    // inside its own allocation -- so when it does, the fields have been filled
    // in the wrong order. The Elder Scrolls Online reports Width=2560x1440 with
    // a 1485x835 Color texture and a 2560x1440 Output texture, which is
    // 1485x835 -> 2560x1440 written backwards. Reading it as given built a 1:1
    // feature at output size and did the upscaling work for nothing.
    //
    // Where a game genuinely renders into part of a larger texture the render
    // size is smaller than the texture, not larger, so that case is untouched:
    // Final Fantasy XIV renders 2227x1253 inside a 3840x2160 Color texture and
    // does not come through here.
    if (gw > d[SLOT_COLOR].Width || gh > d[SLOT_COLOR].Height)
    {
        static bool said_in = false;
        if (!said_in)
        {
            said_in = true;
            Log("[bridge] the game asks for a render of %ux%u from a Color texture of "
                "%ux%u, which is larger than the texture holding it.", gw, gh,
                d[SLOT_COLOR].Width, d[SLOT_COLOR].Height);
            Log("  A subrect has to fit inside its own allocation, so the texture is "
                "used instead and the contract reads %ux%u -> %ux%u.",
                d[SLOT_COLOR].Width, d[SLOT_COLOR].Height, gow, goh);
        }
        gw = d[SLOT_COLOR].Width;
        gh = d[SLOT_COLOR].Height;
    }

    // If even the texture cannot satisfy it, nothing here can: say so once and
    // stop, rather than hand NGX parameters it will refuse on every frame.
    if (gow < gw || goh < gh)
    {
        Warn("this game's DLSS output (%ux%u) is smaller than its input (%ux%u). "
             "The bridge cannot work from that and has stopped. The game is unaffected.",
             gow, goh, gw, gh);
        BridgeDisable("the game's output is smaller than its input");
        for (int i = 0; i < SLOT_COUNT; ++i) g[i]->Release();
        dev11->Release();
        return;
    }

    // Gallipoli's map screen creates a 1440x1440 feature while its textures stay
    // 2560x1440: the game is drawing into a square sub-region of them. Handing
    // DLSS the whole texture for a feature that size is the contradiction NGX
    // answers with 0xBAD00005, and where it does not, DLSS writes 1440 columns
    // of a 2560-wide texture and the rest stays whatever it was -- the two
    // halves people see. The subrect origins this bridge copies all read zero,
    // so there is nothing to place the region by. Stand aside and let the game
    // render that screen itself.
    // A result smaller than the texture holding it. The bridge used to stand
    // aside here, because its copy back was a whole-texture copy: the region
    // came back right and the rest of the game's texture came back as whatever
    // the bridge's own output held, which on Gallipoli's map screen was the
    // "two halves" people saw. The copy back is now the region alone, at 0,0,
    // which is exactly what the game's own DLSS writes for the same block -- the
    // subrect origins in every block seen so far read zero, and a game whose
    // region sat elsewhere would have its own DLSS writing to the wrong place
    // too. Delivering is therefore no worse than the game, and the stand-aside
    // was what left House Party (#8, pull request #9) with no neural rendering
    // in any gameplay scene: its allocations are padded for dynamic resolution,
    // 5120x2048 for a 5120x1440 display, so every frame was "partial".
    unsigned int output_x = 0, output_y = 0;
    gp->Get("DLSS.Output.Subrect.Base.X", &output_x);
    gp->Get("DLSS.Output.Subrect.Base.Y", &output_y);
    if (output_x > d[SLOT_OUTPUT].Width || output_y > d[SLOT_OUTPUT].Height ||
        gow > d[SLOT_OUTPUT].Width - output_x || goh > d[SLOT_OUTPUT].Height - output_y)
    {
        BridgeDisable("the output region exceeds its texture; no output is copied back");
        for (int i = 0; i < SLOT_COUNT; ++i) g[i]->Release();
        dev11->Release();
        return;
    }
    const bool partial = (gow != d[SLOT_OUTPUT].Width) || (goh != d[SLOT_OUTPUT].Height);
    g_bridge.partial_output = partial;
    // Only for the game's own contract. The substitute's block is written by
    // this add-on from the back buffer it saw last, so a mismatch there is one
    // stale frame across a resize, not a region: delivering it built a
    // 1920x1080 feature over 3072x1728 textures and NGX refused three times in
    // a row, which is the disable threshold. Measured in ngxGym, synth-modes.
    if (partial && InterlockedCompareExchange(&g_source, 0, 0) == SRC_SYNTH)
    {
        g_bridge.frame_ready = false;
        for (int i = 0; i < SLOT_COUNT; ++i) g[i]->Release();
        dev11->Release();
        return;
    }
    if (partial && !g_bridge.partial_reported)
    {
        g_bridge.partial_reported = true;
        Log("[bridge] the game asks for a %ux%u result inside a %ux%u texture. The "
            "region at %u,%u is written and the rest of the texture left as it is, "
            "which is what the game's own DLSS does with the same block. The game's "
            "own evaluate is not skipped on these frames.",
            gow, goh, d[SLOT_OUTPUT].Width, d[SLOT_OUTPUT].Height, output_x, output_y);
    }

    // Feature dimensions belong to a view, not to the shared allocation. Two
    // views can use different subrect heights in the same atlas (BG3 dialogs).
    // Rebuild for a changed contract of the SAME view, never just for switching
    // between two already valid contracts. New views get their feature below.
    bool shape_changed = false;
    const auto *owner = g_source == SRC_MIRROR ? g_eval_handle : nullptr;
    for (const auto &history : g_bridge.histories)
        if (history.feature != nullptr && history.game == owner)
            shape_changed = history.w != gw || history.h != gh ||
                            history.ow != gow || history.oh != goh;
    // Every slot's own size, not only Color's and Output's. Depth or motion
    // vectors changing size on their own used to move nothing here.
    for (int i = 0; i < SLOT_COUNT && !shape_changed; ++i)
        shape_changed = (d[i].Format != g_bridge.fmt[i]) ||
                        (d[i].Width  != g_bridge.slot_w[i]) ||
                        (d[i].Height != g_bridge.slot_h[i]);

    if (!g_bridge.frame_ready || shape_changed)
    {
        // A game changing resolution rebuilds once. A game alternating between
        // two buffers rebuilds forever, and each rebuild costs four shared
        // texture pairs and an NGX feature. Past a rebuild every few frames the
        // honest reading is that there is no single shape to serve, so the
        // bridge stands aside and lets the game render itself until one settles.
        const ULONGLONG now = GetTickCount64();
        if (now - g_bridge.rebuild_since > 2000)
        { g_bridge.rebuild_since = now; g_bridge.rebuilds = 0; }

        if (++g_bridge.rebuilds > 8)
        {
            // rebuilds is zeroed on every resume, so it can never reach a
            // threshold that spans pauses. This one is not, and a shape that
            // never settles stops costing eight rebuilds every three seconds
            // for the rest of the session.
            if (++g_bridge.pause_cycles >= 5)
            {
                Log("[bridge] the render shape has not settled across %d pauses. "
                    "Continuing would rebuild eight times every three seconds for "
                    "the rest of the session, so the bridge is stopping. The game "
                    "is unaffected.", g_bridge.pause_cycles);
                BridgeDisable("the render shape never settles");
                for (int i = 0; i < SLOT_COUNT; ++i) g[i]->Release();
                dev11->Release();
                return;
            }

            if (!g_bridge.pause_reported)
            {
                Log("[bridge] the render shape has changed %d times in two seconds, so "
                    "there is no one shape to mirror. Standing aside until it settles; "
                    "the game renders on its own meanwhile.", g_bridge.rebuilds);
                for (int i = 0; i < SLOT_COUNT; ++i)
                    Log("[bridge]   slot %d: %ux%u fmt=%u", i, d[i].Width, d[i].Height,
                        static_cast<unsigned>(d[i].Format));
                g_bridge.pause_reported = true;
            }
            g_bridge.paused_until = now + 3000;
            g_bridge.frame_ready  = false;
            for (int i = 0; i < SLOT_COUNT; ++i) g[i]->Release();
            dev11->Release();
            return;
        }

        DXGI_FORMAT fmt[SLOT_COUNT];
        for (int i = 0; i < SLOT_COUNT; ++i) fmt[i] = d[i].Format;
        g_bridge.width      = d[SLOT_COLOR].Width;
        g_bridge.height     = d[SLOT_COLOR].Height;
        g_bridge.out_width  = d[SLOT_OUTPUT].Width;
        g_bridge.out_height = d[SLOT_OUTPUT].Height;
        for (int i = 0; i < SLOT_COUNT; ++i)
        { g_bridge.slot_w[i] = d[i].Width; g_bridge.slot_h[i] = d[i].Height; }
        g_bridge.render_w   = gw;
        g_bridge.render_h   = gh;
        g_bridge.ngx_out_w  = gow;
        g_bridge.ngx_out_h  = goh;

        Log("[bridge] rebuilding: render %ux%u -> %ux%u", gw, gh, gow, goh);
        if (!BridgeBuildResources(dev11, fmt, gp))
        {
            BridgeFail("resource build");
            for (int i = 0; i < SLOT_COUNT; ++i) g[i]->Release();
            dev11->Release();
            return;
        }
    }

    g_bridge.render_w = gw; g_bridge.render_h = gh;
    g_bridge.ngx_out_w = gow; g_bridge.ngx_out_h = goh;

    if (g_cfg.stage >= 3 && !BridgeSelectHistory(gp))
    {
        for (int i = 0; i < SLOT_COUNT; ++i) g[i]->Release();
        dev11->Release();
        return;
    }

    // If the game alternates its Color and Output textures, writing the result
    // into Output could feed back into the next frame's input. Record the
    // identities for the first stretch of frames so that can be ruled in or out.
    {
        static void *seen[SLOT_COUNT] = {};
        static int   swaps = 0;
        bool moved = false;
        for (int i = 0; i < SLOT_COUNT; ++i)
            if (seen[i] != static_cast<void *>(g[i])) moved = true;
        if (moved && swaps < 24)
        {
            ++swaps;
            int reset_now = 0;
            gp->Get("Reset", &reset_now);
            Log("[bridge] frame %llu textures changed: C=%p O=%p D=%p MV=%p reset=%d",
                g_bridge.frames_done + 1, static_cast<void *>(g[SLOT_COLOR]),
                static_cast<void *>(g[SLOT_OUTPUT]), static_cast<void *>(g[SLOT_DEPTH]),
                static_cast<void *>(g[SLOT_MV]), reset_now);
            for (int i = 0; i < SLOT_COUNT; ++i) seen[i] = g[i];
        }
    }

    // 1. game -> shared, on the D3D11 immediate context. MV goes through a
    //    conversion pass when the driver would not share the game's format.
    ctx->CopyResource(g_bridge.tex11[SLOT_COLOR], g[SLOT_COLOR]);
    if (g_bridge.mv_converted) BridgeBlitMV(ctx, dev11, g[SLOT_MV]);

    // MV conversion can disable the bridge and release its mirrors. Stop before
    // exposure handling can recreate mirrors for a disabled bridge.
    if (g_bridge.disabled)
    {
        for (int i = 0; i < SLOT_COUNT; ++i) g[i]->Release();
        dev11->Release();
        return;
    }
    if (!g_bridge.mv_converted) ctx->CopyResource(g_bridge.tex11[SLOT_MV], g[SLOT_MV]);

    // Mirrored per frame like the rest: it is 1x1 in every report so far, and a
    // stale exposure is as wrong as none.
    {
        ID3D11Resource *ge = nullptr;
        if (gp->Get("ExposureTexture", &ge) == NGX_SUCCESS && ge != nullptr)
        {
            ID3D11Texture2D *gt = nullptr;
            if (SUCCEEDED(ge->QueryInterface(__uuidof(ID3D11Texture2D),
                                             reinterpret_cast<void **>(&gt))) && gt != nullptr)
            {
                if (BridgeEnsureExposure(dev11, gt))
                    ctx->CopyResource(g_bridge.exp11, gt);
                gt->Release();
            }
        }
        else if (g_bridge.exp12 != nullptr)
        {
            // The game stopped supplying one. Sending the last frame's exposure
            // for the rest of the session would be worse than sending none.
            Log("[bridge] the game no longer supplies an exposure texture; the mirror "
                "is dropped rather than left holding a stale one.");
            BridgeReleaseExposure();
        }
    }

    if (g_cfg.stage >= 2 && g_bridge.depth_converted)
        BridgeBlitDepth(ctx, dev11, g[SLOT_DEPTH]);

    if (g_cfg.stage < 3)
    {
        ++g_bridge.frames_done;
        for (int i = 0; i < SLOT_COUNT; ++i) g[i]->Release();
        dev11->Release();
        return;
    }

    // 2. publish those copies to the other queue. The flush matters: without it
    //    the signal may sit unsubmitted and the D3D12 wait never completes.
    const UINT64 v_in = ++g_bridge.fence_value;
    g_bridge.ctx4->Signal(g_bridge.fence11, v_in);
    ctx->Flush();

    // 3. D3D12 side: wait, evaluate, signal back
    g_bridge.queue->Wait(g_bridge.fence12, v_in);
    if (!BeginCommands())
    {
        // A stall is not a failure: the frame is skipped and the game's own DLSS
        // takes it, which is what BridgeWillDeliver now answers.
        if (!g_bridge.stalled) BridgeFail("command list");
        for (int i = 0; i < SLOT_COUNT; ++i) g[i]->Release();
        dev11->Release();
        return;
    }

    Barrier(g_bridge.list, g_bridge.tex12[SLOT_COLOR], D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(g_bridge.list, g_bridge.tex12[SLOT_DEPTH], D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(g_bridge.list, g_bridge.tex12[SLOT_MV], D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(g_bridge.list, g_bridge.tex12[SLOT_OUTPUT], D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (g_bridge.pq.in != nullptr && !PqPassBefore(&g_bridge.pq, g_bridge.queue))
    {
        BridgeFail("HDR10 decode");
        for (int i = 0; i < SLOT_COUNT; ++i) g[i]->Release();
        dev11->Release();
        return;
    }
    g_bridge.params->Set("Color", g_bridge.pq.in  != nullptr ? g_bridge.pq.in  : g_bridge.tex12[SLOT_COLOR]);
    g_bridge.params->Set("Output", g_bridge.pq.out != nullptr ? g_bridge.pq.out : g_bridge.tex12[SLOT_OUTPUT]);
    g_bridge.params->Set("Depth", g_bridge.tex12[SLOT_DEPTH]);
    g_bridge.params->Set("MotionVectors", g_bridge.tex12[SLOT_MV]);
    if (g_bridge.exp12 != nullptr)
        g_bridge.params->Set("ExposureTexture", g_bridge.exp12);
    // The parameter block is shared too; consumers must see this view's
    // contract rather than whichever view was created last.
    g_bridge.params->Set("Width", g_bridge.render_w);
    g_bridge.params->Set("Height", g_bridge.render_h);
    g_bridge.params->Set("OutWidth", g_bridge.ngx_out_w);
    g_bridge.params->Set("OutHeight", g_bridge.ngx_out_h);
    // Mirror the game's own subrect description rather than inventing one. In an
    // upscaling preset the rendered area is a corner of a larger texture, and
    // guessing it is what made DLSS Quality produce a soft, effect-free image.
    // These two are not the game's to declare on our behalf: they state the size
    // of the region THIS feature is being asked to process, and this bridge knows
    // that exactly because it created the feature. Mirroring is right for MV.Scale
    // and exposure, which describe the game's own data and are corrupted by any
    // invention. It is wrong here, and 1.0.18 stopped sending them to a game that
    // sets none -- taking information away from whatever reads the block after us.
    CopyUIntParam(gp, "DLSS.Render.Subrect.Dimensions.Width", g_bridge.render_w);
    CopyUIntParam(gp, "DLSS.Render.Subrect.Dimensions.Height", g_bridge.render_h);
    NeutralUIntParam(gp, "DLSS.Input.Color.Subrect.Base.X", 0u);
    NeutralUIntParam(gp, "DLSS.Input.Color.Subrect.Base.Y", 0u);
    NeutralUIntParam(gp, "DLSS.Input.Depth.Subrect.Base.X", 0u);
    NeutralUIntParam(gp, "DLSS.Input.Depth.Subrect.Base.Y", 0u);
    NeutralUIntParam(gp, "DLSS.Input.MV.Subrect.Base.X", 0u);
    NeutralUIntParam(gp, "DLSS.Input.MV.Subrect.Base.Y", 0u);
    NeutralUIntParam(gp, "DLSS.Output.Subrect.Base.X", 0u);
    NeutralUIntParam(gp, "DLSS.Output.Subrect.Base.Y", 0u);
    NeutralFloatParam(gp, "MV.Scale.X", 1.0f);
    NeutralFloatParam(gp, "MV.Scale.Y", 1.0f);
    MirrorFloatParam(gp, "Jitter.Offset.X");
    MirrorFloatParam(gp, "Jitter.Offset.Y");
    MirrorFloatParam(gp, "Sharpness");
    NeutralFloatParam(gp, "DLSS.Pre.Exposure", 1.0f);
    NeutralFloatParam(gp, "DLSS.Exposure.Scale", 1.0f);

    int reset = 0;
    gp->Get("Reset", &reset);
    g_bridge.params->Set("Reset", (g_bridge.need_reset || reset || g_cfg.reset_every) ? 1 : 0);
    g_bridge.need_reset = false;

    const int mode = g_cfg.mode;

    DWORD code = 0;
    Breadcrumb("running the D3D12 evaluate");
    NVSDK_NGX_Result re = NGX_SUCCESS;
    static const char kNgxMark[] = "ngx evaluate";
    g_bridge.list->BeginEvent(1, kNgxMark, sizeof(kNgxMark));
    if (mode != 1)
        re = SafeEvaluateGuarded(g_bridge.eval_feature, UnwrapList(g_bridge.list),
                                 g_bridge.feature, g_bridge.params, &code);
    g_bridge.list->EndEvent();

    Barrier(g_bridge.list, g_bridge.tex12[SLOT_COLOR],
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(g_bridge.list, g_bridge.tex12[SLOT_DEPTH],
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(g_bridge.list, g_bridge.tex12[SLOT_MV],
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(g_bridge.list, g_bridge.tex12[SLOT_OUTPUT],
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

    UINT64 v_out = EndCommands();
    if (g_bridge.pq.in != nullptr)
        v_out = PqPassAfter(&g_bridge.pq, g_bridge.queue, g_bridge.fence12, &g_bridge.fence_value, v_out);

    // No CPU wait here, and that is deliberate rather than an omission: the
    // signal above is a GPU-side fence and the game's queue waits on it, so this
    // transport never blocks the calling thread.
    //
    // Tested as an accommodation on 2026-09-02 and it changes nothing. A DLSS 5
    // add-on build whose own log announces a "GPU-safe NR workset pool ... exact
    // queue fences recycle only completed submissions" exhausts that pool after
    // its first evaluation here and writes nothing thereafter. Draining this
    // queue on the CPU after every evaluate -- a full pipeline bubble per frame
    // -- left the output byte-identical and the pool still exhausted. So the
    // recycling is not observing this queue, and there is nothing on this side
    // to give it. Measured in ngxGym: same hash, same one evaluation, same
    // exhaustion, drain on and off.

    // One readback, one frame, once per session, and only when asked. See the
    // note on hash_out: this is the only measurement here that can tell a
    // neighbour add-on which changed the picture from one which said it did.
    //
    // The 60th delivered frame rather than the first: DLSS has temporal history
    // and a neighbour may attach a frame or two in, so the first frame is the one
    // most likely to be identical whatever happens afterwards.
    if (g_cfg.hash_out != 0 && !g_bridge.hashed && g_bridge.frames_done >= g_bridge.hash_after &&
        code == 0 && re == NGX_SUCCESS)
    {
        g_bridge.hashed = true;
        BridgeReadback(v_out);
    }

    if (code != 0)
    {
        // An access violation inside NGX means its internal state is no longer
        // trustworthy. Retrying would likely take the game down, so stop here
        // and let it render on its own.
        Log("[bridge] evaluate raised exception 0x%08X -- disabling to protect the game", code);
        {
            // The module and offset, as the create path has printed since 1.3.0.
            // An evaluate fault swallowed here never reached the crash filter,
            // so a report carried the code and nothing else (#22).
            wchar_t who[MAX_PATH + 64] = {};
            if (NameFaultOwner(who, _countof(who))) Log("[bridge]   it faulted in %ls", who);
            LogFaultStack("[bridge]");
        }
        BridgeDisable("NGX raised an exception inside the D3D12 evaluate");
    }
    else if (re != NGX_SUCCESS)
    {
        Log("[bridge] evaluate failed with result 0x%08X, %s", re, NgxResultName(re));
        DumpSentParams();
        BridgeFail("evaluate");
    }
    else
    {
        // Only a completed evaluate clears the failure count.
        g_bridge.consecutive_fails = 0;

        // 4. take the result back into the game's own output texture.
        //
        // The wait stays on the GPU: moving it to the CPU costs about nine
        // milliseconds a frame, because the CPU then stops instead of running
        // ahead to prepare the next one. What it cannot stay is unattended --
        // a GPU-side wait has no timeout and cannot be cancelled, so if the
        // D3D12 side never reaches v_out the game's queue stops for good.
        // BridgeRescueStalledQueue watches this value on later frames.
        g_bridge.ctx4->Wait(g_bridge.fence11, v_out);
        // Always the newest value: fences are monotonic, so signalling the
        // latest satisfies every wait queued behind it. The deadline is not
        // armed here -- BridgeRescueStalledQueue keeps it, against the last time
        // the GPU completed anything at all.
        g_bridge.pending_out = v_out;

        const UINT64 n = ++g_bridge.frames_done;
        // The one place a completed evaluate exists to be copied back, so the
        // only honest place to count a delivered frame. The stage<3 returns
        // above move frames_done as well, and they deliver nothing.
        CountDelivered();

        // Did a DLSS 5 add-on detour CreateFeature after this add-on had already
        // called it? Only asked on the synthetic path, and only for the first
        // few frames: in a mirror session the game creates features of its own
        // and that add-on catches one of those, but here nothing else will ever
        // call CreateFeature again, so the one create it missed was the only one
        // there was going to be. Comparing the bytes is the whole test -- a
        // detour is written over them -- so this states what it observed rather
        // than waiting a length of time somebody guessed.
        if (g_source == SRC_SYNTH && g_bridge.create_prologue_taken &&
            !g_bridge.recreated_for_addon && n >= 2 && n <= 240 &&
            g_bridge.create_feature != nullptr &&
            memcmp(g_bridge.create_prologue,
                   reinterpret_cast<const void *>(g_bridge.create_feature),
                   sizeof(g_bridge.create_prologue)) != 0)
        {
            g_bridge.recreated_for_addon = true;
            Log("[bridge] NVSDK_NGX_D3D12_CreateFeature has been detoured since this add-on "
                "created its feature, which is a DLSS 5 add-on arming after the fact. It "
                "captures its contract from a create it sees, and it did not see this one. "
                "Rebuilding once so that it does.");
            g_bridge.frame_ready = false;
        }

        if (mode == 2)
        {
            if (g_bridge.partial_output)
            {
                // The region only. See the note above the partial test.
                // NGX wrote at the game's output base, not necessarily 0,0.
                // Copying the origin can overwrite the other split-screen view.
                D3D11_BOX box = { output_x, output_y, 0,
                                  output_x + g_bridge.ngx_out_w,
                                  output_y + g_bridge.ngx_out_h, 1 };
                ctx->CopySubresourceRegion(g[SLOT_OUTPUT], 0, output_x, output_y, 0, g_bridge.tex11[SLOT_OUTPUT], 0, &box);
            }
            else
                ctx->CopyResource(g[SLOT_OUTPUT], g_bridge.tex11[SLOT_OUTPUT]);
        }
        else if (mode == 1) ctx->CopyResource(g[SLOT_OUTPUT], g_bridge.tex11[SLOT_COLOR]);
        // mode 0 writes nothing back

        g_bridge.consecutive_fails = 0;

        if (g_cfg.pixels && n >= 2 && n <= 4)
        {
            Log("[bridge] frame %llu buffer contents:", n);
            LogCenterPixel(ctx, dev11, "game in",  g[SLOT_COLOR],               d[SLOT_COLOR].Format);
            LogCenterPixel(ctx, dev11, "shared in", g_bridge.tex11[SLOT_COLOR], d[SLOT_COLOR].Format);
            LogCenterPixel(ctx, dev11, "depth",     g_bridge.tex11[SLOT_DEPTH], DXGI_FORMAT_R32_FLOAT);
            LogCenterPixel(ctx, dev11, "dlss out",  g_bridge.tex11[SLOT_OUTPUT], d[SLOT_OUTPUT].Format);
        }
        else if (n <= 1 || (n % 1800) == 0)
        {
            Log("[bridge] frame %llu delivered (%ux%u)", n, g_bridge.width, g_bridge.height);
        }
    }

    for (int i = 0; i < SLOT_COUNT; ++i) g[i]->Release();
    dev11->Release();
}

// Wrapper so every exit path from the frame work is timed, including the early
// ones taken when a stage is disabled or a rebuild fails.
static void BridgeFrame(ID3D11DeviceContext *ctx, const NVSDK_NGX_Parameter *gp)
{
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    // One lock around the whole frame's worth of context work, so no other
    // thread can land between the copies, the fence signal and the copy back.
    const bool lock = g_bridge.mt != nullptr && g_bridge.mt->GetMultithreadProtected();
    if (lock) g_bridge.mt->Enter();
    BridgeFrameInner(ctx, gp);
    if (lock) g_bridge.mt->Leave();

    QueryPerformanceCounter(&t1);
    TimingTick(t0.QuadPart, t1.QuadPart);
}
