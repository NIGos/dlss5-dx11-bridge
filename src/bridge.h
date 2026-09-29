// D3D11 -> D3D12 NGX bridge.
//
// The game drives DLSS through NVSDK_NGX_D3D11_EvaluateFeature_C. That call is
// intercepted and forwarded untouched, then this bridge reproduces the same
// contract on a second NGX session running on its own D3D12 device. RenoDX's
// DLSS 5 add-on detours the D3D12 entry points, so the D3D12 evaluate is where
// its neural-rendering pass is inserted.
//
// Per frame:
//   1. copy the game's Color / Depth / MotionVectors into shared textures
//   2. signal a shared fence on the D3D11 immediate context
//   3. wait on it from the D3D12 queue, run the D3D12 evaluate, signal back
//   4. wait on the D3D11 context, copy the result into the game's Output
//
// The textures are created on the D3D12 side with D3D12_HEAP_FLAG_SHARED and
// opened as D3D11 aliases, because the game's own textures carry MiscFlags = 0
// and cannot be shared directly.

#pragma once

enum { SLOT_COLOR = 0, SLOT_OUTPUT, SLOT_DEPTH, SLOT_MV, SLOT_COUNT };

static const char *kSlotKey[SLOT_COUNT]  = { "Color", "Output", "Depth", "MotionVectors" };
static const char *kSlotName[SLOT_COUNT] = { "Color", "Output", "Depth", "MV" };

// The four facts a texture description has to yield for the dimension repairs
// in BridgeFrameInner to run: width, height, format and sample count. Nothing
// in that block reads anything else out of a description, and D3D11, D3D12 and
// Vulkan spell those four in three unrelated structs -- so the repairs that
// Gallipoli, The Elder Scrolls Online, Phantasy Star Online 2 and House Party
// each forced stay one block of code rather than one copy per API.
//
// The field names deliberately match D3D11's. Every one of those repairs was
// verified by inspection against a user log and none of those titles is
// available to test, so each read site keeps the exact expression it was
// verified with; only the sample count -- SampleDesc.Count there, Samples here
// -- reads differently at all.
struct TexFacts
{
    UINT        Width;
    UINT        Height;
    DXGI_FORMAT Format;
    UINT        Samples;
};

// Which contract the D3D12 side is mirroring. Every decision that used to read
// "the bridge is running" has to say whose frames it is running on the moment
// there is more than one answer, so the answer is stored rather than inferred.
// Declared here because bridge.inc is included well before the counters beside
// g_eval_count and reads this in NoteAbsentParam and BridgeWillDeliver.
enum Source { SRC_NONE, SRC_MIRROR, SRC_SYNTH };

static volatile LONG g_source;

// Frames this add-on has actually put into the game's Output, counted whichever
// source produced them. g_bridge.frames_done cannot answer that question for
// anyone outside the frame path: it is the mirror's own state, and a second
// source would keep its own. ReportIdle needs one liveness signal that does not
// have to be taught about each new source.
static volatile LONG g_frames_delivered;
// Every source counts here -- the D3D11 bridge, the Vulkan mirror and the
// substitute contract on either transport. Logged every 600 frames, so a log
// shows whether delivery went on after the last runtime teardown; the mirror's
// own counter cannot say that, and the substitute had no counter at all.
static void CountDelivered()
{
    const LONG n = InterlockedIncrement(&g_frames_delivered);
    if (n % 600 == 0) Log("[bridge] %ld frames delivered so far.", n);
}

// The mirror runs on the game's render thread; a synthetic source runs on the
// thread ReShade renders its effects from. Both reach the one g_bridge -- its
// textures, its fence values, its command list -- and the lock BridgeFrame
// already takes is the game's own ID3D11Multithread, held only when the game
// switched multithread protection on. That is a lock about the game's device,
// not about this add-on's state, so it cannot be the one that separates two
// sources.
// ponytail: one global section around the whole frame, not per-object locks.
// It is uncontended in every session that has one source, which the arming
// latch makes the only kind there is; splitting it earns nothing until two
// sources are ever live at once, which is the thing the latch forbids.
static CRITICAL_SECTION g_bridge_cs;

// The HDR10 pair of passes, one per D3D12 session that can carry a synthetic
// contract. On an HDR10 presentation the back buffer holds PQ code values in
// R10G10B10A2; the decode pass turns the colour copy into linear BT.709 float
// (1.0 = 80 nits, the scRGB convention), NGX reads that and writes a linear
// result, and the encode pass turns the result back into PQ in the output
// copy. Defined in bridge.inc; see PqBackBuffer there for when it is built.
struct PqPass
{
    ID3D12Device         *dev;
    ID3D12Resource       *in;     // linear colour NGX reads
    ID3D12Resource       *out;    // linear result NGX writes
    ID3D12RootSignature  *rs;
    ID3D12PipelineState  *pso;
    ID3D12DescriptorHeap *heap;
    bool                  srgb;
    UINT                  cw, ch, ow, oh;
    // Not owned: the session's colour and output textures the passes convert.
    // output_b is the substitute's second Output, and it is null on every other
    // caller and in every mode but vk_sync=2 -- see kOutB in synth.inc. The
    // encode has to know which of the two the evaluate wrote into, because the
    // pipeline alternates them; the decode never does, because colour is single.
    ID3D12Resource       *colour, *output, *output_b;
    // Command lists of their own, one per direction, executed on the session's
    // queue before and after the list NGX records into. Nothing of this
    // add-on's is ever recorded into that list: the DLSS 5 add-on records the
    // host state it sees there -- descriptor heaps included, NGX's own among
    // them -- and sets it again when it intercepts a create, and a heap the
    // feature release freed came back as a dangling pointer inside ReShade
    // (measured 2026-09-03). Reuse is fenced.
    ID3D12CommandAllocator    *alloc[2];
    ID3D12GraphicsCommandList *list[2];
    ID3D12Fence               *fence;
    HANDLE                     ev;
    UINT64                     fv, done[2];
};

struct Bridge
{
    bool disabled;          // set after a hard failure; never retried
    bool session_ready;     // device, queue, fences, NGX session
    bool frame_ready;       // shared textures and NGX feature match the game
    bool hashed;            // the output readback for the current feature has been taken
    UINT64 hash_after;      // frames_done at which it is taken: 60 past the feature's build
    bool msaa_reported;     // the MSAA notice is said once per spell of it

    // The game's queue is made to wait for this fence value. A GPU-side wait
    // cannot be cancelled, so the value it waits on is remembered here and
    // checked on later frames; the fence can be signalled from the CPU, which
    // releases the game even when the work behind it never arrives.
    // Each slot's own dimensions, so a mirror is never sized from another
    // slot's texture.
    // Rebuild thrash. Gallipoli's respawn screen alternates its Color texture
    // between two buffers of different formats, one per frame, so every frame
    // looked like a new shape and rebuilt four shared texture pairs and an NGX
    // feature -- about forty milliseconds of work per frame, and a two-megabyte
    // log. Counting rebuilds over a window is what tells a real resolution
    // change apart from a game alternating between two of them.
    ULONGLONG rebuild_since;
    int       rebuilds;
    ULONGLONG paused_until;
    bool      pause_reported;
    bool      resume_reported;

    // Counted across pauses and never reset, unlike rebuilds. A shape that
    // settles produces one pause; a shape that never settles produces one every
    // three seconds for the whole session, each preceded by eight rebuilds of
    // four shared texture pairs and an NGX feature. The per-window counter
    // cannot see that, by construction.
    int       pause_cycles;
    bool      msaa_cleared;

    // The feature covers only part of the output texture. Gallipoli's map screen
    // creates a 1440x1440 feature while its textures stay 2560x1440: it draws
    // into a square sub-region. Handing DLSS the whole texture for a feature
    // that size is the contradiction NGX answers with 0xBAD00005, and where it
    // does not, DLSS writes 1440 columns of a 2560-wide texture and the rest
    // stays whatever it was -- the two halves people see.
    bool      partial_output;
    bool      partial_reported;
    bool      presets_reported;   // the render preset list is said once per session

    UINT      slot_w[4];
    UINT      slot_h[4];

    // Frame-to-frame pacing. The average alone hides the spread, and a driver
    // that interpolates between presented frames -- NVIDIA's Smooth Motion --
    // cares about the spread rather than the average.
    LONGLONG  last_entry;
    LONGLONG  iv_min;
    LONGLONG  iv_max;

    UINT64    last_completed;   // to tell a stalled GPU from a busy one
    UINT64    pending_out;
    ULONGLONG pending_since;
    // A stall survived rather than fatal; see BridgeRescueStalledQueue. The
    // progress fence is signalled by the queue only, never from the CPU, so it
    // still says what the GPU has really done after fence12 has been pushed
    // forward by hand to release the game.
    ID3D12Fence *fence_progress;
    bool         stalled;
    ULONGLONG    stall_since;
    UINT64       stall_release;
    // The newest value the queue has actually been asked to signal. fence_value
    // runs ahead of it: a frame takes v_in before BeginCommands, and a frame
    // that stalls there never submits, so its value is never reached.
    UINT64       last_submitted;
    int          stalls;
    ULONGLONG    stall_last;   // when the last one began; the count is per minute
    // stall_test in dlss5-bridge.cfg: one wait the queue is given at the 60th
    // submission and released from the CPU that many milliseconds later. It is
    // how the survival above is measured; nothing else uses it.
    ID3D12Fence *stall_fence;
    UINT64       stall_target;
    ULONGLONG    stall_release_at;
    bool         stall_test_done;
    int  consecutive_fails;

    ID3D12Device              *dev12;
    IDXGIAdapter3             *adapter3;
    ID3D12CommandQueue        *queue;
    ID3D12GraphicsCommandList *list;

    // A command allocator must not be reset while the GPU is still executing the
    // commands recorded in it -- doing so recycles that memory underneath the
    // GPU and it goes on to execute garbage. One allocator per frame in flight,
    // each remembering the fence value that retires it, so none is ever reused
    // early. The command list itself is safe to reset as soon as it is submitted.
    static const int           kFrames = 3;
    ID3D12CommandAllocator    *alloc[kFrames];
    UINT64                     alloc_fence[kFrames];
    int                        frame_slot;
    HANDLE                     fence_event;
    ID3D12Fence               *fence12;
    ID3D11Fence               *fence11;
    ID3D11DeviceContext4      *ctx4;

    // Engines that drive D3D11 from more than one thread turn on multithread
    // protection. The runtime then serialises individual calls, but a sequence
    // of them still needs holding as a unit or another thread can interleave
    // into the middle of the copy / signal / copy-back run.
    ID3D11Multithread         *mt;
    UINT64                     fence_value;

    PFN_D3D12CreateFeature   create_feature;

    // The first bytes of NVSDK_NGX_D3D12_CreateFeature as they were when this
    // add-on took its own copy of the pointer. A DLSS 5 add-on detours that
    // export, so a change here is that add-on arriving -- derived, not timed.
    unsigned char            create_prologue[16];
    bool                     create_prologue_taken;
    bool                     recreated_for_addon;
    PFN_D3D12EvaluateFeature eval_feature;
    PFN_D3D12ReleaseFeature  release_feature;
    PFN_AllocateParameters   alloc_params;

    NVSDK_NGX_Parameter *params;
    NVSDK_NGX_Handle    *feature;
    // One temporal history per game feature, sharing the ordered transport.
    // Invalidated together when the resource shape or create contract changes.
    struct History {
        const NVSDK_NGX_Handle *game; NVSDK_NGX_Handle *feature;
        unsigned int w, h, ow, oh;
    };
    History histories[8];

    // The exposure texture is kept apart from the slot array rather than made a
    // fifth slot: every SLOT_COUNT loop dereferences its entry unconditionally,
    // and the games that supply no exposure texture -- ESO, Final Fantasy XIV --
    // would take a null through all of them.
    ID3D12Resource  *exp12;
    ID3D11Texture2D *exp11;
    HANDLE           exp_shared;
    unsigned int     exp_w, exp_h;
    DXGI_FORMAT      exp_fmt;
    bool             exp_reported;

    ID3D12Resource  *tex12[SLOT_COUNT];
    // Built for a synthetic contract on an HDR10 presentation, else in is null.
    PqPass           pq;
    ID3D11Texture2D *tex11[SLOT_COUNT];
    HANDLE           shared[SLOT_COUNT];

    // Identity of the game textures this set was built for. BG3 recreates its
    // render targets mid-session, and Color/Output also swap every frame, so
    // the descriptor rather than the pointer is what has to match.
    // Texture sizes, which in an upscaling mode differ between input and output.
    UINT        width, height;          // Color/Depth/MV texture size
    UINT        out_width, out_height;  // Output texture size
    // What the game told NGX about the actual rendered area, which is smaller
    // than the texture whenever DLSS is upscaling rather than running as DLAA.
    UINT        render_w, render_h;
    UINT        ngx_out_w, ngx_out_h;
    DXGI_FORMAT fmt[SLOT_COUNT];

    bool   need_reset;      // NGX Reset flag for the first frame of a new set
    UINT64 frames_done;

    // Timing. The point is to separate two very different costs: work this code
    // does on the CPU, versus the GPU pipeline bubbles the cross-API fences
    // create. A small CPU figure next to a long frame interval means the cost is
    // the synchronisation, not anything the bridge computes.
    LONGLONG qpf;
    LONGLONG cpu_ticks;
    LONGLONG span_start;
    UINT64   timed_frames;

    // The synthetic path's optical flow, kept apart from cpu_ticks because it is
    // not the bridge's own cost and because it is invisible to the line above:
    // TimingTick is only reached through BridgeFrame, and the whole flow block
    // runs before BridgeFrame is called. nvOFExecute blocks the calling thread,
    // which here is ReShade's present thread, so this is CPU time inside the
    // game's frame rather than merely GPU time -- and it is the budget item.
    // Zero in every mirror session, which is what keeps the report line
    // byte-identical there.
    LONGLONG ofa_ticks;
    UINT64   ofa_frames;

    // The game's depth is R24G8_TYPELESS and D3D11 will not create a shared
    // texture in that format, so the shared copy is R32_FLOAT and a compute pass
    // converts into it. CopyResource cannot: the two formats are in different
    // typeless families.
    bool                       depth_converted;
    ID3D11ComputeShader       *depth_cs;
    ID3D11UnorderedAccessView *depth_uav;   // on the shared R32_FLOAT texture
    ID3D11UnorderedAccessView *mv_uav;      // the MV conversion pass writes the shared copy through it
    ID3D11ShaderResourceView  *depth_srv;   // on the game's depth
    ID3D11Resource            *depth_src;   // held so its pointer cannot be recycled

    // Some games hand NGX motion vectors in a format the driver will not share
    // -- R32G32_FLOAT, for one, is absent from the kernel's shared-format list,
    // and CreateTexture2D then rejects it outright. The shared MV copy is
    // R16G16_FLOAT, the DLSS-recommended motion-vector format, and a compute
    // pass converts into it, the same way depth is converted.
    bool                       mv_converted;
    ID3D11ComputeShader       *mv_cs;
    ID3D11ShaderResourceView  *mv_srv;      // on the game's MV
    ID3D11Resource            *mv_src;      // held so its pointer cannot be recycled

    // Last resort if even that fails: a zero-filled D3D12 texture completes the
    // NGX contract but leaves DLSS blind to disocclusion, which shows up as the
    // previous scene smearing into the new one.
    bool depth_placeholder;
};

static Bridge g_bridge;
