// ---------------------------------------------------------------------------
// synth.inc -- a DLSS contract built from ReShade's own buffers
//
// Two halves, and the first still runs on every session. The measuring half
// answers one question in the log: if this add-on had to build a DLSS contract
// out of ReShade's depth buffer and a motion-vector shader, would the pieces be
// there. It reads, it measures, and it prints, and it touches no NGX parameter.
//
// The building half, below the report, does the thing the report was measuring
// for -- but only when the configured delay has passed and the game has still
// never called DLSS itself. It ships ON, because source=auto is the default and
// falling back is what auto means. source=mirror is how a user turns it off.
//
// Kept out of bridge.inc on purpose. bridge.inc is the file that runs in nine
// shipped titles and nobody should have to edit it for a reason that belongs to
// synthesis; the note above NoteAbsentParam is the whole argument.
//
// The ReShade headers under reshade/ are vendored verbatim rather than
// hand-declared. Reaching find_texture_variable means indexing virtual slot 37
// of effect_runtime, and the only two ways to get 37 are to count 37 pure
// virtuals by hand or to let the compiler count them. A hand-written table that
// drifts produces a jump into whatever ReShade put at that slot -- no compile
// error, no crash near the mistake. This project has already lost releases to
// code landing in the wrong function; it does not need a table of forty-five
// dummy declarations that nothing verifies. Their licence is BSD-3-Clause OR
// MIT, so the MIT arm applies here; reshade/LICENSE.md carries the notice.

// ---------------------------------------------------------------------------

#include "reshade/reshade_events.hpp"

// Set by RegisterWithReShade the moment it succeeds. The module handle is needed
// because ReShadeRegisterEvent is resolved from the same DLL the registration
// came from, and the version is needed because it decides whether any of this is
// allowed to touch an effect_runtime at all.
static HMODULE  g_reshade_dll;
static uint32_t g_reshade_api;

// find_texture_variable is virtual slot 37 of effect_runtime, and
// get_texture_binding slot 45, in every ReShade from 6.0.0 (API 10) through
// 6.8.0 (API 20) -- which is the layout the vendored headers describe. ReShade
// 5.9.0 (API 8) puts them at 33 and 41: effect_runtime derived from swapchain
// rather than device_object then, so the whole table sits four slots earlier.
//
// RegisterWithReShade counts its requested version down to 5 so the D3D11 mirror
// keeps working on those older builds, which deliberately defeats the ABI guard
// ReShade would otherwise apply. That is correct for the mirror, which never
// touches an effect_runtime, and fatal here. So the floor is checked separately
// and stated with both numbers, rather than assumed from the fact that we
// registered at all.
static const uint32_t kSynthApiFloor = 10;

// Any texture annotated ": DEPTH" binds to the same entry in ReShade's semantic
// map, so the NAME is arbitrary and a list of names can never be complete. There
// is no find-by-semantic in the add-on API, so a list is what there is. Two so
// far, both observed: ReShade.fxh declares "texture DepthBufferTex : DEPTH;" in
// namespace ReShade; vort_Shaders declares "texture2D DepthTex : DEPTH;" in
// namespace Depth (vort_Depth.fxh:5), and with only vort loaded in Baldur's
// Gate 3 this add-on reported "NOT PRESENT" for a depth buffer that was bound.
// find_texture_variable matches the plain declared name as well as the mangled
// one, so the unqualified name is what to ask for.
static const char *kSynthDepthNames[] = { "DepthBufferTex", "DepthTex" };

// The providers do not agree on a name, and they do not agree on a direction
// either. Order is preference: the first one that is bound wins.
//
// sign_x/sign_y are the provider's own convention where it can be read out of
// source we are allowed to read, and 0 where it cannot. vort_Shaders is MIT, so
// its convention is a fact rather than a guess: CalcMV samples the previous
// frame at "i.uv + total_motion" (vort_MotionVectors.fxh:232), which makes the
// stored vector uv_prev - uv_cur in ReShade's +y-down UV -- the direction DLSS
// wants, unmodified, hence +1,+1. GetCameraVelocity agrees from the other side,
// computing curr_screen - prev_screen then scaling by (-0.5, +0.5)
// (vort_MotionUtils.fxh:81-92). The other two are all-rights-reserved or
// non-commercial, so their direction is not readable here and stays 0, which
// means "fall back to the config knob".
//
// vort is first because it is the only one of the three that is MIT, compiles on
// current ReShade (verified: 0.59 s in Baldur's Gate 3, 2026-08-30) and states
// its direction in source. Note it writes MotVectTexVort only at V_MV_MODE 1,
// its default and the only mode where it COMPUTES vectors; at modes 2 and 3 it
// consumes someone else's, which is why the other two names still matter.
struct SynthMvProvider
{
    const char *name;
    int         sign_x;
    int         sign_y;
    const char *label;
};

// Row 0 has no ReShade texture. NVOFA is the driver's own optical flow engine
// and this add-on creates the texture it writes into, so there is no name to
// hand find_texture_variable and the two loops that walk these names start at 1.
// Everything else about a provider -- the label the report prints, the direction
// MV.Scale takes its sign from, the index the panel reads -- is the same
// question for this row as for the other three, so it is the same table.
//
// Its direction is not documented, it is measured: 1,166,400 vectors on this
// machine, 100% within 0.5 px of ground truth, with inputFrame = the current
// frame and referenceFrame = the previous one. That is the DLSS convention on
// both axes with no Y flip.
static const SynthMvProvider kSynthMv[] = {
    { "NVIDIA optical flow", 1, 1, "the driver's own optical flow engine (measured)" },
    { "MotVectTexVort",      1, 1, "vort_Shaders vort_Motion.fx (MIT)" },
    { "texMotionVectors",    0, 0, "a texMotionVectors provider (DRME, qUINT_of)" },
    { "MotionVectorsTex",    0, 0, "iMMERSE Launchpad" },
};

// Where the ReShade texture names start. kSynthMv[0] is not one.
static const int kSynthMvReShadeFirst = 1;

// The four ReShade.fxh definitions that say how to read the depth values. They
// are #ifndef defaults in the shader header, not engine state, so a user who has
// not overridden one leaves it absent -- and absent means the default, which for
// RESHADE_DEPTH_INPUT_IS_REVERSED is 1 rather than 0. Reading a miss as "off"
// would invert the one that matters.
struct SynthDepthDef
{
    const char *name;
    int         fxh_default;
};

static const SynthDepthDef kSynthDepthDefs[] = {
    { "RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN",        0 },
    { "RESHADE_DEPTH_INPUT_IS_REVERSED",           1 },
    { "RESHADE_DEPTH_INPUT_IS_LOGARITHMIC",        0 },
    { "RESHADE_DEPTH_LINEARIZATION_FAR_PLANE", 1000 },
};

enum { SYNTH_DEF_UPSIDE_DOWN, SYNTH_DEF_REVERSED, SYNTH_DEF_LOGARITHMIC, SYNTH_DEF_FAR_PLANE };

// One frame's worth of answers. Everything the report prints is either in here
// or derived from it, so the report cannot state something that was not measured.
struct SynthFacts
{
    uint32_t api;
    // reshade::api::device_api, exactly as the runtime's own device answered it.
    // Stored rather than reduced to a yes/no, because there are two transports
    // now and the report has to name the one this session actually got.
    uint32_t device_api;
    uint32_t back_w, back_h;

    int      depth_found;            // a texture variable of that name exists
    int      depth_bound;            // and the DEPTH semantic has a live binding
    UINT     depth_w, depth_h, depth_samples;
    int      depth_is_dsv;           // the binding is the game's own depth-stencil texture
    DXGI_FORMAT depth_fmt;
    int      depth_read;             // the readback actually returned pixels
    int      depth_varies;
    UINT     depth_lo, depth_hi;

    // Only read on Vulkan, and measured everywhere so the report and the refusal
    // both come off the same struct rather than off a second lookup.
    uint32_t depth_usage, mv_usage;

    int      mv_which;               // index into kSynthMv, -1 for none
    int      depth_which;            // index into kSynthDepthNames, -1 for none
    int      mv_bound;
    UINT     mv_w, mv_h, mv_samples;
    DXGI_FORMAT mv_fmt;

    int      def_present[_countof(kSynthDepthDefs)];
    int      def_value[_countof(kSynthDepthDefs)];
};

// The report is printed on change, not on a timer, so it has to be possible to
// say two frames gave the same answer. Hashing the measured struct means a new
// fact added above is compared without anybody remembering to extend a list.
static uint64_t SynthSignature(const SynthFacts *f)
{
    uint64_t h = 1469598103934665603ull;
    const unsigned char *p = reinterpret_cast<const unsigned char *>(f);
    for (size_t i = 0; i < sizeof(*f); ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// D3D24-with-stencil depth keeps the stencil in the top byte, and stencil that
// varies over a flat depth buffer would read as depth that varies. The point of
// the check is to catch the buffer that is present and says nothing, so the byte
// that is not depth is dropped before anything is compared.
static UINT SynthDepthBits(DXGI_FORMAT f, UINT raw)
{
    switch (f)
    {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        return raw & 0x00FFFFFFu;
    default:
        return raw;
    }
}

// A ReShade resource_view handle on a D3D11 device is the ID3D11View itself --
// ReShade's own to_handle takes ID3D11View * and reinterpret_casts it, nothing
// else -- so once the binding is in hand no further ReShade type is involved.
//
// ID3D11View rather than ID3D11ShaderResourceView because stage 3 asks this the
// same question about the render target view reshade_begin_effects hands over.
// GetResource is ID3D11View's, so both answer it; naming the base is what makes
// that true by declaration rather than by the two vtables happening to agree.
static ID3D11Texture2D *SynthTextureFromView(reshade::api::resource_view v)
{
    if (v.handle == 0) return nullptr;
    auto *view = reinterpret_cast<ID3D11View *>(static_cast<uintptr_t>(v.handle));

    ID3D11Resource *res = nullptr;
    view->GetResource(&res);
    if (res == nullptr) return nullptr;

    ID3D11Texture2D *tex = nullptr;
    res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex));
    res->Release();
    return tex;   // caller releases
}

// One bound texture, in whichever API the effect runtime is on.
//
// res is what both transports and NGX take: the device's own native resource
// pointer, which is exactly what a reshade::api::resource handle is documented
// to be -- an ID3D11Resource on a D3D11 device, an ID3D12Resource on a D3D12
// one. tex11 is the same object seen as D3D11 and is null on any other API; it
// exists so the depth readback and the D3D11 measurement keep the exact calls
// they were verified with.
struct SynthBound
{
    reshade::api::resource res;
    ID3D11Texture2D       *tex11;   // caller releases when not null
    UINT        w, h, samples;
    DXGI_FORMAT fmt;
    int         is_dsv;
    // reshade::api::resource_usage, straight off the description ReShade keeps
    // for the resource. It exists for one question, asked only on Vulkan: a
    // transfer command may not read an image that was not created as a transfer
    // source, and neither of the two textures this add-on reads is created by
    // this add-on. Left zero on D3D11, where nothing asks.
    uint32_t    usage;
};

// The D3D11 half is the code that was already here, moved rather than rewritten:
// it runs in every shipped session and its facts have to keep coming from the
// same GetDesc. The D3D12 half exists because a D3D12 resource view handle is a
// descriptor handle and says nothing about the texture behind it, so the
// resource has to be asked for by name.
static void SynthResolveView(reshade::api::device *dev, uint32_t api,
                             reshade::api::resource_view v, SynthBound *out)
{
    SynthBound blank = {};
    *out = blank;
    if (v.handle == 0) return;

    if (api == static_cast<uint32_t>(reshade::api::device_api::d3d11))
    {
        out->tex11 = SynthTextureFromView(v);
        if (out->tex11 == nullptr) return;
        D3D11_TEXTURE2D_DESC td = {};
        out->tex11->GetDesc(&td);
        out->res     = reshade::api::resource { reinterpret_cast<uintptr_t>(out->tex11) };
        out->w       = td.Width;
        out->h       = td.Height;
        out->fmt     = td.Format;
        out->samples = td.SampleDesc.Count;
        out->is_dsv  = (td.BindFlags & D3D11_BIND_DEPTH_STENCIL) ? 1 : 0;
        return;
    }

    if (dev == nullptr) return;
    out->res = dev->get_resource_from_view(v);
    if (out->res.handle == 0) return;   // D3D12 allows a view over no resource

    // reshade::api::format carries DXGI's own numbering for every DXGI format,
    // so this cast is the identity it looks like rather than a table lookup
    // somebody has to keep in step.
    const reshade::api::resource_desc rd = dev->get_resource_desc(out->res);
    out->w       = rd.texture.width;
    out->h       = rd.texture.height;
    out->fmt     = static_cast<DXGI_FORMAT>(rd.texture.format);
    out->samples = rd.texture.samples;
    out->is_dsv  = ((rd.usage & reshade::api::resource_usage::depth_stencil) !=
                    reshade::api::resource_usage::undefined) ? 1 : 0;
    out->usage   = static_cast<uint32_t>(rd.usage);
}

// Returns whether the name exists at all, and separately what it is bound to.
// Those are different answers with different fixes -- "no loaded effect declares
// it" versus "declared but the semantic has no live binding" -- and
// get_texture_binding on its own cannot tell them apart, because it answers zero
// for a name that does not exist too.
//
// get_texture_binding leaves both outputs completely untouched when the handle
// is valid but its semantic has no entry in ReShade's binding map, which is what
// generic depth leaves behind every time no depth buffer is selected: it erases
// the entry rather than storing a zero. An uninitialised local would then be
// read back as a live shader resource view. Zeroing first is the caller's job
// and nobody else's, so it is done here where it cannot be forgotten.
static bool SynthLookup(reshade::api::effect_runtime *rt, const char *name,
                        reshade::api::resource_view *out_srv)
{
    *out_srv = reshade::api::resource_view { 0 };

    // A null handle is also what comes back while effects are reloading, so this
    // is safe to call on any frame.
    reshade::api::effect_texture_variable var = rt->find_texture_variable(nullptr, name);
    if (var.handle == 0) return false;

    reshade::api::resource_view srv_srgb = { 0 };
    rt->get_texture_binding(var, out_srv, &srv_srgb);
    return true;
}

static void SynthReadDefs(reshade::api::effect_runtime *rt, SynthFacts *f)
{
    for (int i = 0; i < static_cast<int>(_countof(kSynthDepthDefs)); ++i)
    {
        char   text[64] = {};
        size_t n        = sizeof(text);

        // A miss returns false and writes nothing, so the default from
        // ReShade.fxh is what the shader actually compiled with. A hit with an
        // empty value is a bare #define, which the FX preprocessor cannot
        // evaluate as a number either, so that also falls back.
        f->def_present[i] = rt->get_preprocessor_definition(kSynthDepthDefs[i].name, text, &n) ? 1 : 0;
        f->def_value[i]   = (f->def_present[i] && text[0] != '\0')
                                ? atoi(text)
                                : kSynthDepthDefs[i].fxh_default;
    }
}

// Five points rather than one. A single sample cannot distinguish a depth buffer
// from a cleared one, and "present but flat" is the total-failure case: DLSS
// given a constant depth has no geometry to reproject against and produces a
// smear, with nothing in any log to say why. The neighbouring project misses
// this by testing a handle to a render target it created itself, which is always
// present and never flat.
static void SynthProbeDepth(reshade::api::command_list *cmd, ID3D11Texture2D *tex, SynthFacts *f)
{
    auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(
        static_cast<uintptr_t>(cmd->get_native()));
    if (ctx == nullptr) return;

    // Multisampled depth cannot be copied into a single-sampled staging texture,
    // so there is nothing to read and saying so beats reporting a failed read as
    // a flat buffer.
    if (f->depth_samples != 1) return;

    // Same reasoning, and the case that actually bites: D3D11 will only copy a
    // depth-stencil resource whole -- CopySubresourceRegion demands a null box
    // when either side carries BIND_DEPTH_STENCIL -- and ReShade binds the
    // game's own depth texture directly whenever generic depth has no backup
    // copy to hand out. The boxed copy below is then rejected, Map still
    // succeeds on a staging texture nothing wrote, and all five points come back
    // equal: a buffer full of geometry reported as FLAT, which is the loudest
    // wrong answer this file could give.
    if (f->depth_is_dsv) return;

    ID3D11Device *dev11 = nullptr;
    ctx->GetDevice(&dev11);
    if (dev11 == nullptr) return;

    const UINT xs[5] = { f->depth_w / 4, f->depth_w * 3 / 4, f->depth_w / 2,
                         f->depth_w / 4, f->depth_w * 3 / 4 };
    const UINT ys[5] = { f->depth_h / 4, f->depth_h / 4, f->depth_h / 2,
                         f->depth_h * 3 / 4, f->depth_h * 3 / 4 };

    int got = 0;
    for (int i = 0; i < 5; ++i)
    {
        UINT raw = 0;
        if (!ReadPixelRaw(ctx, dev11, tex, f->depth_fmt, xs[i], ys[i], &raw)) continue;
        const UINT d = SynthDepthBits(f->depth_fmt, raw);
        if (got == 0) { f->depth_lo = f->depth_hi = d; }
        else if (d < f->depth_lo) f->depth_lo = d;
        else if (d > f->depth_hi) f->depth_hi = d;
        ++got;
    }

    dev11->Release();
    f->depth_read   = (got > 0) ? 1 : 0;
    f->depth_varies = (f->depth_hi != f->depth_lo) ? 1 : 0;
}

static const char *SynthFormatName(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16_FLOAT:          return "R16G16_FLOAT";
    case DXGI_FORMAT_R32G32_FLOAT:          return "R32G32_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_FLOAT:    return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R32_FLOAT:             return "R32_FLOAT";
    case DXGI_FORMAT_R32_TYPELESS:          return "R32_TYPELESS";
    case DXGI_FORMAT_R24G8_TYPELESS:        return "R24G8_TYPELESS";
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return "R24_UNORM_X8_TYPELESS";
    case DXGI_FORMAT_D24_UNORM_S8_UINT:     return "D24_UNORM_S8_UINT";
    case DXGI_FORMAT_D32_FLOAT:             return "D32_FLOAT";
    // The two a Vulkan game routinely arrives with. Naming them is what lets the
    // refusal for a depth format the Vulkan transport cannot carry point at the
    // measurement instead of at a number.
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:  return "D32_FLOAT_S8X24_UINT";
    case DXGI_FORMAT_D16_UNORM:             return "D16_UNORM";
    default:                                return "other";
    }
}

static const char *SynthApiName(uint32_t api)
{
    switch (static_cast<reshade::api::device_api>(api))
    {
    case reshade::api::device_api::d3d9:   return "D3D9";
    case reshade::api::device_api::d3d10:  return "D3D10";
    case reshade::api::device_api::d3d11:  return "D3D11";
    case reshade::api::device_api::d3d12:  return "D3D12";
    case reshade::api::device_api::opengl: return "OpenGL";
    case reshade::api::device_api::vulkan: return "Vulkan";
    default:                               return "an API this add-on has no name for";
    }
}

// Three transports ship in one binary, and which one a session gets is decided
// by the device ReShade is on -- never by what was compiled in. Everything that
// prints a backend reads it through here, off SynthFacts::device_api, which was
// filled from the runtime's own get_api.
static bool SynthApiHasBackend(uint32_t api)
{
    return api == static_cast<uint32_t>(reshade::api::device_api::d3d11) ||
           api == static_cast<uint32_t>(reshade::api::device_api::d3d12) ||
           api == static_cast<uint32_t>(reshade::api::device_api::vulkan);
}

// ---------------------------------------------------------------------------
// The verdict, computed once
//
// Extracted from SynthReport so the report and the report's own rate limit read
// the SAME computation. The alternative -- a second copy of these conditions
// kept in step by hand -- has already failed twice in this file: SynthArmRefusal
// and this verdict disagreed about the depth size and about the sample count,
// and each time the report said "every precondition holds" over a session the
// gate refused. A comparison that is meant to detect a changed verdict has to be
// derived from the verdict itself, or it drifts the same way.
// ---------------------------------------------------------------------------

struct SynthVerdictFacts
{
    bool depth_ok;
    bool mv_ok;
    bool aside;
    bool depth_size_ok;
    bool vk_depth_ok;
    bool vk_mv_ok;
};

static SynthVerdictFacts SynthVerdictOf(const SynthFacts *f)
{
    SynthVerdictFacts v = {};
    // The two extra conditions the Vulkan transport puts on the same two
    // textures, folded in here so the verdict is the verdict for the transport
    // this session would actually use rather than for a different one.
    const bool vk = f->device_api == static_cast<uint32_t>(reshade::api::device_api::vulkan);
    const uint32_t copy_src = static_cast<uint32_t>(reshade::api::resource_usage::copy_source);
    const uint32_t shader_r = static_cast<uint32_t>(reshade::api::resource_usage::shader_resource);
    // Rewritten with the refusal it mirrors: the depth is sampled now, so what
    // it needs is shader_resource, and its format is no longer a question at all.
    v.vk_depth_ok = !vk || (f->depth_usage & shader_r) != 0;
    // Same clause as the gate's, for the same reason: row 0 has no ReShade
    // texture to ask about.
    v.vk_mv_ok    = !vk || f->mv_which == 0 || (f->mv_usage & copy_src) != 0;

    // The size test was in SynthArmRefusal and missing here, so this verdict could
    // read "every precondition holds" over a session the gate would refuse -- and
    // it did, in Baldur's Gate 3 on 2026-08-30: depth 2560x1067 against a
    // 3840x1600 back buffer, because a game that runs DLSS renders its depth at
    // render resolution while ReShade presents at output resolution. A game with
    // no DLSS of its own -- the only kind synth is for -- has the two equal, so
    // this would have stayed dormant until a field report nobody could explain.
    // Every condition the gate refuses on has to be a condition this verdict
    // fails on, or the report is worse than no report.
    // Checked against SynthArmRefusal condition by condition on 2026-08-30. Two
    // were missing: the size, and this one -- the motion vector arm tested
    // mv_samples == 1 while the depth arm tested nothing, so a multisampled
    // depth buffer passed the verdict and was refused by the gate.
    v.depth_size_ok = f->depth_w == f->back_w && f->depth_h == f->back_h;
    // The probe clause reads the way the GATE reads it and not the other way
    // round, and the difference matters on Vulkan: the five-point probe is
    // written against D3D11 staging textures and never runs there, so
    // depth_read is 0 and this verdict used to print "not viable -- depth
    // unverified" over a session the gate would have armed. Nothing was
    // disproved by not sampling, which is exactly what the gate says by refusing
    // only on read-AND-flat.
    v.depth_ok = f->depth_bound && v.depth_size_ok && f->depth_samples == 1 &&
                          (!f->depth_read || f->depth_varies) && v.vk_depth_ok;
    v.mv_ok    = f->mv_bound && f->mv_fmt == DXGI_FORMAT_R16G16_FLOAT &&
                          f->mv_w == f->back_w && f->mv_h == f->back_h &&
                          f->mv_samples == 1 && v.vk_mv_ok;
    v.aside    = f->def_value[SYNTH_DEF_UPSIDE_DOWN] != 0 ||
                          f->def_value[SYNTH_DEF_LOGARITHMIC] != 0;

    return v;
}

// The depth half of the verdict, as one index into the table below. Most
// specific true reason first: size and sample count are read off a descriptor,
// and an unbound depth has no descriptor -- its width is zero, so an earlier
// size test fires and reports "wrong size" for a buffer whose real problem is
// that generic depth never picked one. Baldur's Gate 3 on 2026-08-31 printed
// exactly that contradiction: "present but NOT BOUND" one line above "depth
// wrong size".
static uint32_t SynthDepthReason(const SynthFacts *f, const SynthVerdictFacts &v)
{
    return v.depth_ok             ? 0u :
           !f->depth_found        ? 1u :
           !f->depth_bound        ? 2u :
           !v.depth_size_ok       ? 3u :
           f->depth_samples != 1  ? 4u :
           // Only reachable on Vulkan, and only for a binding with no
           // shader_resource usage -- which generic_depth's backup always has,
           // so this is the surprise case and is named as one rather than
           // folded into "no".
           !v.vk_depth_ok         ? 5u : 6u;
}

// Indexed by SynthDepthReason, and that is the point: the verdict line and the
// verdict key read the same ladder from the same function, so a reason added to
// one cannot go missing from the other.
static const char *const kSynthDepthReason[] = {
    "ok", "no", "not bound", "wrong size", "multisampled", "not sampleable", "flat"
};

// The ladder's largest index is 6, so the table needs seven entries and the key
// needs three bits -- which is why the mv and aside flags start at 0x10.
static_assert(_countof(kSynthDepthReason) == 7,
              "kSynthDepthReason must have one entry per SynthDepthReason result");

// A small integer standing for exactly what the verdict line prints -- the depth
// reason, whether the motion vectors are ok, and whether a depth encoding stands
// the session aside. Nothing else: the point is that two probes with the same
// story compare equal even when unrelated bytes of SynthFacts moved.
//
// depth_read and depth_varies reach this through depth_ok, so it is only
// meaningful AFTER SynthProbeDepth has run. Computing it before the readback
// makes the flat-depth verdict structurally invisible to the comparison.
static uint32_t SynthVerdictKey(const SynthFacts *f)
{
    const SynthVerdictFacts v = SynthVerdictOf(f);
    return SynthDepthReason(f, v) | (v.mv_ok ? 0x10u : 0u) | (v.aside ? 0x20u : 0u);
}

// Prints every precondition with the measurement behind it. A verdict on its own
// sends people to a forum; the number that produced it is what lets them fix it.
static void SynthReport(const SynthFacts *f)
{
    Log("[synth] ----- could a synthetic DLSS contract be built here? -----");

    Log("[synth]   ReShade add-on API: registered at %u, synth needs %u or newer -- %s",
        f->api, kSynthApiFloor,
        f->api >= kSynthApiFloor ? "ok"
                                 : "REFUSED; effect_runtime has a different vtable below that");
    if (f->api < kSynthApiFloor) return;

    // Derived from the device, not from the build. The two transports differ in
    // where the evaluate happens and how many devices the frame touches, and a
    // support report that does not say which one ran cannot be read at all.
    Log("[synth]   backend: the effect runtime is on %s, so %s",
        SynthApiName(f->device_api),
        f->device_api == static_cast<uint32_t>(reshade::api::device_api::d3d11)
            ? "the frame is copied onto a private D3D12 device and the evaluate runs there"
            : f->device_api == static_cast<uint32_t>(reshade::api::device_api::d3d12)
                ? "the evaluate runs on the game's own D3D12 device and no frame data "
                  "crosses between devices"
            : f->device_api == static_cast<uint32_t>(reshade::api::device_api::vulkan)
                ? "the frame crosses into a private D3D12 device through shared textures "
                  "imported back into the game's Vulkan device, and the evaluate runs there"
                : "there is no transport for it and nothing below can be measured");
    if (!SynthApiHasBackend(f->device_api)) return;

    Log("[synth]   back buffer: %ux%u", f->back_w, f->back_h);

    // Not present and not bound are different failures with different fixes:
    // one needs an effect that samples depth to be enabled, the other needs
    // generic depth to have picked a buffer.
    if (!f->depth_found)
    {
        char tried[128] = {};
        for (int i = 0; i < static_cast<int>(_countof(kSynthDepthNames)); ++i)
        {
            if (i != 0) strcat_s(tried, " or ");
            strcat_s(tried, kSynthDepthNames[i]);
        }
        // "None of the names this add-on knows" and "no depth buffer" are not
        // the same statement, and only the first one is derived here: any
        // texture annotated ": DEPTH" binds to the same semantic, so a provider
        // that spells it differently is invisible to this walk.
        Log("[synth]   depth: NOT PRESENT under %s -- no loaded effect declares a depth "
            "texture under a name this add-on knows. Any texture annotated \": DEPTH\" "
            "would bind to the same semantic, so a shader that spells it otherwise is "
            "invisible here. Generic depth may well have a depth buffer.", tried);
    }
    else if (!f->depth_bound)
        Log("[synth]   depth (%s): present but NOT BOUND -- the DEPTH semantic has no live "
            "binding, which is what generic depth leaves behind when it has not selected a "
            "depth buffer.", kSynthDepthNames[f->depth_which]);
    else
    {
        Log("[synth]   depth (%s): bound, %ux%u %s, %u sample%s", kSynthDepthNames[f->depth_which],
            f->depth_w, f->depth_h, SynthFormatName(f->depth_fmt),
            f->depth_samples, f->depth_samples == 1 ? "" : "s");

        // Said here rather than left to be inferred from two numbers on one
        // line. NGX takes depth at the render resolution the contract declares,
        // and this contract declares the back buffer; a depth surface of another
        // size is a broken contract only NGX can refuse, one evaluate at a time.
        if (f->depth_w != f->back_w || f->depth_h != f->back_h)
            Log("[synth]     size: MISMATCH -- %ux%u against a %ux%u back buffer. A game "
                "running its own upscaler renders depth at render resolution while ReShade "
                "presents at output resolution, which is this case and is not something a "
                "setting fixes. A game with no upscaler of its own has the two equal.",
                f->depth_w, f->depth_h, f->back_w, f->back_h);

        if (!f->depth_read)
            // Each reason is read off a fact that was measured. "not 1 sample"
            // used to stand for all three, so a view this add-on could not
            // resolve to a Texture2D -- which leaves the count at zero, never at
            // two -- was reported as multisampled.
            Log("[synth]   depth values: could not be read back%s",
                f->device_api != static_cast<uint32_t>(reshade::api::device_api::d3d11)
                    ? " -- the five-point probe is written against D3D11 staging textures "
                      "and this runtime is not on D3D11. Nothing was disproved; the buffer "
                      "was simply not sampled"
                : f->depth_is_dsv
                    ? " -- the binding is the game's own depth-stencil texture, and D3D11 "
                      "copies such a resource only whole, never the points this samples"
                    : f->depth_samples > 1
                          ? " -- a multisampled depth buffer cannot be copied to a staging texture"
                          : "");
        else if (!f->depth_varies)
            Log("[synth]   depth values: FLAT -- five points across the buffer all read "
                "0x%08X. A constant depth gives DLSS no geometry to reproject against, and "
                "the result is a smear rather than an error.", f->depth_lo);
        else
            Log("[synth]   depth values: vary across the buffer, 0x%08X to 0x%08X over five "
                "points", f->depth_lo, f->depth_hi);
    }

    if (f->mv_which < 0)
    {
        char tried[128] = {};
        // From kSynthMvReShadeFirst: row 0 is the driver's own optical flow
        // engine and no effect can declare it, so naming it here would send a
        // reader looking for a shader that does not exist. See the table's note.
        for (int i = kSynthMvReShadeFirst; i < static_cast<int>(_countof(kSynthMv)); ++i)
        {
            if (i != kSynthMvReShadeFirst) strcat_s(tried, " or ");
            strcat_s(tried, kSynthMv[i].name);
        }
        Log("[synth]   motion vectors: NOT PRESENT -- no loaded effect declares %s. A motion "
            "estimation shader has to be installed and enabled before there is anything to "
            "read.", tried);
    }
    else if (!f->mv_bound)
    {
        Log("[synth]   motion vectors (%s): present but NOT BOUND", kSynthMv[f->mv_which].name);
    }
    else
    {
        Log("[synth]   motion vectors (%s): bound, %ux%u %s, %u sample%s -- %s",
            kSynthMv[f->mv_which].name, f->mv_w, f->mv_h, SynthFormatName(f->mv_fmt),
            f->mv_samples, f->mv_samples == 1 ? "" : "s", kSynthMv[f->mv_which].label);

        // Which of the two answers produced MV.Scale's sign. Without this line a
        // report of smearing cannot be told apart from a report of a wrong knob.
        {
            const int px = g_cfg.mv_sign_x != 0 ? g_cfg.mv_sign_x : kSynthMv[f->mv_which].sign_x;
            const int py = g_cfg.mv_sign_y != 0 ? g_cfg.mv_sign_y : kSynthMv[f->mv_which].sign_y;
            if (g_cfg.mv_sign_x != 0 || g_cfg.mv_sign_y != 0)
                Log("[synth]     direction: %+d,%+d from mv_sign_x/mv_sign_y in the config", px, py);
            else if (f->mv_which == 0)
                // Above the provider branch below because this one is not a
                // convention read out of somebody's source. It was measured here.
                // The number below is a STORED RESULT, not this session's. It was
                // measured once on this machine against a synthetic scene with a
                // known displacement; nothing re-measures it at runtime, and
                // printing it in the present tense told every reader of every log
                // that their own session had verified it. What this session does
                // verify is the conversion, which the self-check reports
                // separately and which is a different claim.
                Log("[synth]     direction: %+d,%+d. Established once on this machine -- "
                    "1,166,400 vectors, all within 0.5 px of a known displacement -- rather "
                    "than read out of somebody's source. Not re-measured here", px, py);
            else if (kSynthMv[f->mv_which].sign_x != 0)
                Log("[synth]     direction: %+d,%+d, this provider's own documented convention",
                    px, py);
            else
                Log("[synth]     direction: this provider does not state one and its source is "
                    "not readable, so +1,+1 is assumed. If the image smears in the direction of "
                    "movement, set mv_sign_y=-1 in dlss5-bridge.cfg.");
        }

        // Both shipped providers declare RG16F at BUFFER_WIDTH x BUFFER_HEIGHT.
        // Anything else is not the texture this was written against, and saying
        // what it actually is turns a report into a fix.
        if (f->mv_fmt != DXGI_FORMAT_R16G16_FLOAT)
            Log("[synth]     shape: WRONG FORMAT -- %s, expected R16G16_FLOAT, which is what "
                "both shipped motion estimation shaders declare",
                SynthFormatName(f->mv_fmt));
        if (f->mv_w != f->back_w || f->mv_h != f->back_h)
            Log("[synth]     shape: WRONG SIZE -- %ux%u against a %ux%u back buffer",
                f->mv_w, f->mv_h, f->back_w, f->back_h);
        if (f->mv_samples != 1)
            Log("[synth]     shape: MULTISAMPLED -- %u samples, and NGX takes single-sampled "
                "motion vectors", f->mv_samples);
        if (f->mv_fmt == DXGI_FORMAT_R16G16_FLOAT && f->mv_w == f->back_w &&
            f->mv_h == f->back_h && f->mv_samples == 1)
            Log("[synth]     shape: matches the back buffer at R16G16_FLOAT");
    }

    // Only on Vulkan, because only there is it a question, and the two textures
    // are asked different questions because they are read in different ways: the
    // motion vectors are copied, so they need copy_source, while the depth is
    // SAMPLED by a compute pass, so it needs shader_resource and nothing else.
    // Neither of them is created by this add-on.
    if (f->device_api == static_cast<uint32_t>(reshade::api::device_api::vulkan))
    {
        const uint32_t copy_src = static_cast<uint32_t>(reshade::api::resource_usage::copy_source);
        const uint32_t shader_r = static_cast<uint32_t>(reshade::api::resource_usage::shader_resource);
        Log("[synth]   Vulkan usage: depth 0x%08X (%s be sampled), motion vectors 0x%08X "
            "(%s be copied from)",
            f->depth_usage, (f->depth_usage & shader_r) ? "can" : "CANNOT",
            f->mv_usage,    (f->mv_usage    & copy_src) ? "can" : "CANNOT");
        Log("[synth]     on Vulkan the DEPTH semantic is bound to ReShade's own depth "
            "backup texture rather than to the game's depth image -- generic_depth forces "
            "a backup on this API -- and that backup is created shader_resource and "
            "copy_dest, with NO copy_source. That is why the depth is read by a compute "
            "pass that samples it rather than by a transfer: a copy cannot be issued "
            "against it, a sample can, and a sampled depth-aspect texel is a normalised "
            "float whatever the storage format is. Neither the format nor the missing "
            "copy_source is a refusal any more.%s",
            (f->depth_usage & copy_src) ? " This binding does carry copy_source as well, "
                                          "which is unusual on this API." : "");
    }

    // Absent means the ReShade.fxh default compiled in, which is not the same as
    // zero -- IS_REVERSED defaults to 1 -- so both the state and the value are
    // printed rather than one standing for the other.
    for (int i = 0; i < static_cast<int>(_countof(kSynthDepthDefs)); ++i)
        Log("[synth]   %s = %d (%s)", kSynthDepthDefs[i].name, f->def_value[i],
            f->def_present[i] ? "set in the preset or the global config"
                              : "not present, so the ReShade.fxh default applies");

    // Standing aside is not the same as failing. These two describe depth this
    // add-on can read but cannot describe to NGX: the create flags say whether
    // depth is inverted and nothing else about its encoding, so a flipped or
    // logarithmic buffer would be handed over as if it were neither.
    if (f->def_value[SYNTH_DEF_UPSIDE_DOWN])
        Log("[synth]   STAND ASIDE: RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN is %d. NGX has no "
            "create flag or parameter for a vertically flipped depth buffer, so there is no "
            "way to state it and it would be read upside down in silence.",
            f->def_value[SYNTH_DEF_UPSIDE_DOWN]);
    if (f->def_value[SYNTH_DEF_LOGARITHMIC])
        Log("[synth]   STAND ASIDE: RESHADE_DEPTH_INPUT_IS_LOGARITHMIC is %d. NGX takes "
            "hardware depth and has no flag for a logarithmic encoding.",
            f->def_value[SYNTH_DEF_LOGARITHMIC]);
    Log("[synth]   RESHADE_DEPTH_INPUT_IS_REVERSED is expressible: it is the DepthInverted "
        "create flag, bit 0x08.");
    Log("[synth]   RESHADE_DEPTH_LINEARIZATION_FAR_PLANE has nothing to express: NGX is given "
        "raw depth, and the far plane is a shader-side convenience.");

    // Only the three the verdict line reads. depth_size_ok and vk_depth_ok were
    // named here too when this was extracted, and their sole remaining use is
    // inside SynthDepthReason, which takes v and asks for itself.
    const SynthVerdictFacts v = SynthVerdictOf(f);
    const bool depth_ok = v.depth_ok;
    const bool mv_ok    = v.mv_ok;
    const bool aside    = v.aside;

    if (depth_ok && mv_ok && !aside)
        Log("[synth]   verdict: every precondition holds. Whether the synthetic contract "
            "is also armed is a separate question -- synth_after, source and the game's "
            "own DLSS decide it, and the arming line says so when it happens.");
    else
        // A depth buffer that is bound but could not be sampled is not the same
        // answer as one that is missing or flat: nothing was disproved, only
        // left unmeasured, and the line above says which. Printing "no" for it
        // would send someone to change a depth setting that is already right.
        Log("[synth]   verdict: not viable as things stand -- depth %s, motion vectors %s%s.",
            kSynthDepthReason[SynthDepthReason(f, v)],
            mv_ok ? "ok" : "no",
            aside ? ", and a depth encoding that cannot be stated to NGX" : "");

    Log("[synth] ----- end of report -----");
}

// ---------------------------------------------------------------------------
// Stage 3: the synthetic contract
//
// The seam this uses was already here. NVSDK_NGX_Parameter is a pure abstract
// class and BridgeFrame takes a const NVSDK_NGX_Parameter *, so a second source
// of contract is a subclass and nothing in the mirror's frame path has to learn
// that it exists. Everything below builds one such object, answers the Gets
// bridge.inc performs, and hands it to the same BridgeFrame the game's evaluate
// detour calls.
//
// The note above NoteAbsentParam binds all of it: this contract arrives already
// complete. Every key in the table below is set explicitly by SynthBuild, and
// SynthSelfCheck reads every one of them back through the object to prove it,
// because a synthetic block that reaches NoteAbsentParam has turned absence into
// a normal operating mode -- and the absence report is what found the MV.Scale
// defect that cost two releases.
// ---------------------------------------------------------------------------

// What a parameter block returns for a key it does not carry. Not invented:
// 0xBAD00010 is UnsupportedParameter, which is what the game's own NGX block
// answers for an unknown key in every log this project has collected, and
// bridge.inc's NgxResultName already names it.
static const NVSDK_NGX_Result kSynthNoSuchKey = static_cast<NVSDK_NGX_Result>(0xBAD00010);

enum SynthKind { SK_UINT, SK_INT, SK_FLOAT, SK_RES };

// The complete inventory of what bridge.inc reads off a source's parameter
// block, derived by reading every Get in the mirror's frame path rather than
// taken from a list: GameTexture's four slots and ExposureTexture in
// BridgeFrameInner, the four size scalars, Reset, the ten subrect keys, the
// seven floats, and PerfQualityValue, DLSS.Feature.Create.Flags and
// DLSS.Enable.Output.Subrects in BridgeBuildResources.
//
// Color, Output, Depth and MotionVectors appear here as the source's own D3D11
// textures. What bridge.inc sets into its D3D12 parameter block are its own
// shared mirrors of them, which is a different set of keys on a different block
// and no business of this one.
//
// Deliberately absent, and absent is the answer rather than an omission:
//
//   ExposureTexture -- there is no exposure texture. BridgeFrameInner reads it
//   directly rather than through the neutral helpers, and treats a failed Get as
//   "the source supplies none", which is exactly true here. Answering with a
//   null would be supplying one.
//
//   the six DLSS.Hint.Render.Preset.* keys -- bridge.inc mirrors only what the
//   source set, because nvsdk_ngx_helpers.h never writes these and absence
//   already means NVSDK_NGX_DLSS_Hint_Render_Preset_Default. Setting one would
//   pick a model this add-on has no measurement to justify picking. Neither
//   reaches NoteAbsentParam, so constraint (a) is untouched either way.
static const struct { const char *key; SynthKind kind; } kSynthSlots[] = {
    { "Color",                                SK_RES   },
    { "Output",                               SK_RES   },
    { "Depth",                                SK_RES   },
    { "MotionVectors",                        SK_RES   },

    { "Width",                                SK_UINT  },
    { "Height",                               SK_UINT  },
    { "OutWidth",                             SK_UINT  },
    { "OutHeight",                            SK_UINT  },

    { "PerfQualityValue",                     SK_UINT  },
    { "DLSS.Feature.Create.Flags",            SK_UINT  },
    { "DLSS.Enable.Output.Subrects",          SK_UINT  },

    { "DLSS.Render.Subrect.Dimensions.Width",  SK_UINT },
    { "DLSS.Render.Subrect.Dimensions.Height", SK_UINT },
    { "DLSS.Input.Color.Subrect.Base.X",      SK_UINT  },
    { "DLSS.Input.Color.Subrect.Base.Y",      SK_UINT  },
    { "DLSS.Input.Depth.Subrect.Base.X",      SK_UINT  },
    { "DLSS.Input.Depth.Subrect.Base.Y",      SK_UINT  },
    { "DLSS.Input.MV.Subrect.Base.X",         SK_UINT  },
    { "DLSS.Input.MV.Subrect.Base.Y",         SK_UINT  },
    { "DLSS.Output.Subrect.Base.X",           SK_UINT  },
    { "DLSS.Output.Subrect.Base.Y",           SK_UINT  },

    { "MV.Scale.X",                           SK_FLOAT },
    { "MV.Scale.Y",                           SK_FLOAT },
    { "Jitter.Offset.X",                      SK_FLOAT },
    { "Jitter.Offset.Y",                      SK_FLOAT },
    { "Sharpness",                            SK_FLOAT },
    { "DLSS.Pre.Exposure",                    SK_FLOAT },
    { "DLSS.Exposure.Scale",                  SK_FLOAT },

    { "Reset",                                SK_INT   },
};

class SynthParams : public NVSDK_NGX_Parameter
{
public:
    SynthParams() { Clear(); }

    void Clear()
    {
        memset(m_v, 0, sizeof(m_v));
        memset(m_set, 0, sizeof(m_set));
    }

    // Set is how the contract is built, so it goes through the same table Get
    // answers from. A key the table does not carry is a mistake in the builder,
    // not a parameter to store: storing it would make SynthSelfCheck pass over a
    // contract bridge.inc cannot read.
    void Set(const char *n, unsigned long long v) override { (void)v; Unknown(n, "unsigned long long"); }
    void Set(const char *n, double v)             override { (void)v; Unknown(n, "double"); }
    void Set(const char *n, void *v)              override { (void)v; Unknown(n, "void *"); }
    void Set(const char *n, ID3D12Resource *v)    override { (void)v; Unknown(n, "ID3D12Resource *"); }

    void Set(const char *n, unsigned int v) override
    { const int i = Find(n, SK_UINT);  if (i < 0) { Unknown(n, "unsigned int"); return; } m_v[i].u = v; m_set[i] = true; }
    void Set(const char *n, int v) override
    { const int i = Find(n, SK_INT);   if (i < 0) { Unknown(n, "int"); return; }          m_v[i].i = v; m_set[i] = true; }
    void Set(const char *n, float v) override
    { const int i = Find(n, SK_FLOAT); if (i < 0) { Unknown(n, "float"); return; }        m_v[i].f = v; m_set[i] = true; }
    void Set(const char *n, ID3D11Resource *v) override
    { const int i = Find(n, SK_RES);   if (i < 0) { Unknown(n, "ID3D11Resource *"); return; } m_v[i].res = v; m_set[i] = true; }

    NVSDK_NGX_Result Get(const char *n, unsigned long long *) const override { (void)n; return kSynthNoSuchKey; }
    NVSDK_NGX_Result Get(const char *n, double *)             const override { (void)n; return kSynthNoSuchKey; }
    NVSDK_NGX_Result Get(const char *n, void **)              const override { (void)n; return kSynthNoSuchKey; }
    NVSDK_NGX_Result Get(const char *n, ID3D12Resource **)    const override { (void)n; return kSynthNoSuchKey; }

    NVSDK_NGX_Result Get(const char *n, unsigned int *out) const override
    { const int i = Find(n, SK_UINT);  if (i < 0 || !m_set[i]) return kSynthNoSuchKey; *out = m_v[i].u; return NGX_SUCCESS; }
    NVSDK_NGX_Result Get(const char *n, int *out) const override
    { const int i = Find(n, SK_INT);   if (i < 0 || !m_set[i]) return kSynthNoSuchKey; *out = m_v[i].i; return NGX_SUCCESS; }
    NVSDK_NGX_Result Get(const char *n, float *out) const override
    { const int i = Find(n, SK_FLOAT); if (i < 0 || !m_set[i]) return kSynthNoSuchKey; *out = m_v[i].f; return NGX_SUCCESS; }

    NVSDK_NGX_Result Get(const char *n, ID3D11Resource **out) const override
    {
        const int i = Find(n, SK_RES);
        if (i < 0 || !m_set[i] || m_v[i].res == nullptr) return kSynthNoSuchKey;

        // No AddRef here, and this is the one line in the file that is silent
        // for hours if it is wrong. NGX does not AddRef the resources in a
        // parameter block -- that is precisely why a game has to keep them alive
        // across the call, and why BridgeReleaseExposure has to clear the key
        // before freeing the texture. GameTexture in bridge.inc balances its own
        // QueryInterface against the caller's Release; the ID3D11Resource it
        // takes out of this Get is never released by anybody. An AddRef here is
        // therefore one reference leaked per texture per frame, with no failure,
        // no log line and no visible symptom until the process runs out of
        // memory an hour later.
        *out = m_v[i].res;
        return NGX_SUCCESS;
    }

    void Reset() override { Clear(); }

private:
    struct Val { unsigned int u; int i; float f; ID3D11Resource *res; };

    // Name and type together. A Set that supplies the right name at the wrong
    // width would otherwise store a value no Get in bridge.inc can retrieve, and
    // the self-check would report the key as unset without saying why.
    static int Find(const char *n, SynthKind k)
    {
        for (int i = 0; i < static_cast<int>(_countof(kSynthSlots)); ++i)
            if (kSynthSlots[i].kind == k && strcmp(kSynthSlots[i].key, n) == 0) return i;
        return -1;
    }

    static void Unknown(const char *n, const char *type)
    {
        // Once per key. This fires only when the builder and the table disagree,
        // which is a defect in this file rather than anything about the game, so
        // it is worth saying and not worth repeating sixty times a second.
        static const char *seen[16];
        static int         count;
        for (int i = 0; i < count; ++i) if (strcmp(seen[i], n) == 0) return;
        if (count < static_cast<int>(_countof(seen))) seen[count++] = n;
        Log("[synth] DEFECT: the synthetic contract set %s as %s, which is not a key "
            "the mirror's frame path reads at that type. The value is discarded; add "
            "it to kSynthSlots or stop setting it.", n, type);
    }

    Val  m_v[_countof(kSynthSlots)];
    bool m_set[_countof(kSynthSlots)];
};

// The one runnable check this stage leaves behind, and it ships enabled. It
// reads every key back through the object's own virtual Get -- not out of the
// table it was written into -- so it exercises the same path bridge.inc will,
// and fails on exactly the defect constraint (a) exists to prevent: a synthetic
// contract that arrives incomplete and lets NoteAbsentParam choose a value
// nothing measured.
static void SynthSelfCheck(const SynthParams *sp, bool mv_from_engine = false)
{
    // Per key rather than per session: a key set only inside a branch would
    // otherwise be reported on the first frame and never again, or not at all
    // depending on which frame ran the check.
    static bool said[_countof(kSynthSlots)];

    for (int i = 0; i < static_cast<int>(_countof(kSynthSlots)); ++i)
    {
        bool ok = false;
        switch (kSynthSlots[i].kind)
        {
        case SK_UINT:  { unsigned int    v = 0;       ok = sp->Get(kSynthSlots[i].key, &v) == NGX_SUCCESS; break; }
        case SK_INT:   { int             v = 0;       ok = sp->Get(kSynthSlots[i].key, &v) == NGX_SUCCESS; break; }
        case SK_FLOAT: { float           v = 0.0f;    ok = sp->Get(kSynthSlots[i].key, &v) == NGX_SUCCESS; break; }
        case SK_RES:   { ID3D11Resource *v = nullptr; ok = sp->Get(kSynthSlots[i].key, &v) == NGX_SUCCESS; break; }
        }
        // MotionVectors legitimately has no ReShade resource when the driver's own
        // optical flow engine is the source: the vectors are written into the
        // shared D3D12 texture from the other side, and the D3D12 evaluate sets
        // that key itself. Reporting a DEFECT for it would be this file accusing
        // itself of the arrangement it was built to have.
        if (mv_from_engine && strcmp(kSynthSlots[i].key, "MotionVectors") == 0)
            continue;
        if (ok || said[i]) continue;
        said[i] = true;
        Log("[synth] DEFECT: synthetic contract left %s unset", kSynthSlots[i].key);
    }
}

// IsHDR states that the colour carries values outside [0,1]. That is a property
// of the format's encoding and of nothing else: every UNORM format is clamped to
// [0,1] by definition, the ten-bit ones included, so only a float format can
// carry what the flag claims.
//
// FormatIsLowPrecisionUnorm next door answers a narrower question -- it names
// the seven eight-bit formats, for a diagnostic the mirror prints over flags the
// game itself declared -- and reading its "false" as "can hold more than 1.0"
// set IsHDR over an R10G10B10A2_UNORM back buffer, which is what ReShade sees on
// an HDR10 swapchain. The DLSS 5 add-on picks its codec from this flag, so that
// is a silent wrong answer rather than an error, and the mirror's own warning
// for it does not fire because that format is not eight-bit either.
static bool SynthFormatCanExceedOne(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return true;
    default:
        return false;
    }
}

// Each bit off something this add-on measured, and the reason is logged beside
// the bit rather than described in the abstract.
//
// MVLowRes (0x02) is deliberately not set. At 1:1 the motion vectors are at
// render resolution, so the flag would happen to be true -- but nothing here
// measured "the motion vectors are lower resolution than the colour", it
// measured that two textures are the same size, and a flag set because a value
// coincides with it is exactly the invention the create-flags comment in
// bridge.inc was written about. MVJittered, DoSharpening and AutoExposure are
// unset for the same reason: no measurement supports any of them.
static unsigned int SynthCreateFlags(DXGI_FORMAT back_fmt, int depth_reversed)
{
    unsigned int f = 0;

    // IsHDR selects the DLSS 5 add-on's codec. The measurement is the back
    // buffer's own format, and the question it has to answer is whether that
    // format can hold a value above 1.0 -- not whether it is wider than eight
    // bits, which is a different question with a different answer at ten. A PQ
    // presentation is the exception: the texture NGX gets is the decoded float
    // (PqBackBuffer, bridge.inc), so the flag is true of what it reads.
    if (SynthFormatCanExceedOne(back_fmt) || PqBackBuffer(back_fmt)) f |= 0x01;

    // DepthInverted is the one thing about ReShade's depth encoding that NGX has
    // a way to state, and RESHADE_DEPTH_INPUT_IS_REVERSED is the definition that
    // states it. Absent means the ReShade.fxh default, which is 1 here -- so the
    // common case sets this bit, and reading a missing definition as 0 would
    // have inverted it.
    if (depth_reversed) f |= 0x08;

    return f;
}

// Everything the block carries, set explicitly, from a value that was measured
// on this frame. The four size scalars are all set here, which is what makes the
// size fallback in BridgeFrameInner -- absent scalars derived from the Color and
// Output descriptors -- dead code on this path: constraint (b) says that repair
// is for a game that under-declares and never a supply route.
// The three textures arrive as reshade::api::resource rather than as
// ID3D11Texture2D, and that is what makes one builder serve both backends. A
// resource handle is documented as the device's own native pointer -- an
// ID3D11Resource on a D3D11 device, an ID3D12Resource on a D3D12 one -- so the
// SK_RES slots below carry exactly what a slot in NVSDK_NGX_Parameter carries,
// and SynthParams is untouched. Nothing dereferences these here; the transport
// that consumes the contract knows which API it is on and the block does not
// have to.
static void SynthBuild(SynthParams *sp, unsigned int w, unsigned int h,
                       DXGI_FORMAT back_fmt, reshade::api::resource back,
                       reshade::api::resource depth, reshade::api::resource mv,
                       int depth_reversed, int which)
{
    sp->Clear();

    // Colour in and colour out are the same surface. There is no second buffer
    // to render into: ReShade hands over the back buffer it is about to apply
    // effects to, and the result has to land back in it to be seen.
    sp->Set("Color",         reinterpret_cast<ID3D11Resource *>(static_cast<uintptr_t>(back.handle)));
    sp->Set("Output",        reinterpret_cast<ID3D11Resource *>(static_cast<uintptr_t>(back.handle)));
    sp->Set("Depth",         reinterpret_cast<ID3D11Resource *>(static_cast<uintptr_t>(depth.handle)));
    sp->Set("MotionVectors", reinterpret_cast<ID3D11Resource *>(static_cast<uintptr_t>(mv.handle)));

    // Strictly 1:1. This add-on can measure the back buffer and nothing else:
    // there is no render target at a lower resolution to upscale from, because
    // the game already finished drawing at full size before ReShade was called.
    // Input equals output and the preset is DLAA, which is the only contract the
    // available inputs describe.
    sp->Set("Width",     w);
    sp->Set("Height",    h);
    sp->Set("OutWidth",  w);
    sp->Set("OutHeight", h);
    sp->Set("PerfQualityValue", 5u);   // NVSDK_NGX_PerfQuality_Value_DLAA

    sp->Set("DLSS.Feature.Create.Flags", SynthCreateFlags(back_fmt, depth_reversed));

    // No subrects: every input is a whole texture at the same size, so there is
    // no region to place and the bases are zero because they were measured to be
    // zero, not because zero is a safe default.
    sp->Set("DLSS.Enable.Output.Subrects", 0u);
    sp->Set("DLSS.Render.Subrect.Dimensions.Width",  w);
    sp->Set("DLSS.Render.Subrect.Dimensions.Height", h);
    sp->Set("DLSS.Input.Color.Subrect.Base.X", 0u);
    sp->Set("DLSS.Input.Color.Subrect.Base.Y", 0u);
    sp->Set("DLSS.Input.Depth.Subrect.Base.X", 0u);
    sp->Set("DLSS.Input.Depth.Subrect.Base.Y", 0u);
    sp->Set("DLSS.Input.MV.Subrect.Base.X", 0u);
    sp->Set("DLSS.Input.MV.Subrect.Base.Y", 0u);
    sp->Set("DLSS.Output.Subrect.Base.X", 0u);
    sp->Set("DLSS.Output.Subrect.Base.Y", 0u);

    // Every provider writes UV-space motion, so the magnitude that turns one into
    // pixels is the back buffer's own size -- measured, not assumed.
    //
    // The sign is a config value where the provider does not state one, and the
    // provider's own documented convention where it does. Only vort_Shaders does:
    // it is MIT, so its direction was read out of its source rather than guessed
    // (kSynthMv). An explicit mv_sign_x/mv_sign_y still wins, because a real
    // installation is where a documented convention gets contradicted.
    const int sign_x = g_cfg.mv_sign_x != 0 ? g_cfg.mv_sign_x
                     : (which >= 0 && kSynthMv[which].sign_x != 0 ? kSynthMv[which].sign_x : 1);
    const int sign_y = g_cfg.mv_sign_y != 0 ? g_cfg.mv_sign_y
                     : (which >= 0 && kSynthMv[which].sign_y != 0 ? kSynthMv[which].sign_y : 1);
    sp->Set("MV.Scale.X", static_cast<float>(sign_x) * static_cast<float>(w));
    sp->Set("MV.Scale.Y", static_cast<float>(sign_y) * static_cast<float>(h));

    // Zero, and this is a copied fact rather than a shortcut. ReShade's first
    // opportunity to see a frame is after the game has rasterised it, so this
    // add-on cannot move the projection matrix and the game is not moving it
    // either: every frame samples the same sub-pixel grid. A jitter this block
    // reported would be a jitter nothing applied, and DLSS would resolve its
    // history against a sub-pixel offset that never happened.
    sp->Set("Jitter.Offset.X", 0.0f);
    sp->Set("Jitter.Offset.Y", 0.0f);

    // DoSharpening is not set above, so DLSS runs no sharpening pass and this
    // value is not read. It is still stated, because the note requires every key
    // the frame path reads to be set by whatever built the block.
    sp->Set("Sharpness", 0.0f);

    // No exposure texture and no AutoExposure, so both of these are the identity
    // that nvsdk_ngx_helpers.h sends for a source with nothing to say about
    // exposure -- which is this source exactly.
    sp->Set("DLSS.Pre.Exposure",   1.0f);
    sp->Set("DLSS.Exposure.Scale", 1.0f);

    // The camera cut this source can detect is none: ReShade publishes no such
    // signal and a cut inferred from the depth buffer would be a guess. Zero is
    // the measured answer, and BridgeFrameInner adds its own need_reset on top
    // of it whenever the shape changed underneath.
    sp->Set("Reset", 0);
}

// ---------------------------------------------------------------------------
// The D3D12 transport
//
// Same contract, same self-check, same arming gate. The only new thing is how
// the game's frame reaches an NGX evaluate and how the result gets back.
//
// It runs on the GAME'S OWN D3D12 device, not on a private one. The private
// device in bridge.inc is not a preference: a D3D11 game has no D3D12 device at
// all, so one has to be conjured, and everything downstream of that -- four
// shared textures with NT handles, a D3D12_FENCE_FLAG_SHARED fence opened twice,
// the R24G8 depth conversion and the R32G32 motion-vector retry, both of which
// exist only because the kernel's shared-format list is narrower than DXGI --
// is machinery for crossing between two devices. A D3D12 game already has the
// device the evaluate needs, so none of that machinery has anything to do. What
// is left is two textures and two copies.
//
// UNVERIFIED, and assumed rather than measured: whether NGX will hold two
// D3D12 sessions on one ID3D12Device. Only the D3D11 NGX entry points are
// hooked, so g_create_count and g_eval_count cannot see a D3D12 game's own DLSS
// and the arming gate cannot refuse on it. Nothing here can establish it either
// -- so it is stated in the log at the moment the session opens, where a report
// carries it, rather than left as a silent premise. The intended target is a
// game that ships no DLSS of its own, which has no first session.
//
// UNVERIFIED, and resolved rather than assumed: whether ReShade's get_native
// hands back its own D3D12 proxy or the device underneath. UnwrapReShadeProxy
// answers that by asking, with the GUID ReShade answers, and says which branch
// it took -- so both cases are correct and the log records which one happened.
// ---------------------------------------------------------------------------

// One past the slots. The exposure mirror is a fifth texture and deliberately
// NOT a fifth slot, for the reason bridge.h gives beside exp12: every
// "i < SLOT_COUNT" loop dereferences its entry unconditionally, and the games
// that supply no exposure texture are the common case. Every loop below stops at
// SLOT_COUNT except the two that name this constant.
static const int kExpTex = SLOT_COUNT;

// Two past the slots: a sixth texture and deliberately not a slot, for exactly
// the reason above. It is the second Output the pipelined Vulkan transport
// alternates with tex[SLOT_OUTPUT], so the
// worker can write one frame's result while the game reads the previous one out
// of the other. It exists on one transport, in one mode -- vk_sync=2 -- and is
// null everywhere else, which is one more reason not to make every "i <
// SLOT_COUNT" loop dereference it. See "The pipeline" above SynthVkFrame.
static const int kOutB = SLOT_COUNT + 1;

struct Synth12
{
    bool tried;      // the session open was attempted; a failure is never retried
    bool ready;      // device, NGX session, queue, allocator ring
    bool resources_retained;

    // The device is this add-on's own rather than the game's, so all four
    // textures have to be created shareable and the frame has to be carried
    // across. True on the Vulkan transport and nowhere else.
    bool shared;

    ID3D12Device              *dev;
    ID3D12CommandQueue        *queue;   // ReShade's own, so ordering is queue order
    ID3D12GraphicsCommandList *list;

    // Same reason as the mirror's ring: an allocator must not be reset while the
    // GPU is still executing what was recorded in it.
    static const int           kFrames = 3;
    ID3D12CommandAllocator    *alloc[kFrames];
    UINT64                     alloc_fence[kFrames];
    int                        slot;
    ID3D12Fence               *fence;
    UINT64                     fence_value;
    HANDLE                     event;

    PFN_D3D12CreateFeature   create_feature;
    PFN_D3D12EvaluateFeature eval_feature;
    PFN_D3D12ReleaseFeature  release_feature;
    PFN_AllocateParameters   alloc_params;

    NVSDK_NGX_Parameter *params;
    NVSDK_NGX_Handle    *feature;

    // Colour in and colour out cannot be the same texture: NGX reads one as a
    // shader resource and writes the other through a UAV in the same evaluate.
    // The back buffer is neither, so it is copied in and copied back out.
    //
    // Indexed by the mirror's own SLOT_ constants, because the shared case below
    // needs all four of them and a second set of names for the same four slots
    // is a second thing to keep in step. On the game's own D3D12 device only
    // SLOT_COLOR and SLOT_OUTPUT are filled: Depth and MotionVectors are the
    // game's resources and are handed to NGX untouched.
    ID3D12Resource *tex[SLOT_COUNT + 2];
    // Only when shared: the NT handle the other API imports the texture through.
    HANDLE          share[SLOT_COUNT + 2];

    // Whether this build made the second Output at kOutB. Read as "the shape
    // this session is built for is the pipelined one", which is what the rebuild
    // test in SynthVkFrame compares against so that editing vk_sync between two
    // frames goes down the existing rebuild path rather than half-changing the
    // transport under a frame in flight.
    bool            out_b;

    // The last build failed on the second Output ALONE, which is the one
    // allocation in this file that is optional: the park delivers every frame the
    // pipeline does, a present earlier. Read and cleared by SynthVkFrame, which
    // drops to the park instead of counting a build failure -- three of those
    // latch g_synth_build_dead and end the contract for the session, which is not
    // what losing an opt-in texture is worth.
    bool            out_b_fail;

    // Consecutive failed builds. A build that fails is "this attempt failed", not
    // "this session is unusable" -- Synth12Build already nulls the feature and
    // gives the textures back, so the rebuild guard tries again on its own. Only
    // a run of them means the transport is really gone.
    int             build_fails;

    // The game's own exposure texture, mirrored at index kExpTex. Zero here means
    // this session carries none, which is every synthetic session and every game
    // that supplies no ExposureTexture. Its shape is independent of the render
    // shape, so it is remembered rather than derived from w/h.
    UINT            exp_w, exp_h;
    DXGI_FORMAT     exp_fmt, out_fmt;

    // The Vulkan mirror's depth arrives packed rather than as floats when the
    // game's depth is D24_UNORM_S8_UINT -- see Synth12MakeDepthUnpack. Then
    // tex[SLOT_DEPTH] is R32_UINT, these four exist, and NGX is handed depth_cvt
    // instead. On every other path they are null and nothing below runs.
    bool                  depth_packed;
    ID3D12Resource       *depth_cvt;
    ID3D12RootSignature  *cvt_rs;
    ID3D12PipelineState  *cvt_pso;
    ID3D12DescriptorHeap *cvt_heap;

    // The HDR10 pair, built for a synthetic contract on a PQ presentation;
    // see PqBackBuffer in bridge.inc. pq.in is null otherwise.
    PqPass                pq;

    UINT            w, h;
    // The Output slot's own size. On the synthetic path it is always w,h -- that
    // contract is DLAA by construction -- but the Vulkan mirror copies a real
    // game's contract, where an upscaling feature declares OutWidth/OutHeight
    // larger than Width/Height and Output has to be that size or NGX refuses.
    UINT            out_w, out_h;
    DXGI_FORMAT     fmt;
    // How long a GPU wait is given used to live here, as a global one transport
    // set and another read. It is an argument now -- Synth12Begin,
    // Synth12EvaluateShared and SynthOfaVkFrame each take it -- because who is
    // waiting is what decides what a safe wait is, and a field cannot say that:
    // the mirror's 900 stayed standing for the D3D11/D3D12 transport, which parks
    // nothing and wants the full four seconds, in any process where both run.
    bool            need_reset;
    UINT64          frames;
};

static Synth12 g_s12;

// A UAV cannot be created over an sRGB format, and the DLSS output is written
// through one. The sRGB and UNORM spellings are the same typeless family, so
// CopyResource still moves the result back into the back buffer untouched --
// the encoding is reinterpreted, not converted, which is what the back buffer's
// own render target view does to it anyway.
static DXGI_FORMAT Synth12UavFormat(DXGI_FORMAT f)
{
    switch (TypedFormat(f))
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
    default:                              return TypedFormat(f);
    }
}

// The contract arrives complete -- constraint (a) -- so there is no fallback
// here and none is wanted: a Get that fails is a defect SynthSelfCheck has
// already reported by name, and inventing a value on top of it would hide it.
static void Synth12CopyU(const NVSDK_NGX_Parameter *src, NVSDK_NGX_Parameter *dst,
                         const char *key)
{
    unsigned int v = 0;
    if (src->Get(key, &v) == NGX_SUCCESS) dst->Set(key, v);
}

static void Synth12CopyF(const NVSDK_NGX_Parameter *src, NVSDK_NGX_Parameter *dst,
                         const char *key)
{
    float v = 0.0f;
    if (src->Get(key, &v) == NGX_SUCCESS) dst->Set(key, v);
}

// One auto-reset event serves every fence wait on this device, and
// SetEventOnCompletion cannot be cancelled -- so a wait that timed out leaves a
// live registration behind, and when the fence finally reaches that value the
// event is signalled with nobody waiting. The NEXT wait then returns at once,
// for a value the GPU has not reached, and whatever that wait was protecting
// runs anyway. bridge.inc states this rule and tests the fence as well as the
// event; this side did not, in five places, one of them the colour copy the
// optical-flow engine reads.
//
// The event is the wake; the fence is the answer.
static bool Synth12WaitFence(UINT64 value, DWORD ms)
{
    if (g_s12.fence == nullptr || g_s12.event == nullptr) return false;
    const UINT64 completed = g_s12.fence->GetCompletedValue();
    if (completed == UINT64_MAX) return false;
    if (completed >= value) return true;
    if (FAILED(g_s12.fence->SetEventOnCompletion(value, g_s12.event))) return false;
    if (WaitForSingleObject(g_s12.event, ms) != WAIT_OBJECT_0) return false;
    const UINT64 after = g_s12.fence->GetCompletedValue();
    return after != UINT64_MAX && after >= value;
}

static bool Synth12Drain(DWORD wait_ms = 4000)
{
    if (g_s12.resources_retained) return false;
    if ((g_s12.fence != nullptr && !Synth12WaitFence(g_s12.fence_value, wait_ms)) ||
        !PqPassWait(&g_s12.pq, g_s12.pq.fv, wait_ms)) {
        g_s12.resources_retained = true;
        g_s12.ready = false;
        Log("[synth] GPU completion unproven; retaining the private feature, images and colour-pass resources.");
        return false;
    }
    return true;
}

// D3D12 keeps no reference to the resources a submitted command list names, so
// the last frame has to be finished with before any of them is freed. This runs
// on a resolution change, which is rare enough that a full wait costs nothing
// and a use-after-free costs a GPU reset.
static bool Synth12ReleaseTextures()
{
    if (!Synth12Drain()) return false;
    if (g_s12.feature != nullptr && g_s12.release_feature != nullptr)
        g_s12.release_feature(g_s12.feature);
    g_s12.feature = nullptr;
    // The exposure key, cleared BEFORE its texture is freed and after the drain
    // above. bridge.inc states the rule beside BridgeReleaseExposure: NGX does
    // not AddRef what a parameter block names, this block is allocated once per
    // session and never Reset, so freeing the texture without clearing the key
    // leaves the driver a freed pointer on the very next evaluate.
    //
    // ONLY when this session actually set it. Clearing unconditionally put an
    // ExposureTexture key, valued null, into the block handed to CreateFeature in
    // every session including those that carry none -- a key the block had never
    // carried before 2026-09-01. Whether NGX minds is not established; that it is
    // a change nobody needed is, so it is not made.
    if (g_s12.params != nullptr && g_s12.tex[kExpTex] != nullptr)
        g_s12.params->Set("ExposureTexture", static_cast<ID3D12Resource *>(nullptr));
    g_s12.exp_w = g_s12.exp_h = 0;
    g_s12.exp_fmt = g_s12.out_fmt = DXGI_FORMAT_UNKNOWN;

    // The same rule, and the other four keys it was never applied to. Every
    // evaluate writes Color, Output, Depth and MotionVectors into this block; the
    // block is allocated once per session and never Reset, so once the textures
    // below are Released it still names four freed ID3D12Resources -- and the
    // next call handed that block is CreateFeature, on a rebuild. "Output" can
    // be naming a fifth texture, tex[kOutB], because the pipelined transport
    // hands the evaluate whichever of the two the frame it armed owns; the loop
    // below frees that one too and this clear covers it either way.
    //
    // Whatever inspects it then reads freed memory. On this machine that is
    // renodx-dlss5, which detours NVSDK_NGX_D3D12_CreateFeature and faulted
    // 0xC0000005 at +0xD006 in roughly one rebuild in three -- intermittent
    // exactly as a read of a freed allocation is, and never on the first build,
    // when the block is still empty.
    if (g_s12.params != nullptr)
    {
        g_s12.params->Set("Color",         static_cast<ID3D12Resource *>(nullptr));
        g_s12.params->Set("Output",        static_cast<ID3D12Resource *>(nullptr));
        g_s12.params->Set("Depth",         static_cast<ID3D12Resource *>(nullptr));
        g_s12.params->Set("MotionVectors", static_cast<ID3D12Resource *>(nullptr));
    }
    for (int i = 0; i < SLOT_COUNT + 2; ++i)
    {
        if (g_s12.tex[i]   != nullptr) { g_s12.tex[i]->Release(); g_s12.tex[i] = nullptr; }
        // The handle is the importing API's only route to the texture, and the
        // driver duplicated it at import time rather than taking ownership, so
        // this side still owns it and still has to close it.
        if (g_s12.share[i] != nullptr) { CloseHandle(g_s12.share[i]); g_s12.share[i] = nullptr; }
    }
    g_s12.out_b = false;

    // The depth unpack pass. The heap holds views of the two textures being freed
    // above and below, so it cannot outlive them; the root signature and the PSO
    // could, but rebuilding all four together is one lifetime to reason about
    // instead of two, and a resolution change is the only thing that gets here.
    if (g_s12.depth_cvt != nullptr) { g_s12.depth_cvt->Release(); g_s12.depth_cvt = nullptr; }
    if (g_s12.cvt_heap  != nullptr) { g_s12.cvt_heap->Release();  g_s12.cvt_heap  = nullptr; }
    if (g_s12.cvt_pso   != nullptr) { g_s12.cvt_pso->Release();   g_s12.cvt_pso   = nullptr; }
    if (g_s12.cvt_rs    != nullptr) { g_s12.cvt_rs->Release();    g_s12.cvt_rs    = nullptr; }
    return PqPassRelease(&g_s12.pq);
}

// wait_ms is how long the command allocator's own retire fence is waited for,
// and it is the caller's because this wait is inside whatever the caller has
// stopped. Four seconds is right on the D3D11/D3D12 path, where nothing is
// parked behind it; under either Vulkan park it is twice the TDR window, so
// those two callers pass their own budget down.
static bool Synth12Begin(DWORD wait_ms = 4000)
{
    const int slot = g_s12.slot;
    const UINT64 retire = g_s12.alloc_fence[slot];
    if (retire != 0 && g_s12.fence->GetCompletedValue() < retire)
    {
        if (!Synth12WaitFence(retire, wait_ms)) return false;
    }
    if (FAILED(g_s12.alloc[slot]->Reset())) return false;
    if (FAILED(g_s12.list->Reset(g_s12.alloc[slot], nullptr))) return false;
    return true;
}

// Submitted on ReShade's own queue, which is what removes the fence entirely.
// Two lists on one queue execute in submission order, so ReShade's recorded work
// runs before this and everything it records afterwards runs after it. The flush
// in Synth12Frame is what puts ReShade's half of that into the queue first;
// without it its commands are still sitting in an open list.
static UINT64 Synth12End()
{
    g_s12.list->Close();
    ID3D12CommandList *lists[] = { g_s12.list };
    g_s12.queue->ExecuteCommandLists(1, lists);

    const UINT64 v = ++g_s12.fence_value;
    g_s12.queue->Signal(g_s12.fence, v);
    g_s12.alloc_fence[g_s12.slot] = v;
    g_s12.slot = (g_s12.slot + 1) % Synth12::kFrames;
    return v;
}

static bool Synth12Open(reshade::api::effect_runtime *rt)
{
    if (g_s12.tried) return g_s12.ready;
    g_s12.tried = true;

    reshade::api::device *rdev = rt->get_device();
    // Whose device this session ends up on is decided here and read everywhere
    // below, so no line further down can claim the wrong one.
    g_s12.shared = rdev != nullptr &&
                   rdev->get_api() == reshade::api::device_api::vulkan;
    const char *whose = g_s12.shared ? "this add-on's own D3D12 device"
                                     : "the game's D3D12 device";

    Log("");
    Breadcrumb("opening the D3D12 synthetic session");
    Log("################ synth: opening on %s ################", whose);

    if (g_s12.shared)
    {
        // A Vulkan game has no D3D12 device, so one is conjured for exactly the
        // reason the D3D11 mirror conjures one: the evaluate a DLSS 5 add-on
        // detours is NVSDK_NGX_D3D12_EvaluateFeature, and there is no Vulkan
        // entry point to offer it instead.
        //
        // ponytail: the default adapter. ReShade exposes no VkPhysicalDevice --
        // it keeps _physical_device private and get_native answers for the
        // device, the queue and the swapchain only -- so there is no LUID to
        // match the game's GPU against. On a machine with one GPU that is the
        // right adapter; on a hybrid laptop the memory import below fails and
        // says so, which is the diagnosis. Matching by LUID needs a
        // VkPhysicalDevice, and getting one needs the vkCreateDevice hook this
        // transport was written to avoid.
        HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
        auto create_device = d3d12 != nullptr
            ? reinterpret_cast<PFN_D3D12CreateDevice_>(GetProcAddress(d3d12, "D3D12CreateDevice"))
            : nullptr;
        if (create_device == nullptr)
        {
            Log("[synth] d3d12.dll is not present or exports no D3D12CreateDevice, so there "
                "is no device to run an evaluate on. The game is unaffected.");
            return false;
        }

        DWORD dcode = 0;
        // SafeCreateDevice is the mirror's own, guard and unwrap included: a
        // fault inside the D3D12 runtime comes back as a code rather than as a
        // dead process, which is the whole reason it exists.
        HRESULT hr = SafeCreateDevice(create_device, nullptr, &g_s12.dev, &dcode);
        if (dcode != 0)
        {
            Log("[synth] D3D12CreateDevice raised 0x%08X inside the D3D12 runtime, so "
                "nothing further is attempted. The game keeps rendering with Vulkan.", dcode);
            return false;
        }
        if (FAILED(hr) || g_s12.dev == nullptr)
        { Log("[synth] D3D12CreateDevice failed 0x%08X", hr); return false; }

        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        g_s12.dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue),
                                      reinterpret_cast<void **>(&g_s12.queue));
        if (g_s12.queue == nullptr)
        { Log("[synth] the private D3D12 device would not create a graphics queue"); return false; }
    }
    else
    {
        reshade::api::command_queue *rq = rt->get_command_queue();
        g_s12.dev = rdev != nullptr
            ? reinterpret_cast<ID3D12Device *>(static_cast<uintptr_t>(rdev->get_native())) : nullptr;
        g_s12.queue = rq != nullptr
            ? reinterpret_cast<ID3D12CommandQueue *>(static_cast<uintptr_t>(rq->get_native())) : nullptr;
        if (g_s12.dev == nullptr || g_s12.queue == nullptr)
        {
            Log("[synth] the effect runtime reports no native D3D12 device or queue, so there "
                "is nothing to run an evaluate on. The game is unaffected.");
            return false;
        }
    }
    Log("[synth] %s is %p and its graphics queue is %p", whose,
        static_cast<void *>(g_s12.dev), static_cast<void *>(g_s12.queue));

    // The same question the mirror asks of the device it creates, asked here of
    // the device it was handed. RenoDX asks it of every command list before
    // forwarding to NGX, so an unwrapped device is the case NGX callers prefer,
    // and the answer is logged either way rather than presumed. SafeCreateDevice
    // has already asked it on the shared path, so this is the other one.
    // unwrap is honoured here for the same reason SafeCreateDevice honours it:
    // it is the knob a user is told to turn when unwrapping is what breaks their
    // driver, and a knob that only reaches one of two devices is worse than no
    // knob at all.
    if (!g_s12.shared) MaybeUnwrap(&g_s12.dev);

    // The scan first, because a game that has DLSS has already loaded NGX and the
    // module it loaded is the one to use. Only when nothing is there is the
    // driver's own loader brought in -- which is the ordinary case here, since
    // the synthetic contract exists for games that ship no DLSS at all.
    HMODULE ngx = FindNgxLoader();
    if (ngx == nullptr && EnsureNgxLoaderLoaded()) ngx = FindNgxLoader();
    if (ngx == nullptr)
    {
        Log("[synth] no module exports the NGX D3D12 entry points, and the driver's own "
            "loader could not be brought in either, so there is no NGX to run a feature on.");
        return false;
    }

    auto init_ext = reinterpret_cast<PFN_Init_Ext>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_Ext"));
    auto init_project = reinterpret_cast<PFN_Init_ProjectID>(
        GetProcAddress(ngx, "NVSDK_NGX_D3D12_Init_ProjectID"));
    g_s12.alloc_params = reinterpret_cast<PFN_AllocateParameters>(
        GetProcAddress(ngx, "NVSDK_NGX_D3D12_AllocateParameters"));
    g_s12.create_feature = reinterpret_cast<PFN_D3D12CreateFeature>(
        GetProcAddress(ngx, "NVSDK_NGX_D3D12_CreateFeature"));
    g_s12.eval_feature = reinterpret_cast<PFN_D3D12EvaluateFeature>(
        GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature"));
    // Not required: a driver that exports no ReleaseFeature costs a leaked
    // feature on a resolution change, which is not a reason to refuse a session.
    g_s12.release_feature = reinterpret_cast<PFN_D3D12ReleaseFeature>(
        GetProcAddress(ngx, "NVSDK_NGX_D3D12_ReleaseFeature"));
    if ((init_ext == nullptr && init_project == nullptr) || g_s12.alloc_params == nullptr ||
        g_s12.create_feature == nullptr || g_s12.eval_feature == nullptr)
    { Log("[synth] required NGX D3D12 exports missing"); return false; }

    // Said before the first call it could affect, so a log that ends inside NGX
    // still carries the premise the session was opened on. Only where it is the
    // premise: a device this add-on created a moment ago cannot be carrying
    // somebody else's session, so on the shared path there is nothing assumed.
    if (!g_s12.shared)
        Log("[synth] ASSUMED, NOT MEASURED: that NGX accepts a second D3D12 session on a "
            "device the game may already have one on. Only the D3D11 NGX entry points are "
            "hooked here, so a D3D12 game's own DLSS is invisible to the arming gate and "
            "cannot be refused on. This path is meant for a game that ships no DLSS, which "
            "has no first session; if this game does ship DLSS, that premise is untested.");

    wchar_t exe_dir[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_dir, MAX_PATH);
    if (wchar_t *ed = wcsrchr(exe_dir, (wchar_t)92)) *(ed + 1) = 0;
    Log("[synth] NGX data path: %ls", exe_dir);

    g_ngx_init_in_flight = true;
    Breadcrumb(g_s12.shared ? "initialising NGX on the private D3D12 device"
                            : "initialising NGX on the game's D3D12 device");
    DWORD code = 0;
    bool inited = false, faulted = false;
    // Same negotiation and the same rule as the mirror's: a refusal is NGX
    // declining a version constant and the next one is tried; a fault means NGX
    // died inside itself, and calling back in is what turned a disabled add-on
    // into a killed process on Final Fantasy VII Remake. Stop at the first one.
    for (int ver = 0x13; ver <= 0x16 && !inited && !faulted; ++ver)
    {
        if (init_ext != nullptr)
        {
            NVSDK_NGX_Result r = SafeInitExt(init_ext, 0x1000000ULL, exe_dir, g_s12.dev, ver, &code);
            if (code != 0)
            { Log("[synth] Init_Ext(version=0x%02X) raised 0x%08X", ver, code); faulted = true; }
            else
            {
                Log("[synth] Init_Ext(version=0x%02X) -> %s", ver, r == NGX_SUCCESS ? "ok" : "refused");
                if (r == NGX_SUCCESS) inited = true;
            }
        }
        if (!inited && !faulted && init_project != nullptr)
        {
            NVSDK_NGX_Result r = SafeInitProjectID(init_project,
                                                   "a0f57b54-1daf-4934-90ae-c4035c19df04",
                                                   exe_dir, g_s12.dev, ver, &code);
            if (code != 0)
            { Log("[synth] Init_ProjectID(version=0x%02X) raised 0x%08X", ver, code); faulted = true; }
            else
            {
                Log("[synth] Init_ProjectID(version=0x%02X) -> %s", ver, r == NGX_SUCCESS ? "ok" : "refused");
                if (r == NGX_SUCCESS) inited = true;
            }
        }
    }
    g_ngx_init_in_flight = false;

    if (faulted)
    {
        Log("[synth] NGX raised an exception opening a D3D12 session on %s, so nothing "
            "further is attempted. The game keeps rendering.", whose);
        return false;
    }
    if (!inited)
    { Log("[synth] no NGX D3D12 init entry point was accepted by this driver"); return false; }

    NVSDK_NGX_Result rp = SafeAllocParams(g_s12.alloc_params, &g_s12.params, &code);
    if (code != 0 || rp != NGX_SUCCESS || g_s12.params == nullptr)
    {
        Log("[synth] NGX would not allocate a parameter block: result 0x%08X, raised 0x%08X",
            rp, code);
        return false;
    }

    for (int i = 0; i < Synth12::kFrames; ++i)
        g_s12.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          __uuidof(ID3D12CommandAllocator),
                                          reinterpret_cast<void **>(&g_s12.alloc[i]));
    if (g_s12.alloc[0] != nullptr)
        g_s12.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_s12.alloc[0], nullptr,
                                     __uuidof(ID3D12GraphicsCommandList),
                                     reinterpret_cast<void **>(&g_s12.list));
    if (g_s12.list != nullptr) g_s12.list->Close();
    g_s12.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                           reinterpret_cast<void **>(&g_s12.fence));
    g_s12.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_s12.list == nullptr || g_s12.fence == nullptr || g_s12.event == nullptr)
    { Log("[synth] could not create the command list, fence or event"); return false; }

    // The list is this add-on's own and not ReShade's, and that is not a
    // preference either. ReShade's command_list_impl caches the root signature
    // and the descriptor heaps it last set and skips re-setting them when they
    // have not changed; NGX sets both directly on the underlying list, which
    // that cache cannot see. Recording the evaluate into ReShade's list would
    // leave its next effect pass running under NGX's root signature, silently.
    Log("[synth] D3D12 session ready on %s. %s", whose, g_s12.shared
        ? "The frame does not start on this device, so every texture NGX reads is "
          "created shareable here and imported by the API the game renders with."
        : "Nothing is shared between devices, so there is no NT handle, no "
          "cross-API fence and no format conversion.");
    g_s12.ready = true;
    return true;
}

// One texture NGX will read or write. On the game's own D3D12 device that is an
// ordinary committed resource; on the shared path it also needs an NT handle,
// and ALLOW_SIMULTANEOUS_ACCESS with it -- the importing API performs no D3D12
// state transitions, so the texture has to be usable while it sits in COMMON.
// That is the same pair of flags the mirror's own shared textures carry, for the
// same reason.
static bool Synth12MakeTexture(int slot, UINT w, UINT h, DXGI_FORMAT fmt, bool uav)
{
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
    rd.Flags            = (uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                               : D3D12_RESOURCE_FLAG_NONE) |
                          (g_s12.shared ? D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS
                                        : D3D12_RESOURCE_FLAG_NONE);

    HRESULT hr = g_s12.dev->CreateCommittedResource(&hp,
        g_s12.shared ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource),
        reinterpret_cast<void **>(&g_s12.tex[slot]));
    if (SUCCEEDED(hr) && g_s12.shared)
        hr = g_s12.dev->CreateSharedHandle(g_s12.tex[slot], nullptr, GENERIC_ALL, nullptr,
                                           &g_s12.share[slot]);
    if (FAILED(hr))
    {
        Log("[synth] could not create%s the %ux%u %s texture for %s: 0x%08X", 
            g_s12.shared ? " or share" : "", w, h, FormatName(fmt),
            slot < SLOT_COUNT ? kSlotName[slot] : slot == kExpTex ? "Exposure" : "Output B", hr);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Unpacking a D24 depth aspect, on the D3D12 side
//
// The Vulkan mirror carries the game's depth aspect out through a buffer and
// back into the shared texture. For VK_FORMAT_D24_UNORM_S8_UINT the Vulkan
// specification says exactly what those bytes are, so nothing here is a guess
// about how a driver packs them -- there is no freedom left to guess about:
//
//   copies.adoc, "Depth/Stencil Aspect Copy": when copying between a buffer and
//   the depth or stencil aspect of an image the data in the buffer is laid out
//   as separate planes, addressed as a different format according to the aspect,
//   and the depth aspect of VK_FORMAT_D24_UNORM_S8_UINT is addressed as
//   VK_FORMAT_X8_D24_UNORM_PACK32.
//
//   formats.adoc: that is a 32-bit packed format, first component named in the
//   most significant bits, so X is bits 31..24 and D is bits 23..0, in host byte
//   order -- little-endian on x86, which is the same order the D3D12 GPU reads
//   the word back in.
//
//   fundamentals.adoc: an unsigned normalised value of b bits is c / (2^b - 1),
//   so 24 bits is c / 16777215.
//
// So the shared texture is declared R32_UINT, the transport is untouched -- the
// depth aspect is four bytes per texel exactly as D32_SFLOAT is, so the staging
// buffer, its size and both copies stay as they are -- and this pass turns those
// words into the R32_FLOAT depth NGX reads.
//
// The mask is mandatory rather than tidy: the top 8 bits carry no defined value
// at all, and unmasked they scale depth by up to 256, which puts every
// reprojection on noise.
//
// The divide is written as a divide. float(c) is exact for c <= 2^24-1 and a
// divide is correctly rounded, where the reciprocal-multiply an optimiser folds
// in can land a ULP away from the hardware UNORM conversion the game's own DLSS
// sees. At 8x8 it costs nothing.
//
// The "opaque" clause in formats.adoc is not a counter-argument: it governs how
// the IMAGE is stored, which is tiled memory nothing here touches, not the
// buffer payload of a copy, which the copies chapter pins.
static bool Synth12MakeDepthUnpack(UINT w, UINT h)
{
    // The one texture on this device the other API never sees: not shared, no NT
    // handle, not in the import loop. NGX reads it and nothing else touches it,
    // which is why it can carry ALLOW_UNORDERED_ACCESS without also carrying
    // ALLOW_SIMULTANEOUS_ACCESS.
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
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    HRESULT hr = g_s12.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource),
        reinterpret_cast<void **>(&g_s12.depth_cvt));
    if (FAILED(hr))
    {
        Log("[synth] the %ux%u R32_FLOAT texture the depth unpack writes into could not "
            "be created: 0x%08X", w, h, hr);
        return false;
    }

    // The same skeleton as the D3D11 depth blit in bridge.inc -- same numthreads,
    // same bounds guard -- with the two view types and the one expression that
    // the packing needs. The D3D11 path never needs the arithmetic because it
    // reads the game's depth through an R24_UNORM_X8_TYPELESS view and the
    // hardware does the divide; there is no such view over an R32_UINT texture,
    // because R24_UNORM_X8_TYPELESS belongs to the R24G8 family and not to R32.
    static const char kSrc[] =
        "Texture2D<uint>    src : register(t0);\n"
        "RWTexture2D<float> dst : register(u0);\n"
        "[numthreads(8,8,1)]\n"
        "void main(uint3 id : SV_DispatchThreadID)\n"
        "{\n"
        "    uint w, h; dst.GetDimensions(w, h);\n"
        "    if (id.x < w && id.y < h)\n"
        "        dst[id.xy] = (src[id.xy] & 0x00FFFFFFu) / 16777215.0;\n"
        "}\n";

    PFN_D3DCompile_ compile = LoadSystemD3DCompiler();
    if (compile == nullptr) { Log("[synth] d3dcompiler_47.dll unavailable"); return false; }

    // D3DCOMPILE_IEEE_STRICTNESS, spelled out because d3dcompiler.h is not
    // included here -- this add-on declares the one entry point it uses and
    // nothing else. Without it fxc folds the divide into a multiply by the
    // rounded reciprocal of 16777215, which can land a ULP off the hardware
    // UNORM conversion; with it the DXBC carries a real div. Verified by
    // compiling this exact source with fxc /Gis and reading the disassembly.
    static const UINT kIeeeStrictness = 1u << 13;

    ID3DBlob *code = nullptr, *err = nullptr;
    hr = compile(kSrc, sizeof(kSrc) - 1, "depthunpack", nullptr, nullptr,
                 "main", "cs_5_0", kIeeeStrictness, 0, &code, &err);
    if (FAILED(hr) || code == nullptr)
    {
        Log("[synth] the depth unpack shader would not compile: %s",
            err != nullptr ? static_cast<const char *>(err->GetBufferPointer()) : "");
        if (err != nullptr) err->Release();
        return false;
    }
    if (err != nullptr) err->Release();

    // One table, two ranges: t0 is the packed depth, u0 is the float it becomes.
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors                    = 1;
    ranges[0].BaseShaderRegister                = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors                    = 1;
    ranges[1].BaseShaderRegister                = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;

    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 2;
    param.DescriptorTable.pDescriptorRanges   = ranges;
    param.ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 1;
    rsd.pParameters   = &param;
    rsd.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    // d3d12.dll is already loaded -- Synth12Open took it -- so this asks the
    // module rather than taking a second reference, and it stays a GetProcAddress
    // rather than a static import for the same reason everything else here is:
    // the add-on has to load on a machine with no D3D12 at all.
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    auto serialize = d3d12 != nullptr
        ? reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(
              GetProcAddress(d3d12, "D3D12SerializeRootSignature"))
        : nullptr;
    if (serialize == nullptr)
    {
        Log("[synth] d3d12.dll exports no D3D12SerializeRootSignature, so the depth unpack "
            "has no root signature");
        code->Release();
        return false;
    }

    ID3DBlob *rsblob = nullptr, *rserr = nullptr;
    hr = serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsblob, &rserr);
    if (FAILED(hr) || rsblob == nullptr)
    {
        Log("[synth] the depth unpack root signature would not serialize 0x%08X: %s", hr,
            rserr != nullptr ? static_cast<const char *>(rserr->GetBufferPointer()) : "");
        if (rserr != nullptr) rserr->Release();
        code->Release();
        return false;
    }
    if (rserr != nullptr) rserr->Release();

    hr = g_s12.dev->CreateRootSignature(0, rsblob->GetBufferPointer(), rsblob->GetBufferSize(),
                                        __uuidof(ID3D12RootSignature),
                                        reinterpret_cast<void **>(&g_s12.cvt_rs));
    rsblob->Release();
    if (FAILED(hr))
    {
        Log("[synth] CreateRootSignature failed 0x%08X", hr);
        code->Release();
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature     = g_s12.cvt_rs;
    pd.CS.pShaderBytecode = code->GetBufferPointer();
    pd.CS.BytecodeLength  = code->GetBufferSize();
    hr = g_s12.dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState),
                                               reinterpret_cast<void **>(&g_s12.cvt_pso));
    code->Release();
    if (FAILED(hr)) { Log("[synth] CreateComputePipelineState failed 0x%08X", hr); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = g_s12.dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap),
                                         reinterpret_cast<void **>(&g_s12.cvt_heap));
    if (FAILED(hr)) { Log("[synth] CreateDescriptorHeap failed 0x%08X", hr); return false; }

    // Written once, here, because both textures live exactly as long as the heap
    // does -- Synth12ReleaseTextures frees all three together.
    const UINT inc = g_s12.dev->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_s12.cvt_heap->GetCPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format                    = DXGI_FORMAT_R32_UINT;
    sd.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels       = 1;
    g_s12.dev->CreateShaderResourceView(g_s12.tex[SLOT_DEPTH], &sd, cpu);

    cpu.ptr += inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format        = DXGI_FORMAT_R32_FLOAT;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_s12.dev->CreateUnorderedAccessView(g_s12.depth_cvt, nullptr, &ud, cpu);

    Log("[synth] depth unpack shader ready (cs_5_0)");
    return true;
}

// Creating the feature. Every create key comes off the synthetic contract rather
// than off anything measured a second time here: SynthBuild set them and
// SynthSelfCheck proved they are there, and reading them back through the block
// is what makes that proof cover this path too.
// out_w/out_h size the Output slot alone. The synthetic callers pass w,h -- their
// contract is 1:1 by construction -- and the Vulkan mirror passes the game's own
// OutWidth/OutHeight, which is larger whenever the game is upscaling.
// depth_packed defaults false, so the two synthetic call sites are untouched and
// keep the R32_FLOAT depth slot they have always had. Only the Vulkan mirror
// passes true, and only when the game's depth is D24_UNORM_S8_UINT.
// What a failed build costs. Red Dead Redemption 2, 2026-09-01: the SYNTHETIC
// contract's create was refused with 0xBAD00010, the caller cleared g_s12.ready,
// and g_s12.ready is one-way behind g_s12.tried -- so a minute later, when the
// game asked for DLSS again, the MIRROR was told "the private D3D12 session could
// not be opened" and stood down for the rest of the session. A failure on one
// path took away the other, which was working.
//
// Three in a row before the session is written off, so a transport that is really
// gone still stops instead of rebuilding every frame.
static const int kSynth12MaxBuildFails = 3;

// NOT g_s12.ready. That flag means "the private D3D12 session opened" -- device,
// queue, NGX, params, list and fence all exist -- and its three other clear sites
// are genuine session deaths: an SEH fault inside an evaluate, the same on the
// shared path, and a fence overrun. A clean 0xBAD00010 out of CreateFeature is
// none of those: the device is fine and one parameter block was refused.
//
// Clearing ready for it took the WORKING Vulkan mirror down together with the
// synthetic contract, in Red Dead Redemption 2 on 2026-09-01, because Synth12Open
// is one-way behind g_s12.tried and BOTH transports open through it. The comment
// above Synth12Build already said a failed build is "this attempt failed" and not
// "this session is unusable"; the code did not do it.
//
// And the first attempt at this bound was wrong in a second way: it counted three
// consecutive CALLS, and the caller retries immediately, so all three were spent
// in seventeen milliseconds. Pacing them changes nothing -- the refusal is
// deterministic and the user came back thirty seconds later either way. What was
// wrong was what tripping the bound DID.
static bool g_synth_build_dead;

// Returned by SynthArmRefusal and compared by ADDRESS, which is why it is a named
// object and not a literal inside the return: SynthMaybeDeliver's rate limit and
// the release below both key on pointer identity.
static const char kSynthContractDead[] =
    "the D3D12 feature for this contract was refused on three builds in a row, so the "
    "synthetic contract stops. The log says what NGX refused";

static void Synth12NoteBuildFailure(bool synthetic)
{
    if (++g_s12.build_fails < kSynth12MaxBuildFails) return;
    Log("[synth] %d D3D12 builds in a row failed, so this contract stops being "
        "retried. Everything above says why each one failed.", g_s12.build_fails);
    // Only from the synthetic call sites. The mirror's own stand-down is VkmRefuse,
    // which fires on its FIRST failed build, so this count was never the mirror's
    // stop switch -- and a mirror failure must not write off a contract it does
    // not own.
    if (synthetic) g_synth_build_dead = true;
}

// Optional formats follow out_pair to preserve existing positional callers.
static bool Synth12Build(const SynthParams *sp, UINT w, UINT h, UINT out_w, UINT out_h,
                         DXGI_FORMAT back_fmt, bool depth_packed = false,
                         UINT exp_w = 0, UINT exp_h = 0, bool out_pair = false,
                         DXGI_FORMAT exp_fmt = DXGI_FORMAT_R32_FLOAT,
                         DXGI_FORMAT output_fmt = DXGI_FORMAT_UNKNOWN,
                         bool srgb = false, bool hdr10 = false)
{
    if (!Synth12ReleaseTextures()) return false;
    g_s12.w = w; g_s12.h = h; g_s12.fmt = back_fmt;
    g_s12.out_w = out_w; g_s12.out_h = out_h;
    g_s12.depth_packed = depth_packed;

    // Colour and output on every transport; depth and motion vectors as well
    // when the frame has to cross into this device, because then they are not
    // the game's resources any more but copies of them.
    const DXGI_FORMAT slot_fmt[SLOT_COUNT] = {
        srgb ? Synth12UavFormat(back_fmt) : TypedFormat(back_fmt),
        Synth12UavFormat(output_fmt == DXGI_FORMAT_UNKNOWN ? back_fmt : output_fmt),
        // R32_FLOAT rather than any depth format: a depth-stencil resource is
        // not in the kernel's shared-format list, and NGX reads depth through
        // an ordinary shader resource view anyway. R16G16_FLOAT is what both
        // shipped motion estimation shaders declare and what the arming gate
        // already refused anything else on, so no conversion is implied here.
        //
        // R32_UINT instead when the mirror says the depth aspect arrives packed:
        // the same four bytes per texel and the same copy, declared as what they
        // actually are so Synth12MakeDepthUnpack can read them as integers.
        depth_packed ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R32_FLOAT,
        DXGI_FORMAT_R16G16_FLOAT,
    };
    const int slots = g_s12.shared ? SLOT_COUNT : (SLOT_OUTPUT + 1);
    g_s12.out_fmt = slot_fmt[SLOT_OUTPUT];
    g_s12.exp_fmt = exp_w != 0 && exp_h != 0 ? exp_fmt : DXGI_FORMAT_UNKNOWN;
    for (int i = 0; i < slots; ++i)
        if (!Synth12MakeTexture(i, i == SLOT_OUTPUT ? out_w : w, i == SLOT_OUTPUT ? out_h : h,
                                slot_fmt[i], i == SLOT_OUTPUT))
        { Synth12ReleaseTextures(); return false; }

    // The exposure mirror. Not in the loop above: it is not a slot, it has its
    // own size, and it exists only when this frame's caller read one off the
    // game. Preserve the caller's scalar float format, including R16_FLOAT.
    // No UAV: NGX reads it and nothing writes it here.
    g_s12.exp_w = exp_w; g_s12.exp_h = exp_h;
    if (exp_w != 0 && exp_h != 0 &&
        !Synth12MakeTexture(kExpTex, exp_w, exp_h, exp_fmt, false))
    { Synth12ReleaseTextures(); return false; }

    // The pipeline's second Output, and it is the ONLY texture the pipeline
    // doubles. The inputs are free by queue order: the game's copies into colour,
    // depth and motion vectors are recorded after the vkCmdWaitEvents on the
    // previous frame's evaluate, and that evaluate waited its own D3D12 fence
    // out before the event was set. Output is the one texture the worker writes
    // and the game reads in the same frame.
    //
    // A build that cannot make it fails whole rather than running half-pipelined:
    // the alternative is a transport that alternates two pairs against one image.
    // Said separately from every other failure here, because it is the only one
    // that is not the contract's: the caller drops to the park and rebuilds.
    g_s12.out_b = false;
    if (out_pair && g_s12.shared)
    {
        if (!Synth12MakeTexture(kOutB, out_w, out_h, slot_fmt[SLOT_OUTPUT], true))
        {
            Log("[synth] the pipeline's second Output (%ux%u %s) could not be created, so "
                "this session runs the park instead. vk_sync=2 is what asks for that "
                "texture; the substitute contract itself is unaffected.",
                out_w, out_h, FormatName(slot_fmt[SLOT_OUTPUT]));
            g_s12.out_b_fail = true;
            Synth12ReleaseTextures();
            return false;
        }
        g_s12.out_b = true;
    }

    if (depth_packed && !Synth12MakeDepthUnpack(w, h))
    { Synth12ReleaseTextures(); return false; }

    // The exposure mirror above and the D24 unpack just here stay single, and
    // neither is a decision this had to weigh: the substitute's Vulkan path
    // passes no exp_w and no depth_packed, so on the one transport that pipelines
    // there is nothing to double.
    if (g_s12.shared && InterlockedCompareExchange(&g_source, 0, 0) != SRC_MIRROR &&
        (PqBackBuffer(back_fmt) || srgb || hdr10) &&
        !PqPassMake(&g_s12.pq, g_s12.dev, g_s12.tex[SLOT_COLOR], g_s12.tex[SLOT_OUTPUT],
                    g_s12.tex[kOutB], srgb))
    { Synth12ReleaseTextures(); return false; }

    Log("[synth] colour in %s, output %s -- the frame is copied into the first and the "
        "result is copied back out of the second, because NGX reads one as a shader "
        "resource and writes the other through a UAV in the same call.",
        FormatName(slot_fmt[SLOT_COLOR]), FormatName(slot_fmt[SLOT_OUTPUT]));

    // What the driver will actually allow with these formats, asked before NGX is
    // asked to build anything with them. The D3D11 mirror has printed this since
    // Escape from Tarkov failed on exactly this and said so only from the other
    // add-on's side, as "output format 1 is not a supported typed codec format" --
    // by which point the bridge looked healthy and the fault looked like ours.
    // The synthetic path never had it, and a Vulkan game reaching this code
    // brings formats no D3D11 game ever has: Baldur's Gate 3 arrives with
    // R11G11B10_FLOAT, which is packed and has no alpha.
    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        if (g_s12.tex[i] == nullptr) continue;
        const D3D12_RESOURCE_DESC rd = g_s12.tex[i]->GetDesc();
        D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {};
        fs.Format = rd.Format;
        if (FAILED(g_s12.dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                                                  &fs, sizeof(fs))))
        {
            Log("[synth]   %-6s fmt %u: the driver would not answer CheckFeatureSupport",
                kSlotName[i], rd.Format);
            continue;
        }
        const bool sample = (fs.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) != 0;
        const bool uav    = (fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0;
        const bool load   = (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0;
        const bool store  = (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
        Log("[synth]   %-6s fmt %u %llux%u flags=0x%X: sample %s, typed UAV %s, "
            "UAV load %s, UAV store %s", kSlotName[i], rd.Format, rd.Width, rd.Height,
            rd.Flags, sample ? "yes" : "NO", uav ? "yes" : "NO",
            load ? "yes" : "NO", store ? "yes" : "NO");
        // Said where it is decisive rather than left to be inferred from four
        // yes/no words: DLSS writes its result through a typed UAV store, so a
        // format without one cannot be an Output whatever else it supports.
        if (i == SLOT_OUTPUT && !store)
            Log("[synth]   *** Output is %s and this driver has no typed UAV store for it. "
                "DLSS writes its result through one, so the feature cannot be built with "
                "this format. ***", FormatName(rd.Format));
        if (i == SLOT_OUTPUT && !(rd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
            Log("[synth]   *** Output was created without ALLOW_UNORDERED_ACCESS ***");
    }

    Synth12CopyU(sp, g_s12.params, "Width");
    Synth12CopyU(sp, g_s12.params, "Height");
    Synth12CopyU(sp, g_s12.params, "OutWidth");
    Synth12CopyU(sp, g_s12.params, "OutHeight");
    Synth12CopyU(sp, g_s12.params, "PerfQualityValue");
    Synth12CopyU(sp, g_s12.params, "DLSS.Feature.Create.Flags");
    Synth12CopyU(sp, g_s12.params, "DLSS.Enable.Output.Subrects");
    g_s12.params->Set("CreationNodeMask", 1u);
    g_s12.params->Set("VisibilityNodeMask", 1u);
    g_s12.params->Set("RTXValue", 0);

    {
        // Read back out of the block that will be handed to NGX, so the line
        // states what was set rather than what was intended.
        unsigned int used = 0;
        g_s12.params->Get("DLSS.Feature.Create.Flags", &used);
        // Which contract this build is for, read off the source rather than
        // assumed. The VULKAN mirror builds through this same function, so in a
        // mirror session both lines below said "synthetic" over the game's own
        // flags: Red Dead Redemption 2's log carries "the synthetic contract's
        // create flags 43" on every mirror rebuild, and 43 is the game's.
        const bool for_mirror =
            InterlockedCompareExchange(&g_source, 0, 0) == SRC_MIRROR;
        LogCreateFlags(for_mirror ? "the mirrored contract's create"
                                  : "the synthetic contract's create", used);
        // The other of the two places a create-flags value reaches NGX. The D3D11
        // mirror publishes its own in BridgeBuildResources and never comes through
        // here; the Vulkan mirror does, so the label has to follow the source or
        // the panel attributes the game's flags to a contract that does not exist.
        g_panel_flags_from = kFlagsFromLabel[for_mirror ? FLAGS_GAME : FLAGS_SYNTH];
        InterlockedExchange(&g_panel_flags, static_cast<LONG>(used));
    }

    // Every failure below this line gives the textures back before it returns.
    // Nothing retries after one -- the caller clears ready -- so what is left
    // behind is left for the life of the process, and on the shared path that is
    // four committed resources and four NT handles rather than only VRAM.
    // NVSDK_NGX_PerfQuality_Value spells DLAA 5, and that member arrived in DLSS
    // SDK 3.1.13. NGX loads the GAME FOLDER's super-resolution snippet before the
    // driver's, and a game folder can carry a much older one: Red Dead Redemption
    // 2 holds nvngx_dlss.dll 2.2.10.0, from 2021, which has no such member and
    // answers UnsupportedParameter. Replacing that file is not a way out either --
    // measured on 2026-09-01, the Rockstar launcher re-planted it within six
    // minutes, before the game had even hooked NGX.
    //
    // So the second attempt asks for the same thing in a spelling every DLSS 2.x
    // understands. The geometry is untouched, and a contract whose render size
    // equals its output size IS DLAA whatever the enum says -- the ratio is the
    // request, the value is only how it is named. This does not break copy, don't
    // invent: 5 is this add-on's own literal with no game-declared value behind
    // it, so changing which spelling it sends changes nothing the game stated.
    //
    // It also DECIDES what two nights of logs could not. Only one attempt differs
    // from the other, so if the retry succeeds the enum value was the obstacle,
    // and if it fails identically the 1:1 ratio is. Either answer is written down
    // rather than guessed at.
    // The four resource keys, set to the textures just built, BEFORE the create.
    // Same reason as the D3D11 path: they used to arrive holding the previous
    // build's freed pointers, clearing them to null fixed that fault and left the
    // create describing four resources that are null, and a neighbour add-on
    // captures the contract at the create. These exist by this line and are the
    // ones the evaluates will name.
    if (g_s12.params != nullptr)
    {
        g_s12.params->Set("Color",         g_s12.pq.in  != nullptr ? g_s12.pq.in  : g_s12.tex[SLOT_COLOR]);
        g_s12.params->Set("Output",        g_s12.pq.out != nullptr ? g_s12.pq.out : g_s12.tex[SLOT_OUTPUT]);
        g_s12.params->Set("Depth",         g_s12.tex[SLOT_DEPTH]);
        g_s12.params->Set("MotionVectors", g_s12.tex[SLOT_MV]);
    }

    static const unsigned int kPqDlaa    = 5;
    static const unsigned int kPqQuality = 2;

    unsigned int pq[2] = { 0, kPqQuality };
    g_s12.params->Get("PerfQualityValue", &pq[0]);
    const int attempts = pq[0] == kPqDlaa ? 2 : 1;

    NVSDK_NGX_Result rf = NGX_SUCCESS;
    DWORD code = 0;
    for (int a = 0; a < attempts; ++a)
    {
        if (a > 0)
        {
            Log("[synth] the create was refused as unsupported at PerfQualityValue %u, so "
                "it is asked once more at %u. The contract is unchanged -- %ux%u in and "
                "%ux%u out is DLAA by ratio whichever value names it -- and the snippet "
                "this process hooked is what decides which spelling it knows.",
                pq[0], pq[a], w, h, out_w, out_h);
            g_s12.params->Set("PerfQualityValue", pq[a]);
        }

        if (!Synth12Begin())
        { Log("[synth] could not start a command list"); Synth12ReleaseTextures(); return false; }
        code = 0;
        Breadcrumb("creating the D3D12 DLSS feature for the synthetic contract");
        rf = SafeCreateFeature(g_s12.create_feature, UnwrapList(g_s12.list),
                               g_s12.params, &g_s12.feature, &code);
        const UINT64 v = Synth12End();

        // A one-off, so it is waited out rather than pipelined -- but on ReShade's
        // present thread, so not for long enough to read as a hang.
        {
            if (!Synth12WaitFence(v, 4000))
            {
                // The one failure that does NOT hand the textures back. Four seconds
                // without a fence signal means the GPU never finished the create, so
                // the textures it was recorded against may still be in flight, and
                // freeing those is the use after free the wait exists to prevent.
                // Leaking them is the cheaper of the two mistakes.
                Log("[synth] feature creation did not complete within 4 s; giving up. The "
                    "textures it was recorded against are left allocated rather than freed "
                    "underneath a GPU that has not finished with them.");
                return false;
            }
        }

        // A fault is never retried: whatever raised it is not a spelling. Nor is a
        // success, nor any refusal other than the one a missing enum member gives.
        if (code != 0 || (rf == NGX_SUCCESS && g_s12.feature != nullptr) ||
            rf != kSynthNoSuchKey)
            break;
    }

    if (rf == NGX_SUCCESS && g_s12.feature != nullptr && pq[0] == kPqDlaa)
    {
        unsigned int used = 0;
        g_s12.params->Get("PerfQualityValue", &used);
        if (used != kPqDlaa)
            Log("[synth] accepted at PerfQualityValue %u. So the snippet this process "
                "hooked has no DLAA member, and the 1:1 contract itself was never the "
                "obstacle -- which is the thing the logs could not decide before.", used);
    }

    if (code != 0)
    {
        // What was observed, first. SafeCreateFeature runs under CaptureFault, so
        // the faulting address is known and the module that owns it can be named.
        // 0xE06D7363 is an MSVC C++ throw and its address is always inside
        // KERNELBASE's RaiseException, which names the messenger and not the
        // sender -- so the module line matters more here than the code does.
        wchar_t who[MAX_PATH + 64] = {};
        Warn("creating the synthetic DLSS feature raised exception 0x%08X inside NGX.", code);
        if (NameFaultOwner(who, _countof(who)))
            Log("[synth]   it faulted in %ls", who);
        Log("[synth]   the create parameters are the six lines above this one, and the "
            "format support for each texture is the four lines above those. An Output "
            "format with no typed UAV store is the failure this has been before.");
        // Offered as something to try, not as the cause. A reporter on 2026-08-31
        // made every nvngx_*.dll on their machine the same version and the create
        // still faulted, so a mismatch is one explanation among several.
        Log("[synth]   NGX loads its feature snippets from the game folder before the "
            "driver's own, so an nvngx_*.dll dropped in there is worth removing as a "
            "test. It is not established as the cause.");
        // Cleared before the release so nothing calls ReleaseFeature on a handle
        // NGX faulted while producing.
        g_s12.feature = nullptr;
        Synth12ReleaseTextures();
        return false;
    }
    if (rf != NGX_SUCCESS || g_s12.feature == nullptr)
    {
        Log("[synth] CreateFeature failed with result 0x%08X, %s", rf, NgxResultName(rf));

        // FeatureNotFound names a missing FILE, and which file is not obvious.
        // NGX loads the super-resolution snippet, nvngx_dlss.dll, from the
        // application folder, and a current driver store does not carry one:
        // measured on 2026-09-01, C:\WINDOWS\System32\DriverStore\FileRepository
        // holds nvngx_dlssg.dll and _nvngx.dll and no nvngx_dlss.dll at all.
        //
        // So a game that ships DLSS brings the file and this works, and a game
        // with NO DLSS brings nothing -- which is exactly the case the substitute
        // contract exists for. Reproduced in ngxGym by deleting that one file from
        // a run folder that still had nvngx_dlssnr.dll: SuperSampling went
        // unavailable with FeatureInitResult 0xBAD00004.
        if (rf == static_cast<NVSDK_NGX_Result>(0xBAD00004))
            Log("[synth]   that result means NGX found no super-resolution snippet to "
                "load. It takes nvngx_dlss.dll from the folder the executable is in, "
                "and current drivers do not ship one -- the driver store carries "
                "nvngx_dlssg.dll for frame generation and nothing for super "
                "resolution. A game that has DLSS brings the file with it; a game "
                "that has none brings nothing, so a copy has to be placed beside the "
                "executable for this contract to be buildable at all. Version 3.1.13 "
                "or newer, for the same reason the version check at arming gives.");
        // NGX names no parameter when it answers UnsupportedParameter, so the block
        // that was handed over is the only evidence there is. Red Dead Redemption 2
        // on 2026-09-01 returned 0xBAD00010 here and this line was not present, so
        // the report said only that something in the block was unacceptable.
        {
            unsigned int uw = 0, uh = 0, uow = 0, uoh = 0, upq = 0, ufl = 0, usr = 0;
            g_s12.params->Get("Width", &uw);      g_s12.params->Get("Height", &uh);
            g_s12.params->Get("OutWidth", &uow);  g_s12.params->Get("OutHeight", &uoh);
            g_s12.params->Get("PerfQualityValue", &upq);
            g_s12.params->Get("DLSS.Feature.Create.Flags", &ufl);
            g_s12.params->Get("DLSS.Enable.Output.Subrects", &usr);
            // The six values above are read back off the block; the three below
            // are this side's own state and are labelled as such, because a line
            // that says "the block it refused" must not mix the two silently.
            Log("[synth]   the block it refused: %ux%u -> %ux%u, PerfQualityValue %u, "
                "create flags %u, output subrects %u. This transport: exposure "
                "texture %s, colour %s, depth %s.",
                uw, uh, uow, uoh, upq, ufl, usr,
                g_s12.tex[kExpTex] != nullptr ? "carried" : "not set",
                FormatName(g_s12.fmt), g_s12.depth_packed ? "packed" : "unpacked");
            // The same ten keys the evaluate-fault dump prints, because the block
            // is allocated once per session and never Reset: a mirror's 1800
            // frames leave their subrect dimensions in it, and a synthetic create
            // that follows carries them. Whether that matters is exactly what a
            // reader of the next 0xBAD00010 needs to rule in or out, and until now
            // it could only be done by re-reading the source.
            static const char *const stale[] = {
                "DLSS.Render.Subrect.Dimensions.Width", "DLSS.Render.Subrect.Dimensions.Height",
                "DLSS.Input.Color.Subrect.Base.X", "DLSS.Input.Color.Subrect.Base.Y",
                "DLSS.Input.Depth.Subrect.Base.X", "DLSS.Input.Depth.Subrect.Base.Y",
                "DLSS.Input.MV.Subrect.Base.X",    "DLSS.Input.MV.Subrect.Base.Y",
                "DLSS.Output.Subrect.Base.X",      "DLSS.Output.Subrect.Base.Y",
            };
            for (size_t i = 0; i < _countof(stale); ++i)
            {
                unsigned int sv = 0;
                if (g_s12.params->Get(stale[i], &sv) == NGX_SUCCESS && sv != 0)
                    Log("[synth]   and still carries: %s = %u", stale[i], sv);
            }
        }
        g_s12.feature = nullptr;
        // The shape is cleared too. It is set before the create can fail, so the
        // panel printed "3840x1600 in, 3840x1600 out. No upscale: DLAA." over a
        // contract NGX had just refused. Zero makes it print "No contract yet",
        // and the rebuild predicate in both transports already tests the feature
        // pointer first, so the rebuild reason still reads "no feature yet".
        g_s12.w = 0;
        Synth12ReleaseTextures();
        return false;
    }

    Log("[synth] feature ready on %s: %ux%u -> %ux%u handle=%p",
        g_s12.shared ? "this add-on's own D3D12 device" : "the game's own device",
        w, h, out_w, out_h, static_cast<void *>(g_s12.feature));
    g_s12.need_reset = true;
    g_s12.build_fails = 0;
    // ponytail: cleared on ANY successful build, the mirror's included. So a game
    // toggled off a second time pays three more refused builds before it stops
    // again. Keying this to the refused shape would avoid that; three builds once
    // per toggle is not worth the state.
    g_synth_build_dead = false;
    return true;
}

// Everything the evaluate reads that is not a texture, carried across from the
// synthetic contract each frame. Both transports run this: the contract and the
// self-check that proves it complete are the same on either, and only the route
// the pixels take differs.
static void Synth12FrameParams(const SynthParams *sp)
{
    Synth12CopyU(sp, g_s12.params, "DLSS.Render.Subrect.Dimensions.Width");
    Synth12CopyU(sp, g_s12.params, "DLSS.Render.Subrect.Dimensions.Height");
    Synth12CopyU(sp, g_s12.params, "DLSS.Input.Color.Subrect.Base.X");
    Synth12CopyU(sp, g_s12.params, "DLSS.Input.Color.Subrect.Base.Y");
    Synth12CopyU(sp, g_s12.params, "DLSS.Input.Depth.Subrect.Base.X");
    Synth12CopyU(sp, g_s12.params, "DLSS.Input.Depth.Subrect.Base.Y");
    Synth12CopyU(sp, g_s12.params, "DLSS.Input.MV.Subrect.Base.X");
    Synth12CopyU(sp, g_s12.params, "DLSS.Input.MV.Subrect.Base.Y");
    Synth12CopyU(sp, g_s12.params, "DLSS.Output.Subrect.Base.X");
    Synth12CopyU(sp, g_s12.params, "DLSS.Output.Subrect.Base.Y");
    Synth12CopyF(sp, g_s12.params, "MV.Scale.X");
    Synth12CopyF(sp, g_s12.params, "MV.Scale.Y");
    Synth12CopyF(sp, g_s12.params, "Jitter.Offset.X");
    Synth12CopyF(sp, g_s12.params, "Jitter.Offset.Y");
    Synth12CopyF(sp, g_s12.params, "Sharpness");
    Synth12CopyF(sp, g_s12.params, "DLSS.Pre.Exposure");
    Synth12CopyF(sp, g_s12.params, "DLSS.Exposure.Scale");

    // ExposureTexture is absent from the contract on purpose and stays absent
    // here: this source supplies none, and setting a null would be supplying one.

    int reset = 0;
    sp->Get("Reset", &reset);
    g_s12.params->Set("Reset", (g_s12.need_reset || reset || g_cfg.reset_every) ? 1 : 0);
    g_s12.need_reset = false;
}

// One frame. The back buffer, ReShade's depth binding and the motion vector
// texture are all resources on this one device, so the whole transport is two
// copies with barriers either side of them.
//
// The states those resources are already in are read off ReShade's own code
// rather than guessed. At reshade_begin_effects the back buffer is in
// render_target: that is the old state runtime.cpp asserts for it in the very
// next thing it does, the implicit back buffer copy at the top of the first
// effect pass. Depth and motion vectors are in shader_resource, which on D3D12
// is PIXEL|NON_PIXEL_SHADER_RESOURCE -- generic_depth's own
// reshade_begin_effects handler leaves the depth there, and it is a built-in
// add-on, so ReShade registers its callback before any external one and it runs
// first. NON_PIXEL is already part of that state, so those two need no barrier
// at all, and not emitting one is strictly safer than emitting one whose old
// state might be wrong: a transition from a state a resource is not in is a
// device removal, not a warning.
static void Synth12Frame(reshade::api::effect_runtime *rt, const SynthParams *sp,
                         ID3D12Resource *back, ID3D12Resource *depth, ID3D12Resource *mv,
                         UINT w, UINT h, DXGI_FORMAT back_fmt)
{
    if (!Synth12Open(rt)) return;

    if (g_s12.feature == nullptr || g_s12.w != w || g_s12.h != h || g_s12.fmt != back_fmt)
    {
        Log("[synth] building the D3D12 feature for %ux%u %s", w, h, FormatName(back_fmt));
        if (!Synth12Build(sp, w, h, w, h, back_fmt)) { Synth12NoteBuildFailure(true); return; }
    }

    // ReShade's own commands for this frame are still sitting in an open list.
    // Submitting them first is what makes queue order the only ordering this
    // needs; there is no fence between the two halves because there are not two
    // queues.
    // ponytail: one extra flush per delivered frame. It costs a submission, not
    // the pipeline bubble the D3D11 path pays for crossing devices. If it ever
    // measures, the alternative is recording into ReShade's own list, which is
    // not safe -- see the note at the end of Synth12Open.
    reshade::api::command_queue *rq = rt->get_command_queue();
    if (rq != nullptr) rq->flush_immediate_command_list();

    if (!Synth12Begin()) return;

    Barrier(g_s12.list, back, D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(g_s12.list, g_s12.tex[SLOT_COLOR], D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COPY_DEST);
    g_s12.list->CopyResource(g_s12.tex[SLOT_COLOR], back);
    Barrier(g_s12.list, g_s12.tex[SLOT_COLOR], D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(g_s12.list, g_s12.tex[SLOT_OUTPUT], D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // The four textures are this frame's, set every frame. Depth and the motion
    // vectors are the game's own resources rather than copies of them, which is
    // the whole difference between this transport and the mirror's.
    g_s12.params->Set("Color", g_s12.tex[SLOT_COLOR]);
    g_s12.params->Set("Output", g_s12.tex[SLOT_OUTPUT]);
    g_s12.params->Set("Depth", depth);
    g_s12.params->Set("MotionVectors", mv);

    Synth12FrameParams(sp);

    DWORD code = 0;
    Breadcrumb("running the D3D12 evaluate for the synthetic contract");
    static const char kNgxMark[] = "ngx evaluate";
    g_s12.list->BeginEvent(1, kNgxMark, sizeof(kNgxMark));
    NVSDK_NGX_Result re = g_cfg.mode != 1
        ? SafeEvaluateGuarded(g_s12.eval_feature, UnwrapList(g_s12.list), g_s12.feature, g_s12.params, &code)
        : NGX_SUCCESS;
    g_s12.list->EndEvent();

    Barrier(g_s12.list, g_s12.tex[SLOT_OUTPUT], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    // Only mode 2 writes back, and this is NOT the mirror's three modes. There,
    // mode 1 copies the colour texture home instead of the output one, which is
    // what proves the transport in isolation; here that would need a second
    // barrier pair chosen by an if in the middle of a barrier sequence, which is
    // the exact shape that produces a transition from a state a resource is not
    // in. So on this transport 1 reads the same as 0: the evaluate runs, nothing
    // reaches the game. Say so rather than claim a parity that is not here.
    if (code == 0 && re == NGX_SUCCESS && g_cfg.mode == 2)
    {
        Barrier(g_s12.list, back, D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);
        g_s12.list->CopyResource(back, g_s12.tex[SLOT_OUTPUT]);
        Barrier(g_s12.list, back, D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    else
    {
        // The back buffer still has to go home in the state ReShade left it in,
        // whether or not anything was written into it.
        Barrier(g_s12.list, back, D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    Barrier(g_s12.list, g_s12.tex[SLOT_OUTPUT], D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_COMMON);
    Barrier(g_s12.list, g_s12.tex[SLOT_COLOR], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COMMON);
    Synth12End();

    if (code != 0)
    {
        // An access violation inside NGX means its own state is no longer
        // trustworthy, so this stops rather than calling back in.
        // Three facts with it, all reachable here and none of them recorded until
        // now. On Red Dead Redemption 2 this branch fires AFTER at least one
        // evaluate over the same textures, the same parameter block and the same
        // feature handle has already succeeded -- the render thread cannot read
        // the next contract until the worker has set idle, and it did -- so
        // everything fixed for the session is excluded and the question is what
        // changed between frames.
        //
        // The device's removed reason is the split that matters. S_OK exonerates
        // the GPU work and makes this a pointer walk inside NGX; HUNG, RESET or
        // DRIVER_INTERNAL_ERROR mean the private device died on an earlier
        // frame's work and NGX faulted on the corpse, which sends the search to
        // the imported textures instead of the parameter block. The two want
        // opposite next steps and guessing which costs a session.
        const HRESULT removed = g_s12.dev != nullptr ? g_s12.dev->GetDeviceRemovedReason()
                                                     : S_OK;
        Log("[synth] the D3D12 evaluate raised exception 0x%08X after %ld delivered "
            "frames; the private device's removed reason is 0x%08X. This backend stops "
            "here. The game keeps rendering on its own.",
            code, InterlockedCompareExchange(&g_frames_delivered, 0, 0), removed);
        {
            // The module and offset, as the create path has printed since 1.3.0.
            // An evaluate fault swallowed here never reached the crash filter,
            // so a report carried the code and nothing else (#22).
            wchar_t who[MAX_PATH + 64] = {};
            if (NameFaultOwner(who, _countof(who))) Log("[bridge]   it faulted in %ls", who);
            LogFaultStack("[bridge]");
        }
        // Named for the create failure already and never for this one, which is
        // the gap: "inside a snippet" and "inside the D3D12 runtime" are different
        // reports, and the faulting address tells them apart for free.
        {
            wchar_t who[MAX_PATH + 64] = {};
            if (NameFaultOwner(who, _countof(who)))
                Log("[synth]   it faulted in %ls", who);
        }
        g_s12.ready   = false;
        g_s12.feature = nullptr;
        return;
    }
    if (re != NGX_SUCCESS)
    {
        // Refused cleanly rather than faulted: the call returned, so the next
        // frame may try again.
        Log("[synth] the D3D12 evaluate was refused with result 0x%08X, %s",
            re, NgxResultName(re));
        return;
    }

    const UINT64 n = ++g_s12.frames;
    CountDelivered();
    if (n <= 1 || (n % 1800) == 0)
        Log("[synth] D3D12 frame %llu delivered (%ux%u)", n, w, h);
}
// The engine's own motion-vector texture, or null when the engine is not the
// source. An accessor rather than a direct read because g_ovk is declared far
// below the evaluate that has to ask.
static ID3D12Resource *SynthOfaVkMvTexture();


// The evaluate, for a session whose textures are all its own. The D3D12 half of
// the Vulkan transport is exactly this: no back buffer arrives on this device
// and none goes home from it, so there is nothing to copy here -- the four
// textures are filled and drained by the other API and this only transitions
// them, runs NGX over them and puts them back in COMMON.
//
// It is a second function rather than a branch inside Synth12Frame because what
// differs is the barrier sequence and nothing else, and a barrier sequence with
// an "if" in the middle of it is the thing that produces a transition from a
// state a resource is not in -- which on D3D12 is a device removal.
//
// wait_ms is the budget for both GPU waits inside it -- the allocator's retire
// fence at the top and the evaluate's own completion at the bottom -- and it is
// the caller's for the same reason each of them is: with the game's queue parked
// at a vkCmdWaitEvents until this returns, four seconds is twice the window
// Windows resets a device after.
// out_slot is which texture NGX writes. SLOT_OUTPUT on every caller but the
// pipelined transport's worker, which alternates it with kOutB so the game can
// copy the previous frame's result out of the other one while this runs.
static bool Synth12EvaluateShared(const SynthParams *sp, DWORD wait_ms = 4000,
                                  int out_slot = SLOT_OUTPUT, bool *wrote_out = nullptr,
                                  bool wait_completion = true)
{
    // ONE observation of mode, taken before anything else and handed back, and
    // the reason is that two reads of it disagree. "Did an evaluate write this
    // pair's Output" is the caller's question and the answer is decided here; the
    // caller used to read g_cfg.mode again after this function returned, with the
    // whole submission -- barriers, Synth12End, PqPassAfter, Synth12WaitFence --
    // between the two reads, while CfgReload replaces g_cfg wholesale from
    // another thread about once a second. A live 1 -> 2 edit landing in that gap
    // marked an Output nothing had ever written as valid, and under the pipeline
    // the copy back is decided from an earlier frame's flag, so that Output --
    // created fresh at the rebuild and never touched -- reached the back buffer.
    //
    // An out-parameter rather than a return value: the mirror's worker reads
    // false as a refusal and counts it toward the five that retire the mirror,
    // so this must not change what this function returns. It is written on every
    // path out, including the Synth12Begin failure below.
    const bool wrote = g_cfg.mode != 1;
    if (wrote_out != nullptr) *wrote_out = wrote;

    if (!Synth12Begin(wait_ms)) return false;

    // The three inputs by the loop and the Output by name, rather than the loop
    // deciding on i == SLOT_OUTPUT: with two Outputs alive the one this frame is
    // not writing must not be transitioned at all, or it is moved out of COMMON
    // while the game's queue is reading it.
    for (int i = 0; i < SLOT_COUNT; ++i)
        if (i != SLOT_OUTPUT)
            Barrier(g_s12.list, g_s12.tex[i], D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(g_s12.list, g_s12.tex[out_slot], D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // The packed branch, and nothing on any other path reaches it: depth_cvt is
    // null unless Synth12Build was told the mirror's depth arrives packed. The
    // loop above has already put tex[SLOT_DEPTH] in NON_PIXEL_SHADER_RESOURCE,
    // which is the state the SRV wants, so only the destination transitions here.
    if (g_s12.depth_cvt != nullptr)
    {
        Barrier(g_s12.list, g_s12.depth_cvt, D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap *heaps[] = { g_s12.cvt_heap };
        g_s12.list->SetDescriptorHeaps(1, heaps);
        g_s12.list->SetComputeRootSignature(g_s12.cvt_rs);
        g_s12.list->SetPipelineState(g_s12.cvt_pso);
        g_s12.list->SetComputeRootDescriptorTable(
            0, g_s12.cvt_heap->GetGPUDescriptorHandleForHeapStart());
        g_s12.list->Dispatch((g_s12.w + 7) / 8, (g_s12.h + 7) / 8, 1);
        Barrier(g_s12.list, g_s12.depth_cvt, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    // The caller's budget again, and this is the wait the park's own arithmetic
    // used to miss: PqPassWait was a hard-coded four seconds, so on an HDR10
    // presentation the worker's worst case carried two of them -- twice the TDR
    // window each, with the game's queue parked behind both.
    if (g_s12.pq.in != nullptr && !PqPassBefore(&g_s12.pq, g_s12.queue, wait_ms))
    {
        Log("[synth] the HDR10 decode did not run, so this frame is not evaluated.");
        for (int i = 0; i < SLOT_COUNT; ++i)
            if (i != SLOT_OUTPUT)
                Barrier(g_s12.list, g_s12.tex[i],
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COMMON);
        Barrier(g_s12.list, g_s12.tex[out_slot], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COMMON);
        Synth12End();
        return false;
    }
    g_s12.params->Set("Color",         g_s12.pq.in  != nullptr ? g_s12.pq.in  : g_s12.tex[SLOT_COLOR]);
    g_s12.params->Set("Output",        g_s12.pq.out != nullptr ? g_s12.pq.out : g_s12.tex[out_slot]);
    g_s12.params->Set("Depth",         g_s12.depth_cvt != nullptr ? g_s12.depth_cvt
                                                                  : g_s12.tex[SLOT_DEPTH]);
    // The engine's own texture when it is the source, and the transport's
    // otherwise. g_ovk.mv12 is non-null only after SynthOfaVkOpen succeeded, which
    // happens on no other path -- the mirror never opens it, so the D3D11 mirror
    // and the nine titles behind it read the same expression they always did.
    //
    // No copy back into SLOT_MV: NGX reads mv12 directly. It is a D3D12 resource
    // on this device like any other, opened from the engine's D3D11 texture.
    ID3D12Resource *const mv_engine = SynthOfaVkMvTexture();
    g_s12.params->Set("MotionVectors", mv_engine != nullptr ? mv_engine
                                                            : g_s12.tex[SLOT_MV]);
    // Only when this session carries one. It was unconditional for one build, on
    // the argument that a key set every frame cannot go stale -- true, but it also
    // introduced the key into every block that had never had it, and the staleness
    // it guards against is already handled by Synth12ReleaseTextures clearing it
    // before the texture is freed.
    if (g_s12.tex[kExpTex] != nullptr)
        g_s12.params->Set("ExposureTexture", g_s12.tex[kExpTex]);
    Synth12FrameParams(sp);

    DWORD code = 0;
    Breadcrumb("running the D3D12 evaluate for the synthetic contract");
    static const char kNgxMark[] = "ngx evaluate";
    g_s12.list->BeginEvent(1, kNgxMark, sizeof(kNgxMark));
    // mode 1 runs everything except the evaluate: the transport is exercised and
    // no upscaled result is written. Callers that decide whether to put that
    // Output on screen have to know -- see the worker's out_valid, which is
    // handed `wrote` rather than reading the key a second time.
    //
    // "Left exactly as it was" is true on an SDR presentation only. PqPassBefore
    // and PqPassAfter below are gated on g_s12.pq.in and on nothing about mode,
    // so on an HDR10 presentation the encode dispatch still writes tex[out_slot]
    // every mode-1 frame, out of a p->out that mode 1 left as the last real
    // evaluate wrote it -- or undefined, on a session that opened at mode 1.
    // Nothing reaches the screen from that on its own, because out_valid is false
    // and the copy back needs mode 2; it matters to a reader who would drop the
    // `wrote` term on the strength of the shorter sentence.
    //
    // ponytail: read off g_cfg without a lock, on the park's worker as well as on
    // the present thread, while CfgReload replaces the whole struct in one
    // non-atomic assignment about once a second. The worst case is one frame
    // built from two configurations -- the same register as the note on
    // work_ms/frame_n -- and it is not worth a lock. What is NOT tolerable is two
    // reads of the same key deciding two halves of one question, which is what
    // `wrote` exists to prevent. Synth12FrameParams reads reset_every the same way.
    NVSDK_NGX_Result re = wrote
        ? SafeEvaluateGuarded(g_s12.eval_feature, UnwrapList(g_s12.list), g_s12.feature, g_s12.params, &code)
        : NGX_SUCCESS;
    g_s12.list->EndEvent();

    for (int i = 0; i < SLOT_COUNT; ++i)
        if (i != SLOT_OUTPUT)
            Barrier(g_s12.list, g_s12.tex[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COMMON);
    Barrier(g_s12.list, g_s12.tex[out_slot], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COMMON);
    if (g_s12.depth_cvt != nullptr)
        Barrier(g_s12.list, g_s12.depth_cvt, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COMMON);
    UINT64 v = Synth12End();
    if (g_s12.pq.in != nullptr) {
        const UINT64 encoded = PqPassAfter(&g_s12.pq, g_s12.queue, g_s12.fence, &g_s12.fence_value, v,
                                          out_slot == SLOT_OUTPUT ? 0 : 1, wait_ms);
        if (encoded == v) {
            Log("[synth] output encoding failed; the completed NR submission is not an encoded output.");
            return false;
        }
        v = encoded;
    }

    if (code != 0)
    {
        // An access violation inside NGX means its own state is no longer
        // trustworthy, so this stops rather than calling back in.
        // The device's removed reason is the split that decides where to look
        // next. S_OK exonerates the GPU work and makes this a pointer walk inside
        // NGX; HUNG, RESET or DRIVER_INTERNAL_ERROR mean the private device died
        // on an earlier frame's work and NGX faulted on the corpse, which sends
        // the search to the imported textures instead of the parameter block.
        // The frame count matters too: this branch is reached on Red Dead
        // Redemption 2 only after at least one evaluate over the same textures
        // and the same feature has succeeded.
        const HRESULT removed = g_s12.dev != nullptr ? g_s12.dev->GetDeviceRemovedReason()
                                                     : S_OK;
        Log("[synth] the D3D12 evaluate raised exception 0x%08X after %ld delivered "
            "frames; the private device's removed reason is 0x%08X. This backend stops "
            "here. The game keeps rendering on its own.",
            code, InterlockedCompareExchange(&g_frames_delivered, 0, 0), removed);
        {
            // The module and offset, as the create path has printed since 1.3.0.
            // An evaluate fault swallowed here never reached the crash filter,
            // so a report carried the code and nothing else (#22).
            wchar_t who[MAX_PATH + 64] = {};
            if (NameFaultOwner(who, _countof(who))) Log("[bridge]   it faulted in %ls", who);
            LogFaultStack("[bridge]");
        }
        {
            wchar_t who[MAX_PATH + 64] = {};
            if (NameFaultOwner(who, _countof(who)))
                Log("[synth]   it faulted in %ls", who);
        }
        // Every per-frame value, as the block held it when it faulted. Eighty-nine
        // frames over the same textures and the same feature succeeded first, so
        // the only things that can have changed are these -- the ten subrects and
        // the five floats Synth12FrameParams copies out of the game's contract on
        // every frame. If one is out of range for the textures NGX was given,
        // this is where it shows.
        {
            static const char *const uk[] = {
                "DLSS.Render.Subrect.Dimensions.Width", "DLSS.Render.Subrect.Dimensions.Height",
                "DLSS.Input.Color.Subrect.Base.X", "DLSS.Input.Color.Subrect.Base.Y",
                "DLSS.Input.Depth.Subrect.Base.X", "DLSS.Input.Depth.Subrect.Base.Y",
                "DLSS.Input.MV.Subrect.Base.X",    "DLSS.Input.MV.Subrect.Base.Y",
                "DLSS.Output.Subrect.Base.X",      "DLSS.Output.Subrect.Base.Y",
                "Width", "Height", "OutWidth", "OutHeight", "Reset",
            };
            for (size_t i = 0; i < _countof(uk); ++i)
            {
                unsigned int uv = 0;
                if (g_s12.params->Get(uk[i], &uv) == NGX_SUCCESS && uv != 0)
                    Log("[synth]   at fault: %s = %u", uk[i], uv);
            }
            static const char *const fk[] = {
                "MV.Scale.X", "MV.Scale.Y", "Jitter.Offset.X", "Jitter.Offset.Y",
                "Sharpness", "DLSS.Pre.Exposure", "DLSS.Exposure.Scale",
            };
            for (size_t i = 0; i < _countof(fk); ++i)
            {
                float fv = 0.0f;
                if (g_s12.params->Get(fk[i], &fv) == NGX_SUCCESS)
                    Log("[synth]   at fault: %s = %.6f", fk[i], fv);
            }
        }
        g_s12.ready   = false;
        g_s12.feature = nullptr;
        return false;
    }
    if (re != NGX_SUCCESS)
    {
        // Refused cleanly rather than faulted: the call returned, so the next
        // frame may try again.
        Log("[synth] the D3D12 evaluate was refused with result 0x%08X, %s",
            re, NgxResultName(re));
        return false;
    }

    // The only thing ordering these two queues, since there is no shared fence
    // between them. The caller has already made sure the copies in have executed
    // -- by idling the game's Vulkan queue, or by seeing the park's ev_in -- and
    // this waits out the D3D12 half before the output is read back. Cheap to say,
    // expensive to run -- see the note below.
    // The budget is the caller's, because who is waiting decides what a safe wait
    // is. With vk_sync=1 this runs on ReShade's present thread with nothing else
    // parked behind it, and four seconds is a generous ceiling. On the Vulkan
    // mirror, and on the substitute's own park, the game's GPU is stopped at a
    // vkCmdWaitEvents until this returns, and Windows resets a device that stops
    // for two seconds -- so both pass a shorter one and give up inside the TDR
    // window.
    // The GPU relay (Pipeline=2/3) asks for the submission without this wait: its
    // release is signalled on this same D3D12 queue, so nothing on this thread
    // has to observe the completion -- the next frame's Synth12Begin waits the
    // allocator's retire fence, which is this submission's own fence value.
    if (wait_completion && !Synth12WaitFence(v, wait_ms))
    {
        // What "stops" means depends on who is reading. This backend records no
        // further frame, and the CPU readback the D3D11 path makes really is not
        // taken -- but on both worker-driven Vulkan transports the read is a copy
        // back that is ALREADY in the game's command buffer, recorded before this
        // ran, and the worker releases the queue into it unconditionally a moment
        // later. So that one frame can be torn: the same deliberate trade the
        // idle-wait timeout in SynthVkFrame states, a torn frame against a queue
        // parked past the 2 s TDR window.
        Log("[synth] the D3D12 evaluate did not complete within %lu ms, so this backend "
            "stops rather than read a texture the GPU may still be writing. On the Vulkan "
            "transports the copy back for this frame is already recorded and the game's "
            "queue is released into it, so this frame itself may be torn.", wait_ms);
        g_s12.ready = false;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The Vulkan transport
//
// A Vulkan game gets the same synthetic contract, the same self-check, the same
// arming gate and the same stand-down. What it cannot get is the game's own
// device: the call a DLSS 5 add-on detours is NVSDK_NGX_D3D12_EvaluateFeature
// and there is no Vulkan entry point to offer it instead, so the evaluate runs
// on a D3D12 device of this add-on's own -- exactly the reason the D3D11 mirror
// has one -- and the frame crosses as four textures created on the D3D12 side
// and imported into the game's VkDevice.
//
// NOTHING IS HOOKED, and that is a finding rather than a preference. ReShade's
// own vkCreateDevice appends VK_KHR_external_memory_win32 to every device it
// creates, as an optional extension: it checks the driver supports it, logs and
// carries on if not. That is the one extension this transport needs.
// VK_KHR_external_memory, VK_KHR_dedicated_allocation and
// VK_KHR_get_memory_requirements2 were all promoted into Vulkan 1.1 core, so on
// any device ReShade itself runs on they are simply there. The peer project
// hooks vkCreateDevice to add that list and to force the timelineSemaphore
// feature on; ReShade already forces timelineSemaphore on unconditionally, for
// its own present-from-compute path. So the hook would buy exactly one thing
// this transport does not use -- VK_KHR_external_semaphore_win32, the
// shared-fence import -- and cost the two hazards the note above HookRemove in
// DllMain names: a second thread through the window where the original bytes are
// back, and a jump left in place across an unload. This transport installs no
// Vulkan hook at all, and nothing is removed on detach because nothing was
// installed. The relay is the one part of this add-on that does install one:
// with Pipeline 2 or 3 and Import=1, fg-relay.inc patches this same vkCreateDevice
// to append that one extension, for an import this transport does not take.
//
// The price of not importing the fence is that the two queues are ordered by
// something other than a shared fence. That used to be the CPU -- ReShade's queue
// idled after the copies in, the D3D12 fence waited on before the copy out, both
// on the present thread -- and it is now the same event park the Vulkan mirror
// runs, with the CPU wait left in place behind vk_sync=1. See "The park" above
// SynthVkFrame for the shape and for what it does and does not fix.
// ponytail: the park stops the game's QUEUE for the length of the evaluate
// instead of its render thread, which is the whole win and is not the whole of
// it. The cheaper of the two upgrades this note named is done: vk_sync=2 runs
// frame N's evaluate while the game renders N+1, so the wait the queue reaches
// is on work that is already a frame old rather than on work that starts when
// the queue arrives -- at one present of latency. It turned out narrower than this note guessed -- one
// second Output rather than a second set of shared images, because with the wait
// recorded above the copies the inputs are free by queue order. The remaining
// upgrade is importing g_s12.fence as a Vulkan timeline semaphore -- a D3D12
// fence and a timeline semaphore are the same object, and g_s12.fence would only
// have to be created D3D12_FENCE_FLAG_SHARED -- which needs
// VK_KHR_external_semaphore_win32. ReShade does not enable it, so the extension
// has to be added at device creation for a device to carry it at all; with
// Pipeline 2 or 3 and Import=1 the relay's create-device hook is what adds it,
// and the import below is still this transport's own decision to take or not.
//
// Every Vulkan entry point is resolved through vkGetDeviceProcAddr out of a
// LoadLibrary of vulkan-1.dll. There is deliberately no import of it: a D3D11
// game on a machine with no Vulkan runtime has to go on loading this add-on, and
// an import table entry would stop it at the loader before DllMain ran.
//
// The Vulkan declarations below are written out rather than vendored, and that
// is the opposite call from the one at the top of this file. That argument was
// about a vtable: an implicit slot count that nothing states and that has been
// observed to move between ReShade releases. These are plain C structs from a
// published ABI that cannot change -- every driver on the machine is already
// compiled against them -- each one checkable against the specification by a
// reader with it open. The alternative is vendoring vulkan_core.h and the
// vk_video tree it includes, for fourteen entry points.
// ---------------------------------------------------------------------------

// Dispatchable handles are pointers; the rest are 64 bits wide on x64, which is
// the only architecture this add-on builds for.
typedef struct SVkDevice_T        *SVkDevice;
typedef struct SVkCommandBuffer_T *SVkCommandBuffer;
typedef uint64_t                   SVkHandle;   // VkImage, VkDeviceMemory, VkBuffer

static const uint32_t kSVkSuccess               = 0;
static const uint32_t kSVkStMemoryAllocateInfo  = 5;
static const uint32_t kSVkStBufferCreateInfo    = 12;
static const uint32_t kSVkStImageCreateInfo     = 14;
static const uint32_t kSVkStImageMemoryBarrier  = 45;
static const uint32_t kSVkStExternalMemoryImage = 1000072001;   // VK_KHR_external_memory
static const uint32_t kSVkStImportMemoryWin32   = 1000073000;   // VK_KHR_external_memory_win32
static const uint32_t kSVkStMemoryWin32Props    = 1000073002;   // ...HANDLE_PROPERTIES_KHR
static const uint32_t kSVkStMemoryDedicated     = 1000127001;   // promoted into Vulkan 1.1
static const uint32_t kSVkHandleTypeD3D12Res    = 0x40;         // D3D12_RESOURCE_BIT
static const uint32_t kSVkImageType2D           = 1;
static const uint32_t kSVkSampleCount1          = 1;
static const uint32_t kSVkTilingOptimal         = 0;
static const uint32_t kSVkSharingExclusive      = 0;
static const uint32_t kSVkUsageTransferSrc      = 1;
static const uint32_t kSVkUsageTransferDst      = 2;
static const uint32_t kSVkLayoutUndefined       = 0;
static const uint32_t kSVkLayoutGeneral         = 1;
static const uint32_t kSVkLayoutTransferSrc     = 6;
static const uint32_t kSVkLayoutTransferDst     = 7;
static const uint32_t kSVkAspectColor           = 1;
static const uint32_t kSVkAspectDepth           = 2;
static const uint32_t kSVkAccessMemoryRead      = 0x8000;
static const uint32_t kSVkAccessMemoryWrite     = 0x10000;
static const uint32_t kSVkStageAllCommands      = 0x10000;
static const uint32_t kSVkQueueFamilyIgnored    = 0xFFFFFFFFu;

// Added for the Vulkan mirror's park. All Vulkan 1.0 core.
static const uint32_t kSVkStEventCreateInfo     = 10;
static const uint32_t kSVkStMemoryBarrier       = 46;
static const uint32_t kSVkEventSet              = 3;   // VK_EVENT_SET
static const uint32_t kSVkStageHost             = 0x4000;

struct SVkEventCreateInfo { uint32_t sType; const void *pNext; uint32_t flags; };
struct SVkMemoryBarrier   { uint32_t sType; const void *pNext;
                            uint32_t srcAccessMask, dstAccessMask; };

struct SVkExtent3D { uint32_t width, height, depth; };
struct SVkOffset3D { int32_t  x, y, z; };
struct SVkSubresourceLayers { uint32_t aspectMask, mipLevel, baseArrayLayer, layerCount; };
struct SVkSubresourceRange  { uint32_t aspectMask, baseMipLevel, levelCount, baseArrayLayer, layerCount; };

struct SVkExternalMemoryImageCreateInfo
{
    uint32_t    sType;
    const void *pNext;
    uint32_t    handleTypes;
};

struct SVkImageCreateInfo
{
    uint32_t        sType;
    const void     *pNext;
    uint32_t        flags;
    uint32_t        imageType;
    uint32_t        format;
    SVkExtent3D     extent;
    uint32_t        mipLevels;
    uint32_t        arrayLayers;
    uint32_t        samples;
    uint32_t        tiling;
    uint32_t        usage;
    uint32_t        sharingMode;
    uint32_t        queueFamilyIndexCount;
    const uint32_t *pQueueFamilyIndices;
    uint32_t        initialLayout;
};

struct SVkBufferCreateInfo
{
    uint32_t        sType;
    const void     *pNext;
    uint32_t        flags;
    uint64_t        size;
    uint32_t        usage;
    uint32_t        sharingMode;
    uint32_t        queueFamilyIndexCount;
    const uint32_t *pQueueFamilyIndices;
};

struct SVkMemoryRequirements { uint64_t size; uint64_t alignment; uint32_t memoryTypeBits; };

struct SVkMemoryDedicatedAllocateInfo
{
    uint32_t    sType;
    const void *pNext;
    SVkHandle   image;
    SVkHandle   buffer;
};

struct SVkImportMemoryWin32HandleInfoKHR
{
    uint32_t    sType;
    const void *pNext;
    uint32_t    handleType;
    HANDLE      handle;
    LPCWSTR     name;
};

// What vkGetMemoryWin32HandlePropertiesKHR answers: the memory types an imported
// handle may be allocated from. The specification requires memoryTypeIndex to be
// one of these for an import, and the image's own memoryTypeBits is a separate,
// wider set -- picking from that one alone is what the validation layer reported
// as VUID-VkMemoryAllocateInfo-memoryTypeIndex-00645.
struct SVkMemoryWin32HandleProperties
{
    uint32_t sType;
    void    *pNext;
    uint32_t memoryTypeBits;
};

struct SVkMemoryAllocateInfo
{
    uint32_t    sType;
    const void *pNext;
    uint64_t    allocationSize;
    uint32_t    memoryTypeIndex;
};

struct SVkImageMemoryBarrier
{
    uint32_t            sType;
    const void         *pNext;
    uint32_t            srcAccessMask;
    uint32_t            dstAccessMask;
    uint32_t            oldLayout;
    uint32_t            newLayout;
    uint32_t            srcQueueFamilyIndex;
    uint32_t            dstQueueFamilyIndex;
    SVkHandle           image;
    SVkSubresourceRange subresourceRange;
};

struct SVkImageCopy
{
    SVkSubresourceLayers srcSubresource;
    SVkOffset3D          srcOffset;
    SVkSubresourceLayers dstSubresource;
    SVkOffset3D          dstOffset;
    SVkExtent3D          extent;
};

struct SVkBufferImageCopy
{
    uint64_t             bufferOffset;
    uint32_t             bufferRowLength;
    uint32_t             bufferImageHeight;
    SVkSubresourceLayers imageSubresource;
    SVkOffset3D          imageOffset;
    SVkExtent3D          imageExtent;
};

typedef void *  (*PFN_SVkGetDeviceProcAddr)(SVkDevice, const char *);
typedef uint32_t(*PFN_SVkCreateImage)(SVkDevice, const SVkImageCreateInfo *, const void *, SVkHandle *);
typedef void    (*PFN_SVkDestroyImage)(SVkDevice, SVkHandle, const void *);
typedef void    (*PFN_SVkGetImageMemoryRequirements)(SVkDevice, SVkHandle, SVkMemoryRequirements *);
typedef uint32_t(*PFN_SVkAllocateMemory)(SVkDevice, const SVkMemoryAllocateInfo *, const void *, SVkHandle *);
typedef uint32_t(*PFN_SVkGetMemoryWin32HandleProperties)(SVkDevice, uint32_t, void *,
                                                        SVkMemoryWin32HandleProperties *);
typedef void    (*PFN_SVkFreeMemory)(SVkDevice, SVkHandle, const void *);
typedef uint32_t(*PFN_SVkBindImageMemory)(SVkDevice, SVkHandle, SVkHandle, uint64_t);
typedef uint32_t(*PFN_SVkCreateBuffer)(SVkDevice, const SVkBufferCreateInfo *, const void *, SVkHandle *);
typedef void    (*PFN_SVkDestroyBuffer)(SVkDevice, SVkHandle, const void *);
typedef void    (*PFN_SVkGetBufferMemoryRequirements)(SVkDevice, SVkHandle, SVkMemoryRequirements *);
typedef uint32_t(*PFN_SVkBindBufferMemory)(SVkDevice, SVkHandle, SVkHandle, uint64_t);
typedef void    (*PFN_SVkCmdPipelineBarrier)(SVkCommandBuffer, uint32_t, uint32_t, uint32_t,
                                             uint32_t, const void *, uint32_t, const void *,
                                             uint32_t, const SVkImageMemoryBarrier *);
typedef void    (*PFN_SVkCmdCopyImage)(SVkCommandBuffer, SVkHandle, uint32_t, SVkHandle, uint32_t,
                                       uint32_t, const SVkImageCopy *);
typedef void    (*PFN_SVkCmdCopyImageToBuffer)(SVkCommandBuffer, SVkHandle, uint32_t, SVkHandle,
                                               uint32_t, const SVkBufferImageCopy *);
typedef void    (*PFN_SVkCmdCopyBufferToImage)(SVkCommandBuffer, SVkHandle, SVkHandle, uint32_t,
                                               uint32_t, const SVkBufferImageCopy *);
// The park, and the one wait the mirror's rebuild needs. Core Vulkan 1.0.
typedef uint32_t(*PFN_SVkEvCreate)(SVkDevice, const SVkEventCreateInfo *, const void *, SVkHandle *);
typedef void    (*PFN_SVkEvDestroy)(SVkDevice, SVkHandle, const void *);
typedef uint32_t(*PFN_SVkEvStatus)(SVkDevice, SVkHandle);
typedef uint32_t(*PFN_SVkEvSet)(SVkDevice, SVkHandle);
typedef uint32_t(*PFN_SVkEvReset)(SVkDevice, SVkHandle);
typedef void    (*PFN_SVkCmdSetEvent)(SVkCommandBuffer, SVkHandle, uint32_t);
typedef void    (*PFN_SVkCmdWaitEvents)(SVkCommandBuffer, uint32_t, const SVkHandle *,
                                        uint32_t, uint32_t,
                                        uint32_t, const SVkMemoryBarrier *,
                                        uint32_t, const void *,
                                        uint32_t, const SVkImageMemoryBarrier *);
typedef uint32_t(*PFN_SVkDeviceWaitIdle)(SVkDevice);

struct SynthVk
{
    bool      tried;
    bool      ready;
    bool      laid_out;      // the four images have been taken out of UNDEFINED
    // The same fact for the pipeline's second Output, and a separate bool rather
    // than laid_out becoming an array because vkmirror.inc reads laid_out in four
    // places and the mirror has no second Output to lay out. Only the
    // substitute's own barrier reads this one.
    bool      laid_out_b;
    // Whether VK_KHR_push_descriptor is enabled on this device. Resolved rather
    // than assumed, and never called from here: it is ReShade's own
    // command_list::push_descriptors that needs it, and that function checks its
    // dispatch table, asserts in a debug build and RETURNS DOING NOTHING in a
    // release one -- a dispatch with no descriptors bound, which is silent wrong
    // pixels rather than an error. The same resolve-is-the-proof argument the
    // vkGetMemoryWin32HandleKHR gate below makes, for the same reason.
    bool      have_push_desc;
    HMODULE   lib;
    SVkDevice dev;

    PFN_SVkCreateImage                 CreateImage;
    PFN_SVkDestroyImage                DestroyImage;
    PFN_SVkGetImageMemoryRequirements  GetImageMemoryRequirements;
    PFN_SVkAllocateMemory              AllocateMemory;
    PFN_SVkGetMemoryWin32HandleProperties GetMemoryWin32HandleProperties;
    PFN_SVkFreeMemory                  FreeMemory;
    PFN_SVkBindImageMemory             BindImageMemory;
    PFN_SVkCreateBuffer                CreateBuffer;
    PFN_SVkDestroyBuffer               DestroyBuffer;
    PFN_SVkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_SVkBindBufferMemory            BindBufferMemory;
    PFN_SVkCmdPipelineBarrier          CmdPipelineBarrier;
    PFN_SVkCmdCopyImage                CmdCopyImage;
    PFN_SVkCmdCopyImageToBuffer        CmdCopyImageToBuffer;
    PFN_SVkCmdCopyBufferToImage        CmdCopyBufferToImage;
    PFN_SVkEvCreate                    EvCreate;
    PFN_SVkEvDestroy                   EvDestroy;
    PFN_SVkEvStatus                    EvStatus;
    PFN_SVkEvSet                       EvSet;
    PFN_SVkEvReset                     EvReset;
    PFN_SVkCmdSetEvent                 CmdSetEvent;
    PFN_SVkCmdWaitEvents               CmdWaitEvents;
    PFN_SVkDeviceWaitIdle              DeviceWaitIdle;

    // The same four slots the D3D12 side created, seen from Vulkan, plus the
    // exposure mirror at kExpTex and the pipeline's second Output at kOutB. Every
    // loop over these stops at SLOT_COUNT except SynthVkImport's and
    // SynthVkReleaseImages', which is why the synthetic path -- which creates a
    // fifth texture on one transport in one mode and a sixth on none -- is
    // untouched.
    SVkHandle img[SLOT_COUNT + 2];
    SVkHandle mem[SLOT_COUNT + 2];

    // Depth cannot be copied image to image into anything: vkCmdCopyImage and
    // vkCmdBlitImage both require the two formats to match exactly when either
    // side is a depth format, and the shared texture on the D3D12 side is
    // R32_FLOAT because a depth-stencil resource is not shareable. A copy
    // through a buffer carries no such rule -- the depth aspect of a 32-bit
    // float depth image is written out as one float per texel, which is exactly
    // what the R32_FLOAT texture takes back in -- so this is the one place a
    // staging allocation is needed.
    //
    // The SYNTHETIC transport no longer uses it: it samples the depth in a
    // compute pass instead, which is what let it stop demanding a 32-bit float
    // copy source. This stays for the VULKAN MIRROR, which is inside an NGX
    // detour with no ReShade command_list to dispatch through and only a raw
    // VkCommandBuffer, and which therefore still carries the copy path. It no
    // longer refuses a depth aspect that is not 32-bit float: the depth aspect of
    // D24_UNORM_S8_UINT is four bytes per texel too, so it travels through this
    // same buffer untouched and Synth12MakeDepthUnpack unpacks it on the far
    // side. What is still refused is a SIXTEEN-bit depth aspect, which is two
    // bytes per texel and would need this allocation and its texture resized.
    SVkHandle buf, buf_mem;

    UINT64 frames;
};

static SynthVk g_svk;

// DXGI to Vulkan, over the formats the four shared textures can actually be.
// Anything else returns VK_FORMAT_UNDEFINED and the caller refuses by name,
// rather than importing a texture whose bits nothing agrees about.
static uint32_t SynthVkFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:        return 37;   // VK_FORMAT_R8G8B8A8_UNORM
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   return 43;   // VK_FORMAT_R8G8B8A8_SRGB
    case DXGI_FORMAT_B8G8R8A8_UNORM:        return 44;   // VK_FORMAT_B8G8R8A8_UNORM
    // Vulkan has no BGRX. The two are the same four bytes and the fourth is
    // ignored on both sides, so this is a spelling difference, not a conversion.
    case DXGI_FORMAT_B8G8R8X8_UNORM:        return 44;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:   return 50;   // VK_FORMAT_B8G8R8A8_SRGB
    case DXGI_FORMAT_R10G10B10A2_UNORM:     return 64;   // VK_FORMAT_A2B10G10R10_UNORM_PACK32
    case DXGI_FORMAT_R16_FLOAT:             return 76;   // VK_FORMAT_R16_SFLOAT
    case DXGI_FORMAT_R16G16_FLOAT:          return 83;   // VK_FORMAT_R16G16_SFLOAT
    case DXGI_FORMAT_R16G16B16A16_FLOAT:    return 97;   // VK_FORMAT_R16G16B16A16_SFLOAT
    case DXGI_FORMAT_R32_FLOAT:             return 100;  // VK_FORMAT_R32_SFLOAT
    // Only the mirror's packed depth slot. Same four bytes as the line above and
    // the same copy; the difference is only that these are integers until
    // Synth12MakeDepthUnpack's pass has run over them.
    case DXGI_FORMAT_R32_UINT:              return 98;   // VK_FORMAT_R32_UINT
    case DXGI_FORMAT_R11G11B10_FLOAT:       return 122;  // VK_FORMAT_B10G11R11_UFLOAT_PACK32
    default:                                return 0;    // VK_FORMAT_UNDEFINED
    }
}

// A barrier on one of THIS add-on's images and never on one of the game's.
// ReShade's own command_list::barrier is what handles the game's resources: it
// reads the image's aspect mask out of the private data it keeps per VkImage,
// which for a depth image is not COLOR -- and which for an image ReShade never
// created is a null it goes on to dereference.
//
// Deliberately over-broad on stages and access. One copy per frame does not
// repay a precise dependency, and a narrow one that is wrong is a race that
// shows up as a frame of stale pixels rather than as an error.
// aspect defaults to COLOR, which is every image this add-on creates. The Vulkan
// mirror is the one caller that passes something else: it barriers the GAME's
// images, and the game's depth image is DEPTH. It cannot use ReShade's own
// barrier for them the way SynthVkFrame does, because inside an NGX detour there
// is no ReShade command_list -- only the game's raw VkCommandBuffer.
static void SynthVkBarrier(SVkCommandBuffer cb, SVkHandle img, uint32_t from, uint32_t to,
                           uint32_t aspect = kSVkAspectColor)
{
    SVkImageMemoryBarrier b = {};
    b.sType         = kSVkStImageMemoryBarrier;
    b.srcAccessMask = kSVkAccessMemoryRead | kSVkAccessMemoryWrite;
    b.dstAccessMask = kSVkAccessMemoryRead | kSVkAccessMemoryWrite;
    b.oldLayout     = from;
    b.newLayout     = to;
    // Not a queue family ownership transfer. These images are
    // VK_SHARING_MODE_EXCLUSIVE and are only ever touched from the one queue
    // ReShade renders effects on; the other API's access to them is separated by
    // the CPU wait or by the event park, not by an acquire and release pair.
    b.srcQueueFamilyIndex = kSVkQueueFamilyIgnored;
    b.dstQueueFamilyIndex = kSVkQueueFamilyIgnored;
    b.image               = img;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    g_svk.CmdPipelineBarrier(cb, kSVkStageAllCommands, kSVkStageAllCommands, 0,
                             0, nullptr, 0, nullptr, 1, &b);
}

// dw/dh are the destination's own size, and zero means "the same as the source",
// which is every caller on the synthetic path -- it created both images itself
// and knows they match. The mirror does not: the sizes come off the game's
// parameter block, so a contract whose Output is smaller than the feature
// declares truncates the copy rather than producing undefined behaviour.
static void SynthVkCopyImage(SVkCommandBuffer cb, SVkHandle src, uint32_t src_layout,
                             SVkHandle dst, uint32_t dst_layout, UINT w, UINT h,
                             UINT dw = 0, UINT dh = 0)
{
    if (dw != 0 && dw < w) w = dw;
    if (dh != 0 && dh < h) h = dh;

    SVkImageCopy c = {};
    c.srcSubresource.aspectMask = kSVkAspectColor;
    c.srcSubresource.layerCount = 1;
    c.dstSubresource.aspectMask = kSVkAspectColor;
    c.dstSubresource.layerCount = 1;
    c.extent.width  = w;
    c.extent.height = h;
    c.extent.depth  = 1;
    g_svk.CmdCopyImage(cb, src, src_layout, dst, dst_layout, 1, &c);
}

static void SynthVkReleaseImages()
{
    // Gated on the entry points rather than on ready, because the one caller
    // that most needs this is a half-finished import -- which sets ready false
    // on its way out and would otherwise leave every image it did create behind.
    if (g_svk.dev == nullptr || g_svk.DestroyImage == nullptr) return;
    // SLOT_COUNT + 2: the exposure mirror and the pipeline's second Output too.
    // Both entries of each are zero when this session never made one, so a
    // session that carries neither pays nothing for the extra turns of the loop.
    for (int i = 0; i < SLOT_COUNT + 2; ++i)
    {
        if (g_svk.img[i] != 0) { g_svk.DestroyImage(g_svk.dev, g_svk.img[i], nullptr); g_svk.img[i] = 0; }
        if (g_svk.mem[i] != 0) { g_svk.FreeMemory(g_svk.dev, g_svk.mem[i], nullptr);   g_svk.mem[i] = 0; }
    }
    if (g_svk.buf     != 0) { g_svk.DestroyBuffer(g_svk.dev, g_svk.buf, nullptr);  g_svk.buf     = 0; }
    if (g_svk.buf_mem != 0) { g_svk.FreeMemory(g_svk.dev, g_svk.buf_mem, nullptr); g_svk.buf_mem = 0; }
    g_svk.laid_out = false;
    g_svk.laid_out_b = false;
}

// Resolve everything, and refuse by name on the first thing that is not there.
// Runs once and a failure is never retried, the same rule the D3D12 session open
// follows.
static bool SynthVkOpen(reshade::api::effect_runtime *rt)
{
    if (g_svk.tried) return g_svk.ready;
    g_svk.tried = true;

    Breadcrumb("opening the Vulkan side of the synthetic transport");
    reshade::api::device *rdev = rt->get_device();
    g_svk.dev = rdev != nullptr
        ? reinterpret_cast<SVkDevice>(static_cast<uintptr_t>(rdev->get_native())) : nullptr;
    if (g_svk.dev == nullptr)
    { Log("[synth] the effect runtime reports no native VkDevice"); return false; }

    // Loaded, never linked. An import of vulkan-1.dll would stop this add-on at
    // the loader in every D3D11 game on a machine with no Vulkan runtime, which
    // is a plausible description of most of the nine titles the mirror runs in.
    g_svk.lib = LoadLibraryW(L"vulkan-1.dll");
    if (g_svk.lib == nullptr)
    {
        Log("[synth] vulkan-1.dll is not loadable in this process, so no Vulkan entry "
            "point can be resolved and this transport refuses. The game is unaffected.");
        return false;
    }
    auto get_proc = reinterpret_cast<PFN_SVkGetDeviceProcAddr>(
        GetProcAddress(g_svk.lib, "vkGetDeviceProcAddr"));
    if (get_proc == nullptr)
    { Log("[synth] vulkan-1.dll exports no vkGetDeviceProcAddr"); return false; }

    // Counted rather than written out, so the line below states the number that
    // was resolved and goes on being true when a fifteenth is added here.
    int resolved = 0;
    #define SYNTH_VK_GET(member, name)                                                     \
        g_svk.member = reinterpret_cast<PFN_SVk##member>(get_proc(g_svk.dev, name));       \
        if (g_svk.member == nullptr)                                                       \
        {                                                                                  \
            Log("[synth] this Vulkan device does not resolve %s, so the transport "        \
                "refuses. Everything asked for here is core Vulkan, so a miss means the "  \
                "device was created without it rather than that it is unsupported.", name);\
            return false;                                                                  \
        }                                                                                  \
        ++resolved;
    SYNTH_VK_GET(CreateImage,                 "vkCreateImage")
    SYNTH_VK_GET(DestroyImage,                "vkDestroyImage")
    SYNTH_VK_GET(GetImageMemoryRequirements,  "vkGetImageMemoryRequirements")
    SYNTH_VK_GET(AllocateMemory,              "vkAllocateMemory")
    SYNTH_VK_GET(FreeMemory,                  "vkFreeMemory")
    SYNTH_VK_GET(BindImageMemory,             "vkBindImageMemory")
    SYNTH_VK_GET(CreateBuffer,                "vkCreateBuffer")
    SYNTH_VK_GET(DestroyBuffer,               "vkDestroyBuffer")
    SYNTH_VK_GET(GetBufferMemoryRequirements, "vkGetBufferMemoryRequirements")
    SYNTH_VK_GET(BindBufferMemory,            "vkBindBufferMemory")
    SYNTH_VK_GET(CmdPipelineBarrier,          "vkCmdPipelineBarrier")
    SYNTH_VK_GET(CmdCopyImage,                "vkCmdCopyImage")
    SYNTH_VK_GET(CmdCopyImageToBuffer,        "vkCmdCopyImageToBuffer")
    SYNTH_VK_GET(CmdCopyBufferToImage,        "vkCmdCopyBufferToImage")
    // The Vulkan mirror's park, and the one wait its rebuild needs. Resolved on
    // both transports rather than only the mirror's: all eight are Vulkan 1.0
    // core, so a device that resolves the fourteen above resolves these too, and
    // one resolve list is one thing to keep in step instead of two.
    SYNTH_VK_GET(EvCreate,                    "vkCreateEvent")
    SYNTH_VK_GET(EvDestroy,                   "vkDestroyEvent")
    SYNTH_VK_GET(EvStatus,                    "vkGetEventStatus")
    SYNTH_VK_GET(EvSet,                       "vkSetEvent")
    SYNTH_VK_GET(EvReset,                     "vkResetEvent")
    SYNTH_VK_GET(CmdSetEvent,                 "vkCmdSetEvent")
    SYNTH_VK_GET(CmdWaitEvents,               "vkCmdWaitEvents")
    SYNTH_VK_GET(DeviceWaitIdle,              "vkDeviceWaitIdle")
    #undef SYNTH_VK_GET

    // Optional, and the only optional one: the depth conversion pass binds its
    // two descriptors with vkCmdPushDescriptorSet through ReShade, and ReShade
    // asks for VK_KHR_push_descriptor on every device it creates but as an
    // optional extension -- so a driver without it is left without it. Resolved
    // here and reported by SynthDepthCsOpen, never called from this file.
    g_svk.have_push_desc = get_proc(g_svk.dev, "vkCmdPushDescriptorSetKHR") != nullptr ||
                           get_proc(g_svk.dev, "vkCmdPushDescriptorSet")    != nullptr;

    // The one extension gate, and the only one there is: everything above is
    // Vulkan 1.1 core, and the timeline semaphore feature the peer project hooks
    // vkCreateDevice to force on is not used here at all.
    //
    // It is asked as a question rather than assumed, because the two structures
    // the import is built from -- VkExternalMemoryImageCreateInfo and
    // VkImportMemoryWin32HandleInfoKHR -- are pNext chain entries, and chaining
    // one from an extension the device did not enable is undefined rather than
    // an error: a driver may ignore the chain and hand back an image bound to
    // ordinary local memory that shares nothing with D3D12, which is wrong
    // pixels and no message anywhere. vkGetMemoryWin32HandleKHR is a command of
    // that same extension, and a device command whose extension was not enabled
    // resolves to null -- so it is resolved and never called. The resolution is
    // the proof; vkAllocateMemory's result below is the check on the proof.
    //
    // UNVERIFIED: the loader is specified to return null here, and some loader
    // versions have been reported returning a trampoline for an extension that
    // is supported but not enabled. Treated as a strong hint and not as the only
    // guard, which is why the allocation result is tested as well.
    if (get_proc(g_svk.dev, "vkGetMemoryWin32HandleKHR") == nullptr)
    {
        Log("[synth] VK_KHR_external_memory_win32 is not enabled on this device -- "
            "vkGetMemoryWin32HandleKHR does not resolve -- so a D3D12 texture cannot be "
            "imported and this transport refuses rather than chain a structure from an "
            "extension that is not there. ReShade asks for that extension on every device "
            "it creates, but as an optional one, so a driver without it is left without it.");
        return false;
    }

    // From the same extension the gate above just proved, so it is resolved here
    // rather than in the core list, where a miss is fatal and the message would
    // say the device was created without a core command.
    g_svk.GetMemoryWin32HandleProperties =
        reinterpret_cast<PFN_SVkGetMemoryWin32HandleProperties>(
            get_proc(g_svk.dev, "vkGetMemoryWin32HandlePropertiesKHR"));

    Log("[synth] Vulkan side ready: VkDevice %p, VK_KHR_external_memory_win32 enabled, "
        "VK_KHR_push_descriptor %s, %d entry points resolved out of vulkan-1.dll with no "
        "import of it.", static_cast<void *>(g_svk.dev),
        g_svk.have_push_desc ? "enabled" : "NOT enabled", resolved);
    g_svk.ready = true;
    return true;
}

// Import the four D3D12 textures, and make the staging buffer depth travels
// through. Runs only after Synth12Build has created them and their NT handles.
// Every size comes off the D3D12 texture that was actually created, not off a
// pair of arguments: Synth12Build sizes Output on its own now, so one w,h could
// only ever be right for three of the four slots.
static bool SynthVkImport()
{
    bool said_memory = false;
    // The list of slots rather than a count, because the two optional textures
    // are independent: the exposure mirror belongs to the Vulkan mirror and the
    // second Output to the substitute's pipelined transport, and a count would
    // make importing the second imply importing the first. Everything inside the
    // loop reads the texture that was actually created, so neither needs a
    // special case: each scalar float format goes through SynthVkFormat
    // like any other, and the second Output is the first one's format by
    // construction.
    int idx[SLOT_COUNT + 2];
    int count = 0;
    for (int k = 0; k < SLOT_COUNT; ++k) idx[count++] = k;
    if (g_s12.tex[kExpTex] != nullptr) idx[count++] = kExpTex;
    if (g_s12.tex[kOutB]   != nullptr) idx[count++] = kOutB;
    for (int k = 0; k < count; ++k)
    {
        const int i = idx[k];
        const char *const nm = i < SLOT_COUNT ? kSlotName[i]
                             : i == kExpTex   ? "Exposure" : "Output B";
        // Read back off the texture that was actually created rather than
        // recomputed from back_fmt, so a slot whose format was decided elsewhere
        // cannot be imported as something else.
        const D3D12_RESOURCE_DESC rd = g_s12.tex[i]->GetDesc();
        const UINT w = static_cast<UINT>(rd.Width);
        const UINT h = rd.Height;
        const uint32_t vkfmt = SynthVkFormat(rd.Format);
        if (vkfmt == 0)
        {
            Log("[synth] %s is %s on the D3D12 side and this add-on has no Vulkan format "
                "for it, so the import refuses rather than guess one.",
                nm, FormatName(rd.Format));
            return false;
        }

        SVkExternalMemoryImageCreateInfo ext = {};
        ext.sType       = kSVkStExternalMemoryImage;
        ext.handleTypes = kSVkHandleTypeD3D12Res;

        SVkImageCreateInfo ici = {};
        ici.sType         = kSVkStImageCreateInfo;
        ici.pNext         = &ext;
        ici.imageType     = kSVkImageType2D;
        ici.format        = vkfmt;
        ici.extent.width  = w;
        ici.extent.height = h;
        ici.extent.depth  = 1;
        ici.mipLevels     = 1;
        ici.arrayLayers   = 1;
        ici.samples       = kSVkSampleCount1;
        ici.tiling        = kSVkTilingOptimal;
        // Transfer only. Nothing on the Vulkan side samples these or writes them
        // from a shader, and asking for STORAGE on a BGRA image is a creation
        // failure on drivers that do not support that pairing -- a refusal
        // earned by a usage flag nothing needs.
        ici.usage         = kSVkUsageTransferSrc | kSVkUsageTransferDst;
        ici.sharingMode   = kSVkSharingExclusive;
        ici.initialLayout = kSVkLayoutUndefined;

        uint32_t r = g_svk.CreateImage(g_svk.dev, &ici, nullptr, &g_svk.img[i]);
        if (r != kSVkSuccess)
        {
            Log("[synth] vkCreateImage refused the %ux%u %s image for %s with VkResult %d",
                w, h, FormatName(rd.Format), nm, static_cast<int>(r));
            return false;
        }

        SVkMemoryRequirements req = {};
        g_svk.GetImageMemoryRequirements(g_svk.dev, g_svk.img[i], &req);

        // ponytail: the lowest allowed memory type. ReShade exposes no
        // VkPhysicalDevice -- it keeps that handle private and get_native
        // answers for the device, the queue and the swapchain only -- so
        // vkGetPhysicalDeviceMemoryProperties cannot be called and the types
        // cannot be inspected. Every bit in memoryTypeBits is a type the driver
        // will accept for this image, so this is correct by the specification
        // and at worst slow, and the driver has already narrowed the set to what
        // imported D3D12 memory can live in. The bits and the choice are logged
        // because a report is the only thing that can settle it on a vendor this
        // has not run on.
        // The intersection of two sets, not one of them. The image's
        // memoryTypeBits says what the image can bind to; the handle's says what
        // THIS D3D12 allocation can be imported as, and the specification requires
        // the index to come from the second. Taking the lowest bit of the first
        // alone is what the Khronos validation layer reported four times a run as
        // VUID-VkMemoryAllocateInfo-memoryTypeIndex-00645 -- it happened to work
        // on this driver, where both sets are 0x3, and is not portable.
        SVkMemoryWin32HandleProperties hp = {};
        hp.sType = kSVkStMemoryWin32Props;
        const uint32_t hr_props = g_svk.GetMemoryWin32HandleProperties != nullptr
            ? g_svk.GetMemoryWin32HandleProperties(g_svk.dev, kSVkHandleTypeD3D12Res,
                                                   g_s12.share[i], &hp)
            : 0xFFFFFFFFu;
        const uint32_t allowed = hr_props == kSVkSuccess && hp.memoryTypeBits != 0
                                     ? (req.memoryTypeBits & hp.memoryTypeBits)
                                     : req.memoryTypeBits;
        if (allowed == 0)
        {
            Log("[synth] no memory type is accepted by both the %s image (0x%08X) and its "
                "imported D3D12 handle (0x%08X), so the import is refused rather than "
                "allocated from a type the specification forbids.",
                nm, req.memoryTypeBits, hp.memoryTypeBits);
            return false;
        }
        uint32_t type_index = 0;
        for (uint32_t b = 0; b < 32; ++b)
            if (allowed & (1u << b)) { type_index = b; break; }
        if (!said_memory)
        {
            said_memory = true;
            Log("[synth] imported memory: the image allows types 0x%08X, the imported "
                "handle allows 0x%08X (query returned %u), and the lowest of the two "
                "together is taken, index %u. UNVERIFIED on AMD and on Intel -- if the "
                "import fails or the picture is wrong on one of those, this line is the "
                "first thing to compare.",
                req.memoryTypeBits, hp.memoryTypeBits, hr_props, type_index);
        }

        // Mandatory for an imported D3D12 resource: the allocation is that one
        // resource, and nothing else can be suballocated out of it.
        //
        // ASSUMED, NOT MEASURED, and stated here because it is the one pNext
        // entry above that has no resolve behind it. VkMemoryDedicatedAllocateInfo
        // is core in Vulkan 1.1; on a 1.0 device it belongs to
        // VK_KHR_dedicated_allocation, which ReShade does not ask for and which
        // has no device command to resolve the way vkGetMemoryWin32HandleKHR
        // proves the win32 extension. What stands in for a resolve is that
        // VK_KHR_external_memory_win32 requires VK_KHR_external_memory, and that
        // one is core in 1.1 as well and is not on ReShade's list either -- so a
        // device that got this far is a 1.1 device or the extension gate above
        // would already have refused it. If a driver is ever reported importing
        // memory that shares nothing, this premise is the first thing to test.
        SVkMemoryDedicatedAllocateInfo ded = {};
        ded.sType = kSVkStMemoryDedicated;
        ded.image = g_svk.img[i];

        SVkImportMemoryWin32HandleInfoKHR imp = {};
        imp.sType      = kSVkStImportMemoryWin32;
        imp.pNext      = &ded;
        imp.handleType = kSVkHandleTypeD3D12Res;
        // Duplicated by the driver rather than consumed, so this side still owns
        // the handle and Synth12ReleaseTextures still has to close it.
        imp.handle     = g_s12.share[i];

        SVkMemoryAllocateInfo mai = {};
        mai.sType           = kSVkStMemoryAllocateInfo;
        mai.pNext           = &imp;
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = type_index;

        r = g_svk.AllocateMemory(g_svk.dev, &mai, nullptr, &g_svk.mem[i]);
        if (r != kSVkSuccess)
        {
            Log("[synth] vkAllocateMemory refused the D3D12 shared handle for %s with "
                "VkResult %d. The likeliest cause is that the D3D12 device this add-on "
                "created is on a different GPU from the one the game renders with, which "
                "nothing here can see: there is no VkPhysicalDevice to read a LUID off.",
                nm, static_cast<int>(r));
            return false;
        }
        r = g_svk.BindImageMemory(g_svk.dev, g_svk.img[i], g_svk.mem[i], 0);
        if (r != kSVkSuccess)
        {
            Log("[synth] vkBindImageMemory failed for %s with VkResult %d",
                nm, static_cast<int>(r));
            return false;
        }
    }

    // Sized off the depth slot itself, which on the mirror is the game's render
    // size rather than anything the colour slots are.
    const D3D12_RESOURCE_DESC dd = g_s12.tex[SLOT_DEPTH]->GetDesc();
    SVkBufferCreateInfo bci = {};
    bci.sType       = kSVkStBufferCreateInfo;
    // One 32-bit word per texel either way: the depth aspect of D32_SFLOAT and
    // the depth aspect of D24_UNORM_S8_UINT -- which the spec addresses as
    // X8_D24_UNORM_PACK32 -- are both four bytes wide, so the packed case needs
    // nothing from this size math.
    bci.size        = static_cast<uint64_t>(dd.Width) * dd.Height * 4;
    bci.usage       = kSVkUsageTransferSrc | kSVkUsageTransferDst;
    bci.sharingMode = kSVkSharingExclusive;
    if (g_svk.CreateBuffer(g_svk.dev, &bci, nullptr, &g_svk.buf) != kSVkSuccess)
    {
        Log("[synth] vkCreateBuffer refused the %llu-byte depth staging buffer", bci.size);
        return false;
    }

    SVkMemoryRequirements breq = {};
    g_svk.GetBufferMemoryRequirements(g_svk.dev, g_svk.buf, &breq);
    // Same choice and the same reasoning as above, and here it is not even a
    // heuristic: this buffer is not imported, so any type the driver allows is
    // as correct as any other.
    uint32_t btype = 0;
    for (uint32_t b = 0; b < 32; ++b)
        if (breq.memoryTypeBits & (1u << b)) { btype = b; break; }

    SVkMemoryAllocateInfo bmai = {};
    bmai.sType           = kSVkStMemoryAllocateInfo;
    bmai.allocationSize  = breq.size;
    bmai.memoryTypeIndex = btype;
    if (g_svk.AllocateMemory(g_svk.dev, &bmai, nullptr, &g_svk.buf_mem) != kSVkSuccess ||
        g_svk.BindBufferMemory(g_svk.dev, g_svk.buf, g_svk.buf_mem, 0) != kSVkSuccess)
    {
        Log("[synth] the depth staging buffer could not be backed with memory");
        return false;
    }

    Log("[synth] %d D3D12 textures imported into the game's VkDevice -- the four slots%s%s "
        "-- and a %llu-byte buffer made for the depth aspect to travel through.",
        count,
        g_s12.tex[kExpTex] != nullptr ? ", the game's exposure texture" : "",
        g_s12.tex[kOutB]   != nullptr ? ", and the pipeline's second Output" : "",
        breq.size);
    return true;
}

// ---------------------------------------------------------------------------
// The depth conversion pass
//
// This is what unblocks the synthetic Vulkan path, and it clears exactly two of
// the three obstacles Baldur's Gate 3 reported on 2026-08-31.
//
//   FORMAT.  The transport carried depth out through a buffer and back in as
//            R32_FLOAT, which is exact for a 32-bit float depth aspect and for
//            nothing else -- a D24 aspect copies out as a packed unnormalised
//            integer. A SAMPLE does not care: texelFetch on a depth-aspect view
//            returns the driver-normalised float for D16, D24S8 and D32 alike,
//            which is raw depth exactly as NGX wants it. No linearisation, no
//            near or far plane, and reversed-Z still carried by the
//            DepthInverted create flag as it already was.
//
//   USAGE.   ReShade's DEPTH semantic on Vulkan is bound to generic_depth's own
//            backup texture, created shader_resource | copy_dest with no
//            copy_source, so no transfer command may read it. shader_resource
//            IS VK_IMAGE_USAGE_SAMPLED_BIT: a copy cannot be issued, a sample
//            can.
//
// The SIZE obstacle is NOT cleared and deliberately is not. This pass is one
// texel in, one texel out, same size, no filtering and no scaling. It could
// rescale and make the refusal disappear; depth resampled to a size the game
// never rendered is invented data, and a game whose depth is smaller than its
// back buffer is running its own upscaler -- which is the Vulkan MIRROR's case
// by definition, and the mirror is the right path there.
//
// Through ReShade's add-on API rather than raw Vulkan, and that is not laziness
// for its own sake: ReShade is the only party that knows each image's aspect
// mask, which for the depth image is DEPTH and not COLOR, and a default
// resource_view_desc makes it derive both the format and the aspect from the
// image it already has private data for. It is also the only party that can
// create a descriptor set on the game's device without this add-on carrying a
// descriptor pool. push_descriptors deletes the pool, the per-slot sets and the
// whole "update a set that may still be in flight" problem before any of them
// exist.
//
// The pass writes into a texture of ITS OWN rather than straight into the
// imported D3D12 depth texture, and one full-resolution R32 image copy carries
// it the rest of the way. That is one extra copy -- about 24 MB at 3840x1600,
// under a tenth of a millisecond -- bought for three things: the destination is
// a resource ReShade created and therefore certainly has private data for,
// whereas an image this add-on made with its own vkCreateImage is registered
// with ReShade only if that call happened to travel down its layer; the
// imported image does not need VK_IMAGE_USAGE_STORAGE_BIT, which on imported
// D3D12 memory is a creation flag nothing here can test; and the D3D12 texture
// behind it does not need ALLOW_UNORDERED_ACCESS either. Three untestable
// premises traded for one copy.
// ---------------------------------------------------------------------------

#include "depth_convert_spv.h"

// The blob is data, and the one failure mode of a data file is that it stopped
// being the thing its header describes. Both numbers are in that header.
static_assert(sizeof(kDepthConvertSpv) == 1640,
              "depth_convert_spv.h is not the 1640-byte module its header describes");
static_assert(sizeof(kDepthConvertSpv) / sizeof(kDepthConvertSpv[0]) == 410,
              "depth_convert_spv.h is not the 410-word module its header describes");

struct SynthDepthCs
{
    bool tried;
    bool ready;
    reshade::api::device *dev;

    reshade::api::pipeline_layout layout;
    reshade::api::pipeline        pso;
    reshade::api::sampler         samp;

    // Ours, and the only thing the dispatch writes. R32_FLOAT at the game's
    // render size, and it never leaves copy_source except for the dispatch.
    reshade::api::resource      dst;
    reshade::api::resource_view dst_uav;
    UINT                        w, h;

    // The view over whatever the DEPTH semantic is bound to, cached because the
    // binding almost never changes, and retired TWO frames late because the
    // command buffer that names it has not been submitted yet when it is
    // replaced. Two, where this was one until the pipeline landed, and one is no
    // longer enough on any transport -- so it is a two-slot ring destroyed as it
    // comes round again rather than a per-mode branch.
    //
    // What the two frames buy, taking the pipeline as the weakest case: at the
    // top of frame N+2 the render thread has waited the free event of the pair
    // frame N armed, and the worker sets that only after it has SEEN frame N's
    // ev_in signalled by the GPU -- so frame N-1's command buffer, the last one
    // that named the view retired at frame N, has completed. One frame would
    // prove only that frame N-2's had, because under the pipeline the wait at the
    // top of frame N+1 is for the worker of frame N-1.
    //
    // The ring counts CALLS of this function, not arms, and the difference is
    // what the margin is really for: a call that returns without arming -- a
    // refused depth view, a frame that could not take a pair, a teardown race on
    // the event handles -- advances the ring and proves nothing. Two calls late
    // is the arithmetic; two arms late is what the paragraph above proves.
    // Retiring on the arm ordinal instead would make them the same sentence.
    //
    // Serialised, the wait_idle in the MIDDLE of the previous frame already says
    // more than either -- it covers everything recorded before it, and the view
    // being destroyed here was last named a frame further back. Parked, one frame said something a step weaker than the serialised
    // case -- queue order rather than a retired command buffer -- and two says the
    // same thing with a frame of margin on it.
    //
    // On the stall branch the worker sets its free event having seen NOTHING, so
    // there the proof is void by construction: the stall IS the GPU not having
    // got there. That is the frame the re-check after the wait in SynthVkFrame
    // exists to stop, and it is what that one line is protecting.
    uint64_t                    src_res;
    reshade::api::resource_view src_srv;
    reshade::api::resource_view retire_srv[2];
    int                         retire_i;

    UINT64 frames;
};

static SynthDepthCs g_dcs;

#include "present_spv.h"

// The result drawn into the back buffer through a render pass, for a swapchain
// created without VK_IMAGE_USAGE_TRANSFER_DST_BIT. ReShade's layer adds
// COLOR_ATTACHMENT and TRANSFER_SRC to every swapchain it sees and nothing
// else, so a game that asked for neither transfer bit -- Ghost Recon
// Breakpoint on Vulkan, #23 -- leaves an image that can be copied out of and
// not into. It can always be rendered into: that bit ReShade guarantees.
//
// The result crosses one extra image on the way. The imported D3D12 output is
// transfer-only by construction (see the import), so it is copied into a
// sampleable texture of ReShade's making, and that one is drawn with a point
// sampler at the same size: a copy, by other means.
struct SynthVkPresent
{
    bool tried, ready;
    reshade::api::device         *dev;
    reshade::api::pipeline_layout layout;
    reshade::api::pipeline        pso;
    reshade::api::sampler         samp;
    DXGI_FORMAT                   pso_fmt;

    reshade::api::resource        mid;
    reshade::api::resource_view   mid_srv;
    UINT                          w, h;
    DXGI_FORMAT                   mid_fmt;
};

static SynthVkPresent g_vkp;

// The per-size objects; the pipeline stays.
static void SynthVkPresentRelease()
{
    if (g_vkp.dev == nullptr) return;
    if (g_vkp.mid_srv.handle != 0)
    { g_vkp.dev->destroy_resource_view(g_vkp.mid_srv); g_vkp.mid_srv = reshade::api::resource_view { 0 }; }
    if (g_vkp.mid.handle != 0)
    { g_vkp.dev->destroy_resource(g_vkp.mid); g_vkp.mid = reshade::api::resource { 0 }; }
    g_vkp.w = g_vkp.h = 0;
}

// Everything, before the device goes.
static void SynthVkPresentClose()
{
    SynthVkPresentRelease();
    if (g_vkp.dev == nullptr) return;
    if (g_vkp.pso.handle != 0)
    { g_vkp.dev->destroy_pipeline(g_vkp.pso); g_vkp.pso = reshade::api::pipeline { 0 }; }
    if (g_vkp.layout.handle != 0)
    { g_vkp.dev->destroy_pipeline_layout(g_vkp.layout); g_vkp.layout = reshade::api::pipeline_layout { 0 }; }
    if (g_vkp.samp.handle != 0)
    { g_vkp.dev->destroy_sampler(g_vkp.samp); g_vkp.samp = reshade::api::sampler { 0 }; }
    g_vkp.tried = g_vkp.ready = false;
    g_vkp.dev = nullptr;
}

static bool SynthVkPresentOpen(reshade::api::device *dev, DXGI_FORMAT fmt)
{
    using namespace reshade::api;

    if (g_vkp.ready && g_vkp.pso_fmt == fmt) return true;
    if (g_vkp.tried && !g_vkp.ready) return false;
    // The pipeline names its target format, so another back buffer format is
    // another pipeline. The layout and the sampler carry over.
    if (g_vkp.ready)
    { g_vkp.dev->destroy_pipeline(g_vkp.pso); g_vkp.pso = pipeline { 0 }; g_vkp.ready = false; }
    g_vkp.tried = true;
    g_vkp.dev   = dev;
    if (dev == nullptr) return false;

    if (g_vkp.samp.handle == 0)
    {
        sampler_desc sd = {};
        sd.filter    = filter_mode::min_mag_mip_point;
        sd.address_u = texture_address_mode::clamp;
        sd.address_v = texture_address_mode::clamp;
        sd.address_w = texture_address_mode::clamp;
        if (!dev->create_sampler(sd, &g_vkp.samp))
        { Log("[synth] the present sampler could not be created."); return false; }
    }
    if (g_vkp.layout.handle == 0)
    {
        static const descriptor_range range = {
            /*binding*/ 0, /*dx_register_index*/ 0, /*dx_register_space*/ 0, /*count*/ 1,
            /*visibility*/ shader_stage::pixel, /*array_size*/ 1,
            descriptor_type::sampler_with_resource_view };
        pipeline_layout_param param = {};
        param.type                    = pipeline_layout_param_type::push_descriptors_with_ranges;
        param.descriptor_table.count  = 1;
        param.descriptor_table.ranges = &range;
        if (!dev->create_pipeline_layout(1, &param, &g_vkp.layout))
        { Log("[synth] the present pipeline layout could not be created."); return false; }
    }

    shader_desc vs = {};
    vs.code      = kPresentVertSpv;
    vs.code_size = sizeof(kPresentVertSpv);
    shader_desc ps = {};
    ps.code      = kPresentFragSpv;
    ps.code_size = sizeof(kPresentFragSpv);
    format             rtf  = static_cast<format>(fmt);
    primitive_topology topo = primitive_topology::triangle_list;
    rasterizer_desc    rs   = {};
    rs.cull_mode = cull_mode::none;
    depth_stencil_desc ds   = {};
    ds.depth_enable     = false;
    ds.depth_write_mask = false;
    blend_desc         bs   = {};

    pipeline_subobject subs[7] = {
        { pipeline_subobject_type::vertex_shader,         1, &vs },
        { pipeline_subobject_type::pixel_shader,          1, &ps },
        { pipeline_subobject_type::render_target_formats, 1, &rtf },
        { pipeline_subobject_type::primitive_topology,    1, &topo },
        { pipeline_subobject_type::rasterizer_state,      1, &rs },
        { pipeline_subobject_type::depth_stencil_state,   1, &ds },
        { pipeline_subobject_type::blend_state,           1, &bs },
    };
    if (!dev->create_pipeline(g_vkp.layout, 7, subs, &g_vkp.pso))
    {
        Log("[synth] the present pipeline could not be created for %s. The two SPIR-V "
            "modules are the ones present_spv.h describes; a refusal here is the driver's.",
            FormatName(fmt));
        return false;
    }
    g_vkp.pso_fmt = fmt;
    g_vkp.ready   = true;
    Log("[synth] present pass built: the result is drawn into the %s back buffer with one "
        "fullscreen triangle instead of copied, because the image was not created to be "
        "copied into.", FormatName(fmt));
    return true;
}

static bool SynthVkPresentSize(UINT w, UINT h, DXGI_FORMAT fmt)
{
    using namespace reshade::api;

    if (g_vkp.mid.handle != 0 && g_vkp.w == w && g_vkp.h == h && g_vkp.mid_fmt == fmt) return true;
    SynthVkPresentRelease();

    const resource_desc rd(w, h, 1, 1, static_cast<format>(fmt), 1, memory_heap::default_,
                           resource_usage::shader_resource | resource_usage::copy_dest);
    if (!g_vkp.dev->create_resource(rd, nullptr, resource_usage::shader_resource, &g_vkp.mid))
    {
        Log("[synth] the %ux%u %s texture the present pass samples could not be created.",
            w, h, FormatName(fmt));
        return false;
    }
    if (!g_vkp.dev->create_resource_view(g_vkp.mid, resource_usage::shader_resource,
                                         resource_view_desc(static_cast<format>(fmt)), &g_vkp.mid_srv))
    {
        Log("[synth] no shader resource view could be made over the present pass's texture.");
        SynthVkPresentRelease();
        return false;
    }
    g_vkp.w = w;
    g_vkp.h = h;
    g_vkp.mid_fmt = fmt;
    return true;
}

// Records into the caller's command list. The back buffer arrives in
// copy_source and leaves in render_target, as it does on the copy path.
// out is the Output image the caller decided to read back, passed rather than
// looked up: the pipeline alternates two of them and only the caller knows which
// pair's wait it just recorded.
static void SynthVkPresentDraw(reshade::api::command_list *cmd, SVkCommandBuffer cb,
                               reshade::api::resource back, reshade::api::resource_view rtv,
                               UINT w, UINT h, SVkHandle out)
{
    using namespace reshade::api;

    cmd->barrier(g_vkp.mid, resource_usage::shader_resource, resource_usage::copy_dest);
    SynthVkBarrier(cb, out, kSVkLayoutGeneral, kSVkLayoutGeneral);
    SynthVkCopyImage(cb, out, kSVkLayoutGeneral,
                     static_cast<SVkHandle>(g_vkp.mid.handle), kSVkLayoutTransferDst, w, h);
    cmd->barrier(g_vkp.mid, resource_usage::copy_dest, resource_usage::shader_resource);
    cmd->barrier(back, resource_usage::copy_source, resource_usage::render_target);

    render_pass_render_target_desc rt = {};
    rt.view     = rtv;
    rt.load_op  = render_pass_load_op::load;
    rt.store_op = render_pass_store_op::store;
    cmd->begin_render_pass(1, &rt, nullptr);
    const viewport vp = { 0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f };
    const rect     sc = { 0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h) };
    cmd->bind_viewports(0, 1, &vp);
    cmd->bind_scissor_rects(0, 1, &sc);
    cmd->bind_pipeline(pipeline_stage::all_graphics, g_vkp.pso);
    sampler_with_resource_view sv = { g_vkp.samp, g_vkp.mid_srv };
    descriptor_table_update u = {};
    u.binding     = 0;
    u.count       = 1;
    u.type        = descriptor_type::sampler_with_resource_view;
    u.descriptors = &sv;
    cmd->push_descriptors(shader_stage::pixel, g_vkp.layout, 0, u);
    cmd->draw(3, 1, 0, 0);
    cmd->end_render_pass();
}

// Built once. A failure is latched and never retried, the same rule Synth12Open
// and SynthVkOpen follow, and every branch that returns false names its own
// reason -- there is no path out of here that is silently off.
static bool SynthDepthCsOpen(reshade::api::device *dev)
{
    using namespace reshade::api;

    if (g_dcs.tried) return g_dcs.ready;
    g_dcs.tried = true;
    g_dcs.dev   = dev;

    if (dev == nullptr) return false;

    if (!dev->check_capability(device_caps::compute_shader))
    {
        Log("[synth] this device reports no compute support, so the depth conversion "
            "cannot be built and the synthetic contract has no depth to hand NGX.");
        return false;
    }
    if (dev->get_api() != device_api::vulkan)
    {
        // The same host code would serve D3D12 with the cs_5_0 blob bridge.inc
        // already compiles at runtime, and that is the upgrade path. It is not
        // built, because nothing on the D3D12 synthetic backend has been
        // measured asking for it and a second blob is a second thing to keep in
        // step.
        // ponytail: Vulkan only. Add the DXBC arm when a D3D12 title reports the
        // depth refusal this clears.
        Log("[synth] the depth conversion pass carries a SPIR-V module only, so it can be "
            "built on Vulkan and not on this runtime.");
        return false;
    }
    if (!g_svk.have_push_desc)
    {
        Log("[synth] VK_KHR_push_descriptor is not enabled on this device, so the depth "
            "conversion cannot bind its two descriptors. ReShade asks for that extension "
            "on every device it creates, but as an optional one, so a driver without it is "
            "left without it -- and ReShade's own push_descriptors then records nothing at "
            "all rather than failing, which would be silent wrong depth.");
        return false;
    }
    if (kDepthConvertSpv[0] != 0x07230203u)
    {
        Log("[synth] DEFECT: depth_convert_spv.h does not begin with the SPIR-V magic "
            "number, so the array is not a module. Nothing is dispatched.");
        return false;
    }

    // Point, because texelFetch reads no filter state at all -- the sampler
    // exists only because the module declares a combined image sampler. Clamp
    // for the same reason: nothing outside the image is ever addressed.
    sampler_desc sd = {};
    sd.filter    = filter_mode::min_mag_mip_point;
    sd.address_u = texture_address_mode::clamp;
    sd.address_v = texture_address_mode::clamp;
    sd.address_w = texture_address_mode::clamp;
    if (!dev->create_sampler(sd, &g_dcs.samp))
    {
        Log("[synth] the depth conversion sampler could not be created.");
        return false;
    }

    // One set with two bindings, matching the module: binding 0 the combined
    // image sampler, binding 1 the r32f storage image. Both in ONE parameter,
    // because ReShade maps one parameter to one descriptor set and the module
    // puts both bindings in set 0.
    static const descriptor_range ranges[2] = {
        { /*binding*/ 0, /*dx_register_index*/ 0, /*dx_register_space*/ 0, /*count*/ 1,
          /*visibility*/ shader_stage::compute, /*array_size*/ 1,
          descriptor_type::sampler_with_resource_view },
        { 1, 0, 0, 1, shader_stage::compute, 1, descriptor_type::unordered_access_view },
    };

    pipeline_layout_param params[2] = {};
    params[0].type                    = pipeline_layout_param_type::push_descriptors_with_ranges;
    params[0].descriptor_table.count  = 2;
    params[0].descriptor_table.ranges = ranges;
    // A push constant parameter consumes no descriptor set, so the descriptors
    // above stay in set 0 whichever order these two are written in.
    params[1].type                      = pipeline_layout_param_type::push_constants;
    params[1].push_constants.binding    = 0;   // offset, in 32-bit values
    params[1].push_constants.count      = 2;   // uvec2 extent
    params[1].push_constants.visibility = shader_stage::compute;

    if (!dev->create_pipeline_layout(2, params, &g_dcs.layout))
    {
        Log("[synth] the depth conversion pipeline layout could not be created.");
        dev->destroy_sampler(g_dcs.samp);
        g_dcs.samp = sampler { 0 };
        return false;
    }

    shader_desc cs = {};
    cs.code      = kDepthConvertSpv;
    cs.code_size = sizeof(kDepthConvertSpv);

    pipeline_subobject sub = {};
    sub.type  = pipeline_subobject_type::compute_shader;
    sub.count = 1;
    sub.data  = &cs;

    if (!dev->create_pipeline(g_dcs.layout, 1, &sub, &g_dcs.pso))
    {
        Log("[synth] the depth conversion pipeline could not be created from the %u-byte "
            "SPIR-V module. The module is the one depth_convert_spv.h describes; a refusal "
            "here is the driver's, and vkCreateComputePipelines does not report why.",
            static_cast<unsigned>(sizeof(kDepthConvertSpv)));
        dev->destroy_pipeline_layout(g_dcs.layout);
        dev->destroy_sampler(g_dcs.samp);
        g_dcs.layout = pipeline_layout { 0 };
        g_dcs.samp   = sampler { 0 };
        return false;
    }

    Log("[synth] depth conversion built: one 8x8 compute pass that samples a DEPTH-aspect "
        "view and stores R32_FLOAT, so the depth binding no longer has to be 32-bit float "
        "and no longer has to be a copy source. It is still required to be the size of the "
        "back buffer, and that is not something this pass may fix.");
    g_dcs.ready = true;
    return true;
}

static void SynthDepthCsRelease()
{
    if (g_dcs.dev == nullptr) return;
    for (int i = 0; i < 2; ++i)
        if (g_dcs.retire_srv[i].handle != 0)
        {
            g_dcs.dev->destroy_resource_view(g_dcs.retire_srv[i]);
            g_dcs.retire_srv[i] = reshade::api::resource_view { 0 };
        }
    if (g_dcs.src_srv.handle != 0)
    { g_dcs.dev->destroy_resource_view(g_dcs.src_srv); g_dcs.src_srv = reshade::api::resource_view { 0 }; }
    if (g_dcs.dst_uav.handle != 0)
    { g_dcs.dev->destroy_resource_view(g_dcs.dst_uav); g_dcs.dst_uav = reshade::api::resource_view { 0 }; }
    if (g_dcs.dst.handle != 0)
    { g_dcs.dev->destroy_resource(g_dcs.dst); g_dcs.dst = reshade::api::resource { 0 }; }
    g_dcs.src_res = 0;
    g_dcs.w = 0;
    g_dcs.h = 0;
}

// The destination, sized to the frame. Created in copy_source and left there
// between frames, so every frame's barrier pair is the same two states -- a
// barrier from a state a resource is not in is the one mistake this class of
// code makes.
static bool SynthDepthCsSize(UINT w, UINT h)
{
    using namespace reshade::api;

    if (g_dcs.dst.handle != 0 && g_dcs.w == w && g_dcs.h == h) return true;

    if (g_dcs.dst_uav.handle != 0)
    { g_dcs.dev->destroy_resource_view(g_dcs.dst_uav); g_dcs.dst_uav = resource_view { 0 }; }
    if (g_dcs.dst.handle != 0)
    { g_dcs.dev->destroy_resource(g_dcs.dst); g_dcs.dst = resource { 0 }; }

    const resource_desc rd(w, h, 1, 1, format::r32_float, 1, memory_heap::default_,
                           resource_usage::unordered_access | resource_usage::copy_source);
    if (!g_dcs.dev->create_resource(rd, nullptr, resource_usage::copy_source, &g_dcs.dst))
    {
        Log("[synth] the %ux%u R32_FLOAT texture the depth conversion writes into could not "
            "be created.", w, h);
        return false;
    }
    if (!g_dcs.dev->create_resource_view(g_dcs.dst, resource_usage::unordered_access,
                                         resource_view_desc(format::r32_float), &g_dcs.dst_uav))
    {
        Log("[synth] no unordered access view could be made over the depth conversion's own "
            "R32_FLOAT texture.");
        g_dcs.dev->destroy_resource(g_dcs.dst);
        g_dcs.dst = resource { 0 };
        return false;
    }
    g_dcs.w = w;
    g_dcs.h = h;
    return true;
}

// One frame. Records into the caller's command list and returns the texture the
// result is in, or a null handle with a reason already logged.
//
// No layout transition on the SOURCE, and that is deliberate rather than an
// omission: generic_depth's own reshade_begin_effects handler leaves the depth
// binding in shader_resource, which on Vulkan is SHADER_READ_ONLY_OPTIMAL --
// exactly the layout push_descriptors declares for a combined image sampler.
// Transitioning it would be transitioning it to the state it is already in. The
// same-state barrier below is there for the memory dependency alone, so the
// copy generic_depth made is visible to this read. The peer project dispatches
// its equivalent with a barrier on the destination and none on the source.
static reshade::api::resource SynthDepthCsRun(reshade::api::command_list *cmd,
                                              reshade::api::resource depth, UINT w, UINT h)
{
    using namespace reshade::api;

    if (!g_dcs.ready || cmd == nullptr) return resource { 0 };
    if (!SynthDepthCsSize(w, h)) { g_dcs.ready = false; return resource { 0 }; }

    // The slot this frame owns, holding whatever the frame two back put in it.
    // Nothing still in flight names it -- see the field's own note, which states
    // what makes that true on each of the three transports.
    const int ri = g_dcs.retire_i;
    g_dcs.retire_i = ri ^ 1;
    if (g_dcs.retire_srv[ri].handle != 0)
    {
        g_dcs.dev->destroy_resource_view(g_dcs.retire_srv[ri]);
        g_dcs.retire_srv[ri] = resource_view { 0 };
    }

    if (g_dcs.src_srv.handle == 0 || g_dcs.src_res != depth.handle)
    {
        g_dcs.retire_srv[ri] = g_dcs.src_srv;
        g_dcs.src_srv        = resource_view { 0 };
        g_dcs.src_res        = 0;

        // A default-constructed desc, and the default is the point: ReShade then
        // takes the format off the image's own create info and derives the
        // aspect mask from it -- DEPTH for a depth format, with STENCIL removed
        // because the usage says shader_resource. Naming a format here would be
        // this add-on guessing at an aspect ReShade already knows.
        if (!g_dcs.dev->create_resource_view(depth, resource_usage::shader_resource,
                                             resource_view_desc(), &g_dcs.src_srv))
        {
            static bool said;
            if (!said)
            {
                said = true;
                Log("[synth] no shader resource view could be made over the depth binding, "
                    "so the depth conversion cannot read it. On Vulkan that binding is "
                    "generic_depth's own backup texture and is created shader_resource, so "
                    "a refusal here is a surprise rather than the expected case.");
            }
            return resource { 0 };
        }
        g_dcs.src_res = depth.handle;
    }

    cmd->barrier(depth, resource_usage::shader_resource, resource_usage::shader_resource);
    cmd->barrier(g_dcs.dst, resource_usage::copy_source, resource_usage::unordered_access);

    cmd->bind_pipeline(pipeline_stage::compute_shader, g_dcs.pso);

    sampler_with_resource_view swv = {};
    swv.sampler = g_dcs.samp;
    swv.view    = g_dcs.src_srv;

    descriptor_table_update u = {};
    u.binding     = 0;
    u.count       = 1;
    u.type        = descriptor_type::sampler_with_resource_view;
    u.descriptors = &swv;
    cmd->push_descriptors(shader_stage::compute, g_dcs.layout, 0, u);

    u.binding     = 1;
    u.type        = descriptor_type::unordered_access_view;
    u.descriptors = &g_dcs.dst_uav;
    cmd->push_descriptors(shader_stage::compute, g_dcs.layout, 0, u);

    // The module bounds-checks against this rather than against imageSize, so it
    // is the one value that must be right or the edge of the picture is written
    // from a texel that was never fetched.
    const uint32_t extent[2] = { w, h };
    cmd->push_constants(shader_stage::compute, g_dcs.layout, 1, 0, 2, extent);

    // 8 by 8, which is what the module declares.
    cmd->dispatch((w + 7) / 8, (h + 7) / 8, 1);

    cmd->barrier(g_dcs.dst, resource_usage::unordered_access, resource_usage::copy_source);

    if (++g_dcs.frames == 1)
        Log("[synth] depth conversion: first dispatch, %ux%u, %u by %u groups.",
            w, h, (w + 7) / 8, (h + 7) / 8);
    return g_dcs.dst;
}

// The driver's optical flow engine, which on this transport runs on a private
// D3D11 device sharing this file's own D3D12 textures. Declared here and defined
// beside the rest of the optical flow code, because that is where OfaFrameOn is
// and this is the one caller that lives above it.
static bool SynthOfaVkOpen(UINT w, UINT h, DXGI_FORMAT back_fmt);
// wait_ms is the budget for the two GPU waits inside it, and it is the caller's
// for the same reason Synth12EvaluateShared's is: under the park the game's
// queue is stopped until this returns.
static bool SynthOfaVkFrame(DWORD wait_ms = 4000);
// Whether the engine, and not a ReShade effect, is this session's motion vector
// source. True once it has opened AND once it has given up, because a session
// that gave up stops delivering rather than swapping estimator.
static bool SynthOfaVkIsSource();
// Whether the engine is running right now, and whether it gave up. IsSource above
// is true for BOTH, on purpose; these two tell them apart for the one caller that
// has to. g_ovk is defined far below.
static bool SynthOfaVkReady();
static bool SynthOfaVkStopped();
// Dropped and allowed to open again when the shared textures it aliases are
// replaced, which is what a resolution or format change does.
static void SynthOfaVkRebuild();

// ---------------------------------------------------------------------------
// The park
//
// What the note above the Vulkan declarations calls the price of not importing
// the fence: rq->wait_idle() drains the game's queue on the CPU in the middle of
// reshade_begin_effects, and the optical flow and the D3D12 evaluate then run
// with the game's render thread stopped behind them. Ghost Recon Breakpoint on
// an RTX 4070 SUPER, 1920x1080, reported ~150 fps of its own against ~75 with
// the substitute contract and the GPU 46% busy (#23). The work is not what
// costs; the serialisation is.
//
// So the Vulkan mirror's park, on this transport. The copies in are recorded, a
// vkCmdSetEvent announces them, a vkCmdWaitEvents on a second event stops the
// game's QUEUE at that point instead of its CPU, and the copy back is recorded
// after the wait. A worker thread sees the first event, runs the optical flow
// and the D3D12 evaluate on the private devices, and host-sets the second.
// Nothing is hooked and no extension is needed -- vkmirror.inc's header states
// why that is the whole reason this shape exists, and why the Khronos validation
// layer reports two VUIDs for it that are the park rather than a mistake.
//
// Three things differ from the mirror's, and each is a decision rather than an
// accident of where the code sits:
//
//   The command buffer is ReShade's own immediate one and not the game's. So the
//   park is submitted by ReShade at the end of the frame it was recorded in, and
//   everything ReShade records after this -- every technique, and its blit to the
//   swapchain -- is behind the wait as well. That is what makes releasing the
//   event unconditional here too, and for a worse consequence than a dropped
//   frame.
//
//   The worker runs the optical flow as well as the evaluate. On this transport
//   the vectors are computed between the copies and the evaluate, and its own
//   CPU wait on an ID3D11Query is exactly the second half of what is being moved
//   off the present thread.
//
//   A stall RETIRES this transport rather than draining. The mirror keeps going
//   because a game that has its own DLSS loses nothing when a mirrored frame is
//   skipped; here the substitute IS the picture, and a queue that stops answering
//   is not a condition this transport can improve on by trying again.
//
//   Retiring does NOT by itself make the reuse question moot, and this comment
//   said it did. The stall is decided on the worker thread while the render
//   thread is already inside the idle wait for that same worker, so without a
//   re-check after that wait exactly one more frame records and resets a pair
//   whose vkCmdWaitEvents is still queued. What bounds the reset is the
//   unconditional release at the foot of the worker's loop -- the queue is let
//   past 300 ms later rather than held -- and what stops the frame that would
//   exercise it is the re-check after the idle wait below, which is the mirror's
//   own (vkmirror.inc:1434-1436). The reset is not the damage anyway: the
//   depth view retired at the top of that frame is, because the proof that lets
//   SynthDepthCsRun destroy it is the worker having SEEN the GPU reach the
//   copies, and on the stall branch it has seen nothing.
//
// The two alternating pairs are the mirror's and the proof is the mirror's, in
// its own words: at the top of frame N+2 the render thread has waited on idle,
// which the worker sets only after it has seen frame N+1's ev_in signalled by
// the GPU, so everything frame N recorded -- its vkCmdWaitEvents included -- is
// earlier in queue order and has completed. One queue, one submission order.
//
// vk_sync=1 in dlss5-bridge.cfg puts the serialisation back. It is the fallback
// a user is told to try when the park misbehaves on their driver, and it is this
// transport exactly as 1.4.8 shipped it.
//
// THE PIPELINE, vk_sync=2, and it is one event and one line moved.
//
// Under the park frame N arms pair p and then waits on ev_out[p] -- the pair it
// just armed -- so the queue cannot pass until the worker woken a few lines
// earlier is done. Under the pipeline frame N waits on ev_out[prev], the pair
// the PREVIOUS frame armed, and it waits FIRST: the recorded order is
// vkCmdWaitEvents(ev_out[prev]), then the barriers, then the copies in, then
// vkCmdSetEvent(ev_in[p]), then the copy back out of the previous frame's
// Output. The evaluate the queue waits for was started a whole frame ago, so
// what the wait costs is whatever is left of it rather than the whole of it.
//
// It is still a wait, and whether the queue stops there is a race with the
// worker rather than a promise. The only direct evidence collected is the count
// the delivered line prints -- waits already satisfied at the moment they were
// recorded -- and it says both things, measured in ngxGym on 2026-09-04:
// 0 of about 600 in every window of synth-vk-pipeline, where the host renders
// one frame at a time and waits on its own fence and so has nothing between two
// presents to overlap the evaluate with; 593 of 600 in synth-vk-handover, where
// the same host had been running the game's own DLSS first. So the queue does
// stop at it, on a host with no work of its own. What the pipeline changes is
// what the queue is waiting FOR, not that it never waits.
//
// The set AFTER the wait is what makes the worker's two host resets sound, and
// that ordering is the whole design. A worker that has SEEN ev_in[p] has seen a
// command recorded later in the same buffer than the wait on ev_out[prev], so
// that wait has executed and ev_out[prev] can be reset -- it cannot be reset too
// early. And the render thread records no further command naming either event
// until it has waited free_ev[p], which that same worker sets last -- so it
// cannot be reset too late. Both halves are queue order plus one Win32 wait,
// which is the proof shape the two paragraphs above already use. Put the set
// before the wait, as the park does, and only the first half holds: the reset
// would then race a queue that may already be at the next frame's wait, and the
// copy back would read an Output the worker is writing. Neither the log nor the
// validation layer can see that, which is why the order is the design and not a
// tuning.
//
// ONE texture is doubled, not four. The copies into colour, depth and motion
// vectors are recorded after the wait, and on every branch that succeeds the
// worker set that event only after waiting its own D3D12 fence out, so the
// inputs are free by queue order. The exceptions are named in the budget block
// below: the two refusal branches and the exception branch return with the list
// still executing, so the frame AFTER a refusal is the one where the inputs are
// not free. Output is the one texture the worker writes and the game reads in
// the same frame, so there are two of them -- tex[SLOT_OUTPUT] and tex[kOutB] --
// and the arm's pair index says which.
//
// What that costs is one back buffer, because the substitute builds 1:1: the
// second Output is created at the back buffer's own width and height in the
// Output slot's own format, so it is width x height x bytes-per-texel and
// nothing else. 8.3 MB at 1920x1080 with a 4-byte format, 24.6 at 3840x1600,
// double each for RGBA16F.
//
// WHAT IT COSTS THE USER, and it is why this is opt-in and the park stays the
// default. The picture on screen is the previous present's: about 17 ms of extra
// latency at 60 fps and 33 at 30. And ReShade's own techniques run after this
// handler into the same back buffer, reading the CURRENT frame's depth over a
// colour that is one present old -- so a depth-driven effect, SSAO or DOF or
// fog, is a frame out of register with the image it shades. Named because it is
// real; nobody has measured how visible it is. The world and the HUD are one
// present old TOGETHER, because the result is copied or drawn over the whole
// back buffer rather than composited into it, so there is no seam inside the
// image itself.
//
// A THIRD VALIDATION VUID, and it is the pipeline in the same way the park's two
// are the park. Measured in ngxGym on 2026-09-04, once per frame:
//
//   VUID-vkResetEvent-event-03821  a host vkResetEvent on an event a submitted
//                                  command buffer waits on, with no execution
//                                  dependency the layer can see
//
// The dependency the specification asks for is there and is the one written out
// two paragraphs above: the worker resets ev_out[prev] only after the GPU has
// signalled ev_in[p], which was recorded LATER in the same command buffer than
// the wait on ev_out[prev], and vkCmdSetEvent with an ALL_COMMANDS stage mask
// means everything before it in submission order has completed. The layer tracks
// buffers rather than the commands inside them, so what it sees is a pending
// buffer that names the event. Documented rather than silenced, exactly as
// vkmirror.inc's note does for the other two.
//
// WHAT THE PIPELINE DOES NOT FIX, and it is the same thing the park does not
// fix. The total GPU work is unchanged: the optical flow and the evaluate are
// the same two submissions on the same GPU, and a GPU the game already saturates
// gains only the ordering. The queue can still stop at the wait -- it did
// throughout the gym, where the host has no work between presents -- and what
// moves is that it stops for what is LEFT of an evaluate that started a frame
// ago rather than for the whole of one that started this frame.
//
// Measured in ngxGym on 2026-09-04, RTX 5090, synth-vk-pipeline and two
// throwaway copies of it that differ only in vk_sync, so all three ran the same
// frames -- present thread inside this function, averaged over 600-frame windows
// at 2560x1440 and 1920x1080:
//
//   vk_sync=1  serialised  7.97 - 8.27 ms
//   vk_sync=0  park        6.31 - 7.04 ms
//   vk_sync=2  pipeline    1.02 - 2.15 ms
//
// with the worker's own body at 5.0-6.0 ms parked and 4.4-6.1 ms pipelined. The
// present thread is what this mode buys and the numbers are what it costs to
// check: the evaluate did not get faster.
//
// WHAT THE PARK DOES NOT FIX, measured in ngxGym on 2026-09-04, RTX 5090, the two
// transports run against each other on the same scenario. At 1920x1080 the
// present thread's own time inside this function fell from 8.06 ms to 4.41 ms a
// frame, and the game's queue was parked 4.41 ms; at 3840x1600 it fell from 9.04
// to 8.47 with 8.39 ms of park. So the CPU wait is gone and the GPU stall is
// not: the optical flow and the evaluate are real work on the same GPU, and
// while the queue is parked the game has nothing else in it to overlap them
// with. What this buys is the game's render thread, which is what a title with
// CPU work of its own between two presents can use -- and it is why the frame
// rate in the gym did not move at all: that host renders one frame at a time and
// waits for its own fence, so there was no CPU time there to give back.
// ---------------------------------------------------------------------------

// Budgets, in milliseconds, and every one of them is about the 2 s Windows TDR
// window rather than about what is comfortable. The game's GPU is stopped for
// the whole of the worker's body, so the worst case is the sum of everything it
// can wait on, and that sum has now been counted twice: six terms on an SDR
// presentation, eight on an HDR10 one. Counted end to end: 300 to see the copies, then the optical flow's colour copy
// -- which is the command allocator's retire fence FIRST and then the copy
// itself, 600 + 600 -- then its D3D11 query at 200, then the evaluate, which is
// again the allocator and then the evaluate, 600 + 600. That is
// 300 + 600 + 600 + 200 + 600 + 600 = 2900 ms.
//
// EIGHT terms and not six on an HDR10 presentation, which is where the sum used
// to be wrong twice over. Synth12EvaluateShared runs PqPassBefore before the
// evaluate and PqPassAfter after it, and each one waits the PQ pass's own fence:
// 600 + 600 more, for 4100. They were 4000 each and outside every budget in this
// file until PqPassWait was given the caller's -- two waits of twice the TDR
// window, inside a body whose whole design is that window. The PQ pass exists
// only when the back buffer is HDR10 and the source is not the mirror.
//
// So the worst case is past the TDR window and saying otherwise was the defect.
// What keeps it off a real machine is that the allocator's retire fence is
// normally already signalled: Synth12EvaluateShared waits its own submission out
// on every success, so the next Synth12Begin finds nothing to wait for. The
// paths that leave it pending are the ones that submit and return without
// waiting -- the NGX-refused branch and the HDR10 decode refusal -- which means
// the two 600s above are reached on the frame AFTER a refused evaluate, and on
// no other. Each individual wait is inside the window; a frame that reaches the
// sum has lost the device whatever this file does.
//
// The mirror's own budget is 300 + 900 + 900, because its worker waits on two
// things -- the allocator's retire fence inside Synth12Begin and then the
// evaluate -- where this one waits on five, or seven on an HDR10 presentation.
// The sum is what has to stay inside the window, not the individual number.
// ponytail: kVkmIdleMs is derived from the 1200 that arithmetic used to give and
// is therefore 700 ms shorter than the worker it guards, which is the failure its
// own comment says it exists to prevent. Named here rather than changed: raising
// it changes how long a mirrored game's render thread is held, which is the
// mirror's decision to make on the mirror's own evidence.
static const DWORD kSvpSeeCopiesMs = 300;
static const DWORD kSvpWorkMs      = 600;
// What the render thread gives the previous frame's worker before it decides the
// worker is stuck rather than slow. Derived from the SDR sum above and not
// chosen: a hand-picked number below it retires the transport on one
// slow-but-successful frame, which is why it followed the corrected sum up
// rather than staying at the 1900 the old two-wait arithmetic gave. The margin
// is the wake-up and the CPU frame between the two clocks.
//
// It deliberately does NOT follow the HDR10 sum up to 4300, and that is the one
// number here chosen by what the game can afford rather than by the worker: this
// wait runs inside g_bridge_cs -- SynthVkFrame is called from OnSynthBeginEffects
// between that Enter and its Leave -- and the game's own Vulkan DLSS detour takes
// the same section before VkMirrorFrame. So one stuck worker already stops the
// present thread AND the game's render thread, inside its own
// NVSDK_NGX_VULKAN_EvaluateFeature, for 3.1 s. The worker's worst case is what
// the retire line reports; this is what the game is made to wait, and on an
// HDR10 presentation a worker at its true worst case is retired one second
// early. Moving the wait above the Enter is the real answer and is a bigger
// rearrangement than it looks, since SynthVkFrame is called from inside.
//
// SynthVkDrainWorker takes this same budget a second time on the same thread,
// and the two are ADDITIVE within one forwarded evaluate: the mirror's rebuild
// spends kVkmIdleMs on g_vkm.idle and then this on the substitute's free events,
// so the number a report needs for a mirrored game with a stuck substitute
// worker is 1400 + 3100 = 4.5 s, not 3.1. The drain waits both free events in
// ONE WaitForMultipleObjects for exactly that reason -- two sequential waits
// would have made it 1400 + 6200.
static const DWORD kSvpIdleMs      = kSvpSeeCopiesMs + 4 * kSvpWorkMs + 200 + 200;

struct SynthVkPark
{
    // Off for the session: the events or the thread could not be made. The
    // transport then runs serialised, which is a slower frame and not a lost one.
    bool      off;
    // The pipeline's second Output could not be allocated, so vk_sync=2 falls to
    // the park for the session. Its own latch and not off, which would fall to
    // the serialised path instead: the park needs no second texture and is the
    // better of the two answers to "this machine has no VRAM for the pipeline".
    bool      no_pipe;
    // Which transport was last announced: 0 nothing yet, 1 parked, 2 serialised.
    // Not a bool, because vk_sync is re-read about once a second and a user who
    // flips it mid-session would otherwise change transport in silence with the
    // standing line still naming the other one -- on the one key whose whole
    // purpose is "try this if the park misbehaves on your driver".
    int       said;

    // This transport has retired itself: the game's queue did not reach the
    // copies, or the previous frame's park never cleared. Its own flag rather
    // than g_svk.ready, which is the shared latch saying whether the VULKAN SIDE
    // opened at all -- VkMirrorFrame reads that one through SynthVkOpen, and
    // turns a false into VkmRefuse, which is permanent for the session. A
    // substitute stall writing it would take the game's own mirrored DLSS down
    // with it, from the park's worker thread, for a hiccup that is not the
    // mirror's business.
    bool      retired;

    // Set FIRST in SynthVkParkStop, before anything is released, so a render
    // thread that has not yet armed cannot arm behind the teardown. VkmShutdown's
    // own order and for its reason. It narrows the window rather than closing it:
    // a thread already past both test points reads the four handles into locals,
    // which closes the half where they are zeroed under it, and SynthVkParkStop
    // destroys no VkEvent, which closes the half where the object is freed after
    // that read.
    volatile LONG stopping;

    // The join expired and the worker is still running. Everything it names is
    // then left standing on purpose -- see SynthVkParkStop -- and
    // SynthVkShutdownForDevice reads this to know it must free nothing.
    bool      orphaned;

    SVkHandle ev_in[2];    // device -> host: the copies in have executed
    SVkHandle ev_out[2];   // host -> device: the evaluate is done
    int       next;        // the pair the next frame takes; render thread only
    int       take;        // the pair the worker services next; worker only
    // Arms handed over and not yet finished. Never more than one under the park,
    // where the render thread waits both free events at the top of the frame, and
    // up to two under the pipeline, which is the whole point of it. The wake
    // event is auto reset, so two arms handed over while the worker is inside its
    // body arrive as one signal; this count is what tells it there are two.
    volatile LONG queued;
    // Manual reset, set while that pair carries no arm. Replaces the park's
    // single idle handle, and the pipeline is why there are two: frame N+1 must
    // be free to arm the other pair while the worker is still on this one.
    HANDLE    free_ev[2];
    HANDLE    wake;        // auto reset, kicks the worker
    HANDLE    worker;
    volatile LONG stop;

    // One handover. Written by the render thread while nothing reads that pair --
    // the free event above is the proof -- and read by the worker alone after
    // that. The contract is copied rather than pointed at: the present thread's
    // own SynthParams is a stack object in OnSynthBeginEffects and is gone by the
    // time the worker runs.
    struct SvpArm
    {
        SynthParams sp;
        bool        ofa_mv;
        bool        pipe;    // a pipelined arm: the worker owns the two resets
        int         pair;
        UINT64      ordinal; // which frame armed it, for the lag the log prints
    } arm[2];

    // An arm on that pair whose ev_out no frame has waited on yet. False on the
    // first frame of a session, after a rebuild, and after a frame that returned
    // early between the wait and the arm -- which is exactly when there is
    // nothing to wait for and nothing to copy back, and is why this is not
    // "armed", which would never become false again.
    bool      pending[2];

    // Whether the PREVIOUS delivering frame armed at all, and it is what makes
    // pending[] mean "armed one frame ago" rather than "armed at some point".
    // Cleared at the top of every SynthMaybeDeliver on a Vulkan runtime and set
    // only at the arm, so a frame that finds it false knows delivery stopped and
    // resumed -- however many frames the gap was -- and clears the pair state
    // there rather than at each of the dozen returns that can skip an arm. Those
    // returns are real and ordinary: SynthArmRefusal, a depth binding that flaps
    // as generic_depth swaps its backup, a render-target shape disagreement, a
    // live vk_present edit. Without it the first frame back waits an ev_out
    // signalled arbitrarily long ago and copies back that pair's Output -- a real
    // picture, from before the gap, however old the gap is. Render thread only.
    bool      armed_last;

    // Whether that pair's Output image holds a frame that was evaluated. The copy
    // back has to be RECORDED before the evaluate that fills it has run, so this
    // frame's decision is made on an earlier frame's result: false means the
    // game's own back buffer goes to the screen untouched, which is what the
    // first frames after an arm or a rebuild need -- the image is undefined until
    // something has written it, and copying that over the back buffer is a frame
    // of garbage. Per pair and NOT monotone: a mid-session evaluate failure hands
    // the frame back rather than repeating the last one, which is what the single
    // flag did before there were two.
    bool      out_valid[2];

    // Arms handed over this session, and what the pipelined log line prints from
    // it: which frame is on screen and which frame's evaluate it is showing. A
    // difference of anything but 1 means the wait is in the wrong place or a pair
    // is being skipped. Render thread only.
    UINT64    armed_n;
    UINT64    shown_on, shown_for;
    // Frames whose vkCmdWaitEvents was ALREADY satisfied when it was recorded,
    // which is the only direct evidence the queue did not stop there: an event
    // observed set before the command was even recorded was set long before the
    // GPU could reach it. One EvStatus call, no clock.
    LONG      free_n;

    // The two numbers a performance report needs, and they answer different
    // questions: how long the worker's own body took, and how long the present
    // thread spent inside the transport. The first used to be called park_ms and
    // the line called it "the game's queue was parked" -- close enough under the
    // park, where the queue is stopped for that body plus the wake-up either side
    // of it, and wrong under the pipeline, where the queue stops for whatever is
    // left of a body that started a frame earlier. It measures the worker either
    // way, so it is named for the worker.
    // ponytail: the render thread adds to the frame pair while the worker reads
    // and clears both. A lost sample in a log statistic is not worth a lock; make
    // them interlocked if anything ever decides on these numbers.
    double    work_ms, frame_ms;
    LONG      work_n, frame_n;
    UINT64    frames;      // delivered by the worker under the park; its counter
};

static SynthVkPark g_svp;

// Wall clock in milliseconds. QueryPerformanceCounter because the numbers this
// measures are single-digit milliseconds and GetTickCount's 16 ms cannot see
// them.
static double SynthNowMs()
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

static DWORD WINAPI SynthVkParkWorker(LPVOID)
{
    PollYield poll_yield;
    for (;;)
    {
        WaitForSingleObject(g_svp.wake, INFINITE);
        if (g_svp.stop != 0)
        {
            // The one break in this loop, and it releases the game on its way
            // out. Both pairs, because which one the render thread armed is
            // exactly what is unknowable here -- the same reason the mirror's
            // worker sets both. Both free events with them, or a render thread
            // already inside the top-of-frame wait sits there for the whole
            // budget behind a teardown that has already happened.
            if (g_svk.EvSet != nullptr && g_svk.dev != nullptr)
                for (int i = 0; i < 2; ++i)
                    if (g_svp.ev_out[i] != 0) g_svk.EvSet(g_svk.dev, g_svp.ev_out[i]);
            for (int i = 0; i < 2; ++i)
                if (g_svp.free_ev[i] != nullptr) SetEvent(g_svp.free_ev[i]);
            break;
        }

        // Every arm that is queued before going back to sleep. wake is auto
        // reset, so two arms handed over while this thread was inside its body
        // arrive as one signal; the count is what says how many there really
        // are. The other direction -- a signal with nothing queued, which is what
        // a set on an already-signalled event looks like from here -- costs one
        // test and no work.
        while (g_svp.queued > 0 && g_svp.stop == 0)
        {
            const SynthVkPark::SvpArm a = g_svp.arm[g_svp.take];
            const int slot = a.pair;
            g_svp.take ^= 1;

            // Poll rather than wait: a VkEvent signalled by the GPU has no waitable
            // Win32 handle behind it. PollYield bounds the initial spin and uses a
            // local high-resolution timer for subsequent polls so a slow frame does
            // not burn a core.
            poll_yield.Start();
            const ULONGLONG t0 = GetTickCount64();
            bool seen = false;
            for (;;)
            {
                if (g_svk.EvStatus(g_svk.dev, g_svp.ev_in[slot]) == kSVkEventSet) { seen = true; break; }
                const ULONGLONG waited = GetTickCount64() - t0;
                if (waited > kSvpSeeCopiesMs) break;
                poll_yield.Pause();
            }

            if (seen)
            {
                // The pipeline's two host resets, here and nowhere else, and only
                // after ev_in has been SEEN. Both halves of the proof are in the
                // header above: a set ev_in[slot] means the queue is past the wait on
                // ev_out[slot ^ 1] that the same frame recorded EARLIER in the same
                // buffer, so this is not too early; and the render thread records
                // nothing else naming either event until free_ev[slot], which this
                // loop sets last, so it is not too late. Under the park the render
                // thread does both resets itself, correctly, because there the wait
                // is on the pair it just armed.
                if (a.pipe && g_svk.EvReset != nullptr && g_svk.dev != nullptr)
                {
                    if (g_svp.ev_out[slot ^ 1] != 0) g_svk.EvReset(g_svk.dev, g_svp.ev_out[slot ^ 1]);
                    if (g_svp.ev_in[slot]      != 0) g_svk.EvReset(g_svk.dev, g_svp.ev_in[slot]);
                }

                const double t_body = SynthNowMs();
                // Serialised against the game's own NGX evaluate, as the mirror's
                // worker is. On the present thread the two could not overlap because
                // ReShade presents from the thread the game renders on; off that
                // thread they can, for the one frame a handover takes.
                //
                // Both calls are handed this transport's budget as an argument. It
                // used to be a global one path set and another read, which is how the
                // mirror's 900 could land on a substitute frame -- and, worse, how a
                // mirror frame left 900 standing for the D3D11/D3D12 transport, which
                // parks nothing and wants the full four seconds.
                //
                // The Output this arm owns. Under the park there is one and it is
                // SLOT_OUTPUT; under the pipeline the pair index picks between the
                // two, which is the whole of what doubling one texture costs here.
                const int out_slot = (a.pipe && a.pair != 0) ? kOutB : SLOT_OUTPUT;
                if (g_ngx_cs_ready) EnterCriticalSection(&g_ngx_cs);
                const bool mv_ready = !a.ofa_mv || SynthOfaVkFrame(kSvpWorkMs);
                // Whether an evaluate ran, decided inside the call from the same
                // observation that decided whether to run it. Left false when
                // mv_ready short-circuits, which is what a refused frame needs.
                bool wrote = false;
                const bool ok = mv_ready &&
                                Synth12EvaluateShared(&a.sp, kSvpWorkMs, out_slot, &wrote);
                if (g_ngx_cs_ready) LeaveCriticalSection(&g_ngx_cs);
                g_svp.work_ms += SynthNowMs() - t_body;
                ++g_svp.work_n;

                // Read by the render thread on a later frame, and only there: it
                // decides whether that frame records a copy back at all.
                //
                // `wrote` is part of it because this flag means "an evaluate WROTE
                // this pair's Output", and mode 1 runs the whole transport with
                // the evaluate itself skipped -- Synth12EvaluateShared returns
                // success having written no evaluate result. Under the pipeline
                // the copy back is decided from an earlier frame's flag, so a true
                // here at mode 1 puts a texture nothing has ever written on
                // screen; the pair's second Output is created fresh at every
                // rebuild, so that is undefined GPU memory rather than a stale
                // frame. mode 0 keeps a true and is right to: it evaluates, and
                // only withholds the copy back.
                //
                // It comes back OUT of the call and is not re-read here. Reading
                // g_cfg.mode again at this line was a second, independent
                // observation of a struct CfgReload replaces wholesale about once
                // a second, taken a whole submission after the one that decided
                // whether to write -- so a live 1 -> 2 edit landing between them
                // marked an unwritten Output valid. Same size, and no window left.
                g_svp.out_valid[slot] = ok && wrote;
                // The pipelined transport counts and prints on the render thread,
                // where the copy back it counts is actually recorded. Under the park
                // the worker is the only place the result is known in time.
                if (ok && !a.pipe)
                {
                    const UINT64 n = ++g_svp.frames;
                    CountDelivered();
                    if (n <= 1 || (n % 600) == 0)
                        // Each average with its own divisor's count. One label for
                        // both said the second number was averaged over a window it
                        // was not: the two counters advance on different frames --
                        // work_n only on frames a worker saw the copies for, frame_n
                        // on every frame that reached the foot of SynthVkFrame.
                        Log("[synth] Vulkan frame %llu delivered; the worker took %.2f ms over "
                            "the last %ld frames it ran on, which is most of what the game's "
                            "queue was parked for -- the wake-up either side of it is not "
                            "measured -- and the present thread spent %.2f ms in the transport "
                            "over the last %ld frames.",
                            n, g_svp.work_n != 0 ? g_svp.work_ms / g_svp.work_n : 0.0,
                            g_svp.work_n,
                            g_svp.frame_n != 0 ? g_svp.frame_ms / g_svp.frame_n : 0.0,
                            g_svp.frame_n);
                    if ((n % 600) == 0)
                    { g_svp.work_ms = g_svp.frame_ms = 0.0; g_svp.work_n = g_svp.frame_n = 0; }
                }
            }
            else
            {
                // Not a skipped frame: the frame this worker was woken for is still
                // queued with its own vkCmdWaitEvents unexecuted, and a queue that did
                // not reach a copy in 300 ms is not a condition trying again improves
                // on. The release below is what lets that frame past, 300 ms late; the
                // re-check after the top-of-frame wait in SynthVkFrame is what stops the
                // frame behind it from being recorded at all, and neither of those is
                // this branch's doing.
                //
                // What retires here is this transport and nothing else. The thread,
                // the three Win32 handles and the four VkEvents stay until the device
                // goes -- no frame records again, so nothing touches them -- and the
                // line no longer claims a teardown that does not happen.
                Log("[synth] the game's queue did not reach the substitute's copies within "
                    "%lu ms, so this transport records no further frame and the game keeps "
                    "rendering on its own. If it happens repeatedly on this driver, "
                    "vk_sync=1 in dlss5-bridge.cfg runs the transport without the park or the "
                    "pipeline, at the cost of a full CPU wait every frame.", kSvpSeeCopiesMs);
                g_svp.retired = true;
            }

            // UNCONDITIONAL, and written here rather than inside either branch for
            // exactly that reason. ReShade's command buffer is stopped at a
            // vkCmdWaitEvents on this event with the whole rest of the frame behind
            // it; an exit path that does not set it is a device loss for the game
            // rather than a dropped frame.
            //
            // Guarded like its two siblings, and for a case they already knew about:
            // if SynthVkParkStop's join expires this thread is an orphan, and the
            // device it was given can be gone by the time it gets here. That path
            // leaves g_svk.dev standing precisely so this line still has something to
            // call -- but the entry point and the event are tested anyway, because
            // vkSetEvent(NULL, NULL) is a fault rather than a no-op.
            if (g_svk.EvSet != nullptr && g_svk.dev != nullptr && g_svp.ev_out[slot] != 0)
                g_svk.EvSet(g_svk.dev, g_svp.ev_out[slot]);
            // LAST of the three, and that order is what the header's second half of
            // the reset proof rests on: the render thread arms this pair again only
            // after waiting on this handle, so by then both resets above have landed
            // and ev_out[slot] carries this arm's result.
            if (g_svp.free_ev[slot] != nullptr) SetEvent(g_svp.free_ev[slot]);
            InterlockedDecrement(&g_svp.queued);
        }
    }
    return 0;
}

// The four VkEvents, the three Win32 events and the thread. Each piece tested on
// its own: a teardown can leave one made and the others gone, and remaking one
// that exists leaks a handle or starts a second thread on the same slot.
static bool SynthVkParkEnsure()
{
    // All four, not ev_in[0] alone. vkCreateEvent can succeed for the first pair
    // and refuse for the second, and a test on one handle would then call the set
    // complete and let the second frame record vkCmdSetEvent on VK_NULL_HANDLE.
    // Unreachable today -- the failure returns false before the thread exists and
    // latches g_svp.off -- and one line rather than an argument about that.
    const bool have_events = g_svp.ev_in[0] != 0 && g_svp.ev_in[1] != 0 &&
                             g_svp.ev_out[0] != 0 && g_svp.ev_out[1] != 0;
    if (have_events && g_svp.worker != nullptr) return true;

    if (!have_events)
    {
        SVkEventCreateInfo eci = {};
        eci.sType = kSVkStEventCreateInfo;
        // Only the ones that are missing, for the same reason the test above
        // counts all four: creating over a handle that exists leaks it.
        for (int i = 0; i < 2; ++i)
            if ((g_svp.ev_in[i]  == 0 &&
                 g_svk.EvCreate(g_svk.dev, &eci, nullptr, &g_svp.ev_in[i])  != kSVkSuccess) ||
                (g_svp.ev_out[i] == 0 &&
                 g_svk.EvCreate(g_svk.dev, &eci, nullptr, &g_svp.ev_out[i]) != kSVkSuccess))
            { Log("[synth] vkCreateEvent refused, so the frame cannot be parked."); return false; }
    }

    for (int i = 0; i < 2; ++i)
        if (g_svp.free_ev[i] == nullptr)
            g_svp.free_ev[i] = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (g_svp.wake == nullptr) g_svp.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_svp.free_ev[0] == nullptr || g_svp.free_ev[1] == nullptr || g_svp.wake == nullptr)
    { Log("[synth] the park's own Win32 events could not be created."); return false; }
    // Set, not merely present: a shutdown can join the worker with a free event
    // clear, and the next frame's wait would then time out for a park that no
    // longer exists.
    for (int i = 0; i < 2; ++i) SetEvent(g_svp.free_ev[i]);

    if (g_svp.worker == nullptr)
    {
        g_svp.stop = 0;
        g_svp.worker = CreateThread(nullptr, 0, &SynthVkParkWorker, nullptr, 0, nullptr);
        if (g_svp.worker == nullptr)
        { Log("[synth] the transport's worker thread would not start."); return false; }
    }
    return true;
}

// Detach, and the one case that has to work: the add-on unloaded or the device
// destroyed while a frame is parked. Release the game first, then stop the
// thread -- the same order VkmShutdown establishes and for the same reason.
//
// It must also run BEFORE SynthVkShutdownForDevice's vkDeviceWaitIdle: a queue
// parked at a vkCmdWaitEvents never goes idle, so that wait would never return.
//
// It destroys no VkEvent, and that is two fixes in one omission. This is called
// from DllMain with no lock held, so a render thread already inside the arm block
// holds those handles in locals -- reading them into locals closes the read-zero
// half of that race and nothing closes the destroy-after-read half except not
// destroying them. And a queue can still be parked at a vkCmdWaitEvents naming
// one: the EvSet passes below signal the events, they do not make the queued
// command retire. So the destroy lives in SynthVkParkDestroyEvents, which
// SynthVkShutdownForDevice calls after its vkDeviceWaitIdle and DllMain never
// calls at all -- four VkEvents on a device the process is finished with cost
// nothing, and leaving them valid turns the race into a late set on an event
// nobody waits on.
static void SynthVkParkStop()
{
    // An orphaned worker is still inside everything below and outlives the
    // VkDevice it was given, so a second call must not EvSet on a destroyed
    // device, must not join a thread this one already gave up on, and must not
    // close the Win32 handles the orphan still sets at the foot of its loop.
    // Latched rather than unwound: the thread has no bound this side can enforce,
    // so leaking its handles is the whole of the available answer. This is the
    // second call the detach in DllMain makes after a shutdown that orphaned.
    if (g_svp.orphaned) return;

    if (g_svp.worker != nullptr)
    {
        // FIRST, before anything is released, and this is VkmShutdown's order for
        // VkmShutdown's reason: a render thread that has not started a frame yet
        // reads this and does not arm behind the teardown -- which would reset
        // ev_out after the release below and then park the game's GPU on an event
        // no code left in the process would ever set. It narrows the window and
        // does not close it, because this function is also called from DllMain
        // with no lock held; what closes it is the handle test in the arm block
        // for the read-zero half, and this function destroying no VkEvent for the
        // other half.
        g_svp.stopping = 1;
        if (g_svk.EvSet != nullptr && g_svk.dev != nullptr)
            for (int i = 0; i < 2; ++i)
                if (g_svp.ev_out[i] != 0) g_svk.EvSet(g_svk.dev, g_svp.ev_out[i]);
        g_svp.stop = 1;
        SetEvent(g_svp.wake);
        const bool joined = WaitForSingleObject(g_svp.worker, 2000) == WAIT_OBJECT_0;
        // Again, and this pass is the one that counts: it covers the 2000 ms
        // expiring on a worker still inside its evaluate, which leaves the loop's
        // own release unreached. It runs whether or not the join succeeded --
        // releasing the game is the part that has to happen either way.
        if (g_svk.EvSet != nullptr && g_svk.dev != nullptr)
            for (int i = 0; i < 2; ++i)
                if (g_svp.ev_out[i] != 0) g_svk.EvSet(g_svk.dev, g_svp.ev_out[i]);

        if (!joined)
        {
            // Nothing below runs, and the objects are leaked on purpose. The
            // worker is still inside its body and still names all of it: the four
            // VkEvents, the three Win32 handles, g_s12's textures and feature, and
            // the optical flow's private D3D11 device. Destroying any of them
            // under a running thread is the use after free this order exists to
            // prevent, and 2000 ms is not a number the worker is bound by -- its
            // own worst case is 2900, and under FreeLibrary the fault path's
            // NameFaultOwner wants the loader lock DllMain is holding, so the
            // wait cannot succeed at all.
            //
            // The wake set above is still signalled and stop is left at 1, so the
            // worker exits by itself when it finishes; it is the join that gave
            // up, not the stop.
            Log("[synth] the park's worker did not finish within 2000 ms, so its four "
                "VkEvents, its three Win32 handles and the private D3D12 and D3D11 objects "
                "it is inside are left standing rather than destroyed under a thread that "
                "still names them. The game's queue was released twice. That thread "
                "outlives the VkDevice it was given, so the release at the foot of its "
                "loop may be a call into a destroyed device; nothing here can bound it. "
                "The park does not run again in this process.");
            g_svp.off      = true;
            g_svp.orphaned = true;
            return;
        }
        CloseHandle(g_svp.worker);
        g_svp.worker = nullptr;
    }

    for (int i = 0; i < 2; ++i)
        if (g_svp.free_ev[i] != nullptr) { CloseHandle(g_svp.free_ev[i]); g_svp.free_ev[i] = nullptr; }
    if (g_svp.wake != nullptr) { CloseHandle(g_svp.wake); g_svp.wake = nullptr; }
    g_svp.stop      = 0;
    g_svp.queued    = 0;
    g_svp.next      = 0;
    g_svp.take      = 0;
    g_svp.pending[0] = g_svp.pending[1] = false;
    g_svp.armed_last = false;
    g_svp.out_valid[0] = g_svp.out_valid[1] = false;
    g_svp.said      = 0;
    // stopping and retired are NOT cleared here, and that is the difference
    // between this function's two callers. SynthVkShutdownForDevice clears them
    // itself once its own teardown has finished, because a new device gets a new
    // attempt at the transport. The detach in DllMain must not: SynthVkFrame's
    // entry test is `retired || stopping != 0`, so clearing them at the foot of
    // this function reopens the gate on a present thread that has not yet reached
    // that test, and SynthVkParkEnsure then builds fresh events and a NEW worker
    // whose start address is in the module the loader is unloading. The mirror
    // never had this: VkmShutdown sets stopping and only OnVkmInitRuntime clears
    // it. Which is also why they cannot be cleared in SynthVkParkEnsure, where
    // they would read more naturally: SynthVkFrame tests them above the call that
    // builds the park, so a clear there never runs and the transport stays retired
    // for the session -- measured in ngxGym on 2026-09-04, synth-vk-nomirror,
    // where the substitute delivered nothing after the first mode change.
}

// The four VkEvents, and the only caller is SynthVkShutdownForDevice, AFTER its
// vkDeviceWaitIdle. Split out of SynthVkParkStop for the two reasons that
// function's header gives: a queue can still be parked at a vkCmdWaitEvents
// naming one of these when the park is stopped, and the detach caller races a
// render thread that is holding them in locals. Here the queue is provably idle
// and the caller holds g_bridge_cs, which is the section SynthVkFrame runs
// inside. They are four of the objects the Khronos validation layer counts at
// vkDestroyDevice.
static void SynthVkParkDestroyEvents()
{
    if (g_svk.EvDestroy == nullptr || g_svk.dev == nullptr) return;
    for (int i = 0; i < 2; ++i)
    {
        if (g_svp.ev_in[i]  != 0) { g_svk.EvDestroy(g_svk.dev, g_svp.ev_in[i],  nullptr); g_svp.ev_in[i]  = 0; }
        if (g_svp.ev_out[i] != 0) { g_svk.EvDestroy(g_svk.dev, g_svp.ev_out[i], nullptr); g_svp.ev_out[i] = 0; }
    }
}

// WHO MAY TOUCH g_s12, and there are THREE owners rather than the two this
// sentence used to name. The private D3D12 session belongs to whoever holds
// g_bridge_cs, EXCEPT for the length of one substitute arm, when it belongs to
// the park's worker -- whose free event is the proof, set last on every path out
// of its body -- and for the length of one mirrored evaluate, when it belongs to
// the mirror's worker, whose proof is a different handle again, g_vkm.idle.
//
// The two workers exclude each other with g_ngx_cs. g_bridge_cs held by the
// mirror excludes the park's worker through the drain below and the mirror's own
// through its idle wait. g_bridge_cs held by the SUBSTITUTE's render thread
// excludes the park's worker the same way and excludes the mirror's worker by
// nothing at all: it waits no g_vkm handle anywhere -- nothing in this file so
// much as names g_vkm -- and what stands between them is the five-second quiet
// window in SynthReleaseLatch, which is a window and not a handshake.
//
// This is that proof, taken by hand. The render thread already waits both free
// events at the top of every frame that rebuilds; the Vulkan mirror had no such
// wait at all, and its rebuild releases exactly what the worker is inside: the
// NGX feature, the shared textures including tex[kOutB], the imported VkImages
// and the optical flow's private D3D11 device. It ran under g_bridge_cs alone
// and the worker takes g_ngx_cs alone, so the two sections do not overlap. Under
// vk_sync=0 that was masked by the game's queue being parked, which holds the
// mirror's own vkDeviceWaitIdle until the worker has finished -- an accident
// nothing stated and which vk_sync=2 removes.
//
// Draining rather than widening the lock, because widening puts the 4.4-5.9 ms
// evaluate back inside the section the present thread takes every frame, which
// is the park undone. It cannot deadlock: the free event is set by the worker,
// whose only prerequisite is the GPU reaching ev_in, and the present thread
// reaches its submit without g_bridge_cs.
static bool SynthVkDrainWorker()
{
    // A park that is being torn down is not one to wait on, and refusing is the
    // right answer either way: SynthVkParkStop is called from DllMain with no
    // lock held, so it can close both free events and null them while this
    // function is between its test and its wait. A closed Win32 handle is worse
    // than a zeroed VkEvent -- the OS may reissue the value to any other
    // allocation in the process, so the wait can succeed on something unrelated
    // or block on it for the whole budget. The caller must not rebuild across a
    // teardown in any case.
    if (g_svp.stopping != 0 || g_svp.orphaned) return false;
    if (g_svp.worker == nullptr) return true;
    // Into locals before the test and the wait, the same rule the arm blocks
    // follow and for the same teardown. One WaitForMultipleObjects rather than
    // two waits, so the budget below is the budget for the whole drain: two
    // sequential waits of kSvpIdleMs each is twice the number the constant's own
    // derivation reasons about, on the game's render thread, inside its DLSS
    // detour, under g_bridge_cs.
    HANDLE fe[2] = { g_svp.free_ev[0], g_svp.free_ev[1] };
    if (fe[0] == nullptr || fe[1] == nullptr) return true;
    return WaitForMultipleObjects(2, fe, TRUE, kSvpIdleMs) == WAIT_OBJECT_0;
}

// One frame.
//
// The states the game's resources are in at reshade_begin_effects are the same
// facts the D3D12 transport reads, and they come off ReShade's own code rather
// than off an assumption: the back buffer is in render_target -- the old state
// runtime.cpp itself names for it in the implicit back buffer copy at the top of
// the first effect pass -- and depth and motion vectors are in shader_resource,
// which is where generic_depth's own reshade_begin_effects handler leaves the
// depth, and it is a built-in add-on whose callback runs before any external one.
//
// Unlike the D3D12 transport the back buffer and the motion vectors do have to
// be transitioned here, because Vulkan reads an image in a transfer only from
// TRANSFER_SRC_OPTIMAL or GENERAL and SHADER_READ_ONLY_OPTIMAL is neither.
// ReShade's own barrier is what does it, and that is the whole reason to use it:
// it is the only thing that knows each image's aspect mask, and getting that
// wrong copies the wrong plane.
//
// The DEPTH binding is the exception and no longer moves at all: it is read by a
// compute pass that samples it, and shader_resource is already the layout a
// sampled read wants. See SynthDepthCsRun.
static void SynthVkFrame(reshade::api::effect_runtime *rt, reshade::api::command_list *cmd,
                         const SynthParams *sp, reshade::api::resource back,
                         reshade::api::resource_view back_view, uint32_t back_usage,
                         reshade::api::resource depth, reshade::api::resource mv,
                         UINT w, UINT h, DXGI_FORMAT back_fmt)
{
    using reshade::api::resource_usage;

    // Drawn rather than copied when the back buffer cannot be copied into, or
    // when the file says so. Decided per frame from the usage ReShade reports,
    // because a swapchain is recreated on every mode change and the next one
    // need not be created like the last.
    const bool draw = g_cfg.vk_present == 2 ||
                      (g_cfg.vk_present == 0 &&
                       (back_usage & static_cast<uint32_t>(resource_usage::copy_dest)) == 0);

    if (!Synth12Open(rt)) return;
    if (!SynthVkOpen(rt)) return;
    // This transport's own two exits, read beside the shared one above rather
    // than through it: retired says a stall or a missed park ended the substitute
    // here, stopping says a teardown is in flight. Neither is a statement about
    // whether the Vulkan side opened, which is what SynthVkOpen answers and what
    // the mirror reads through it.
    if (g_svp.retired || g_svp.stopping != 0) return;

    // Fetched above the rebuild rather than below it, because the rebuild is the
    // one thing here that needs the queue before it needs a command buffer.
    reshade::api::command_queue *rq = rt->get_command_queue();
    if (rq == nullptr) return;

    const double frame_t0 = SynthNowMs();

    // Three transports off one key, decided per frame because vk_sync is re-read
    // about once a second like every other key. 0 parks, 1 serialises, 2
    // pipelines; both 0 and 2 need the worker and its events, and a session that
    // could not build them falls back to 1 rather than losing the frame.
    int want = g_svp.off ? 1 : g_cfg.vk_sync;
    // A machine that could not allocate the second Output runs the park, which
    // delivers every frame the pipeline does and one present sooner. Said by the
    // build; this is only the latch it set.
    if (want == 2 && g_svp.no_pipe) want = 0;
    if (want != 1 && !SynthVkParkEnsure())
    {
        // Said by whichever branch failed. The transport still works serialised,
        // so this is a slower frame rather than a lost one.
        Log("[synth] the park could not be built, so this session runs the fully "
            "serialised transport instead.");
        g_svp.off = true;
        want = 1;
    }
    const bool park = want == 0;
    const bool pipe = want == 2;
    // Said again when the answer changes, not once for the session: vk_sync is
    // re-read about once a second and this branch is per frame, so a user who
    // flips it mid-session used to change transport with no line at all and leave
    // the standing one naming the wrong transport.
    const int say = park ? 1 : pipe ? 3 : 2;
    if (g_svp.said != say)
    {
        g_svp.said = say;
        Log("[synth] the Vulkan transport %s. %s",
            park ? "parks the game's queue on a Vulkan event while the optical flow and "
                   "the D3D12 evaluate run on a worker thread"
                 : pipe ? "pipelines: the optical flow and the D3D12 evaluate for one frame "
                          "run on a worker thread while the game renders the next, so the "
                          "wait the game's queue reaches is on work that started a frame "
                          "earlier rather than on work that starts when it gets there"
                 : "waits the game's queue idle on the CPU and runs the optical flow and "
                   "the D3D12 evaluate on the present thread",
            park ? "Set vk_sync=1 in dlss5-bridge.cfg to go back to the serialised "
                   "transport if the picture or the driver misbehaves."
                 : pipe ? "The picture is the previous present's, so this costs about 17 ms "
                          "of latency at 60 fps and 33 at 30, and a depth-driven ReShade "
                          "effect reads the current frame's depth over it. vk_sync=0 is the "
                          "default and has neither cost."
                 : "vk_sync=0 parks instead, which is the default and does not stop the "
                   "render thread.");
    }

    // Decided here rather than beside the block that does it, because the wait
    // below has to know: a rebuild takes both pairs, and it also takes the queue
    // idle, which cannot happen with a frame still parked. g_s12.out_b != pipe is
    // what routes a vk_sync edit between two frames down this path instead of
    // half-changing the transport under a frame in flight -- the rebuild already
    // idles the queue, clears both out_valid and makes or drops the second Output.
    const bool rebuild = g_s12.feature == nullptr || g_s12.w != w || g_s12.h != h ||
                         g_s12.fmt != back_fmt || g_s12.out_b != pipe;

    // Before anything reads or writes the private D3D12 session, the optical flow
    // engine or the shared images: the worker owns all three for the length of an
    // arm. Waited on whenever a worker exists, not only when this frame parks --
    // vk_sync can be edited between two frames, and the rebuild below idles the
    // game's queue, which never happens while a frame is parked.
    //
    // ONE pair under the pipeline, the one this frame is about to arm, because
    // that is the whole of what it needs and waiting the other would put the
    // park's serialisation back on the CPU. BOTH in every other case, and both is
    // sufficient rather than merely cautious: each worker sets its own ev_out
    // before its free event; the worker's own reset of an ev_out is for a pair
    // whose wait on it has already executed; and the render thread's resets,
    // which only the park makes, happen after it has waited BOTH free events
    // here. So no queued wait can be sitting on an event something cleared behind
    // it.
    if (g_svp.worker != nullptr)
    {
        const int first = (pipe && !rebuild) ? g_svp.next : 0;
        const int last  = (pipe && !rebuild) ? g_svp.next : 1;
        for (int i = first; i <= last; ++i)
            if (g_svp.free_ev[i] != nullptr &&
                WaitForSingleObject(g_svp.free_ev[i], kSvpIdleMs) != WAIT_OBJECT_0)
            {
                // The one condition that reaches here is an arm whose worker is
                // not done, which under the park means ReShade's command buffer is
                // stopped at its vkCmdWaitEvents with the whole rest of that frame
                // behind it. Retiring without setting the event leaves the game's
                // queue exactly there, so it is released by hand first -- the same
                // statement SynthVkParkStop makes and for the same stakes. The
                // line used to claim the game kept rendering on its own while
                // doing nothing to make that true.
                //
                // Both events, because either pair can be the one a queued wait
                // names and nothing here can tell which: this transport records no
                // further frame after this line, so an ev_out set for a pair
                // nobody waits on is set for nobody.
                //
                // What the queue is released INTO is an already-recorded copy back,
                // while the stuck worker may still be writing that Output -- the
                // same memory, no fence between the two APIs. That frame can be
                // torn. It is the deliberate trade: a torn frame against a queue
                // parked past the 2 s TDR window.
                if (g_svk.EvSet != nullptr && g_svk.dev != nullptr)
                    for (int k = 0; k < 2; ++k)
                        if (g_svp.ev_out[k] != 0) g_svk.EvSet(g_svk.dev, g_svp.ev_out[k]);
                Log("[synth] the worker had not finished pair %d %lu ms later, so both of "
                    "its events were set by hand to let the game's queue past -- that frame "
                    "may be torn -- and this transport records no further frame. The game "
                    "keeps rendering on its own from the next one.", i, kSvpIdleMs);
                g_svp.retired = true;
                return;
            }
    }

    // The mirror's own re-check (vkmirror.inc:1434-1436), for the two things that
    // can happen while this thread is blocked for kSvpIdleMs: the worker can
    // retire the transport on a stall, and a detach can stop the park. Reading
    // either at the top of this function is reading it before the only wait long
    // enough to matter.
    //
    // What it protects is not the event reset below -- the unconditional release
    // at the foot of the worker's loop already bounds that -- but the depth view
    // SynthDepthCsRun retires a few lines down. That destroy is safe because the
    // worker saw the GPU reach the previous frame's copies; on the stall branch
    // it saw nothing, so the proof is void by construction and vkDestroyImageView
    // would be called on a view a submitted command buffer still names.
    if (!g_svk.ready || g_svp.retired || g_svp.stopping != 0) return;

    if (rebuild)
    {
        Log("[synth] building the D3D12 feature and its Vulkan import for %ux%u %s%s",
            w, h, FormatName(back_fmt), pipe ? ", with the pipeline's second Output" : "");
        // Idled first, and this is not belt and braces. Nothing waits for the
        // command buffer this transport records into -- serialised, everything
        // after the wait in the middle of the frame is submitted with nothing
        // behind it; parked, the whole frame is -- so at the top of the next
        // frame last frame's command buffer may still be executing and still
        // naming these images. Destroying a VkImage a submitted command buffer
        // names is undefined, and a resolution change is exactly when it would
        // happen. Safe here because the wait on idle above returned, so no frame
        // is parked and the queue can reach the end of its work.
        rq->wait_idle();
        // The new Output images hold nothing until something has evaluated into
        // them, and both the park and the pipeline record their copy back before
        // they know. Without this every resolution change would put one frame of
        // undefined memory on screen. The pair state goes with them: after this
        // there is nothing queued on either pair, so nothing to wait on and
        // nothing to copy back until something arms again.
        g_svp.out_valid[0] = g_svp.out_valid[1] = false;
        g_svp.pending[0]   = g_svp.pending[1]   = false;
        g_svp.next = 0;
        g_svp.take = 0;
        // And all four VkEvents, which is the one place in this file a host reset
        // of every one of them is unambiguously safe: the queue was idled two
        // lines above, so no submitted command buffer names any of them. Every
        // entry into and every exit from vk_sync=2 comes through this predicate
        // -- g_s12.out_b != pipe -- and without this a session that had been
        // parking left both ev_in SIGNALLED by the GPU, because there the render
        // thread resets them and the worker does not. The pipeline's worker
        // resets ev_in only AFTER it has seen it set, so its first frame on each
        // pair would read the park era's signal and evaluate over copies the
        // game's queue had not made yet: two torn frames, both shown.
        if (g_svk.EvReset != nullptr && g_svk.dev != nullptr)
            for (int i = 0; i < 2; ++i)
            {
                if (g_svp.ev_in[i]  != 0) g_svk.EvReset(g_svk.dev, g_svp.ev_in[i]);
                if (g_svp.ev_out[i] != 0) g_svk.EvReset(g_svk.dev, g_svp.ev_out[i]);
            }
        // The images before the textures they are backed by: releasing a D3D12
        // resource that an imported VkDeviceMemory still names is a use after
        // free neither API has any way to report.
        SynthVkReleaseImages();
        // The optical flow engine's own two textures, which are sized to the
        // shape Synth12Build is about to change, and the private device they
        // were created on. It opens again on the next frame, at the new size.
        SynthOfaVkRebuild();
        // The depth conversion's own texture and its two views. Safe here and
        // nowhere else in this function: the queue was just idled, so nothing
        // submitted still names them. The pipeline, the layout and the sampler
        // survive -- none of them depends on the size.
        SynthDepthCsRelease();
        SynthVkPresentRelease();
        if (!Synth12Build(sp, w, h, w, h, back_fmt, false, 0, 0, pipe))
        {
            // The pipeline's own texture is the one failure here that is not the
            // contract's, and counting it as one retires the substitute after
            // three frames -- for a mode the user opted into and can be taken
            // away from them instead. The latch drops the next frame to the park,
            // which rebuilds without it.
            if (g_s12.out_b_fail)
            { g_s12.out_b_fail = false; g_svp.no_pipe = true; return; }
            Synth12NoteBuildFailure(true);
            return;
        }
        if (!SynthVkImport()) { SynthVkReleaseImages(); g_svk.ready = false; return; }
    }

    // Built on the first frame that gets this far and never retried. A failure
    // stops the transport rather than delivering a frame with no depth in it,
    // and every branch inside has already named its own reason.
    if (!SynthDepthCsOpen(rt->get_device()))
    {
        Log("[synth] the depth conversion pass could not be built, so this transport stops. "
            "The game keeps rendering on its own.");
        g_svk.ready = false;
        return;
    }

    // The motion vector source, decided once. The driver's own optical flow
    // engine if it will open -- it creates its own two textures and needs only
    // the private D3D12 device to open them on -- and one of the three ReShade
    // providers otherwise. Once it has
    // opened, or once it has stopped, no ReShade motion-vector texture is read
    // again on this transport: changing estimator under a feature whose temporal
    // history was built from the other one is the defect this source was added
    // to remove, reintroduced.
    SynthOfaVkOpen(w, h, back_fmt);
    const bool ofa_mv = SynthOfaVkIsSource();

    // The arming gate now lets a session through on the PROMISE of the engine --
    // see the Vulkan branch in OnSynthBeginEffects -- so this frame can arrive
    // with the engine not open and no ReShade texture behind mv either. There is
    // then nothing to copy and nothing to evaluate over: mv.handle is zero, and
    // recording vkCmdCopyImage from VK_NULL_HANDLE into ReShade's own command
    // buffer is undefined behaviour in the game's queue, not a dropped frame.
    //
    // Stand down instead. The engine gets its next chance on the next frame if it
    // has not given up; if it has, g_ovk.stopped closes the gate above and this
    // session is over.
    // Two ways to arrive here without a usable source, and this used to catch only
    // the first. The engine never opened and there is no ReShade texture either --
    // or the engine opened, ran, and then STOPPED, which leaves ofa_mv true because
    // a stopped session must not swap estimator under a feature whose history came
    // from the other one.
    //
    // In that second case the arming gate disagrees with this function: it refuses
    // the engine on g_ovk.stopped, walks the ReShade names, and hands over a live
    // motion-vector texture -- which the copy below then skips, because ofa_mv is
    // true. Every frame did the full round after that, barriers and copies and the
    // queue idle included, and delivered nothing, silently, while the panel named a
    // ReShade shader as the source. Standing down is the same decision; doing it out
    // loud and without the work is the fix.
    if (!SynthOfaVkReady() && (ofa_mv || mv.handle == 0))
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            if (SynthOfaVkStopped())
                Log("[synth] the driver's optical flow engine has stopped, so this "
                    "transport stops delivering rather than change estimator underneath "
                    "a feature whose temporal history was built from its vectors. The "
                    "game keeps rendering on its own.");
            else
                Log("[synth] the driver's optical flow engine has not opened and there "
                    "is no ReShade motion-vector texture either, so nothing is delivered "
                    "until one of the two arrives. The log above says why the engine did "
                    "not open.");
        }
        return;
    }

    cmd->barrier(back, resource_usage::render_target, resource_usage::copy_source);
    if (!ofa_mv)
        cmd->barrier(mv, resource_usage::shader_resource, resource_usage::copy_source);

    // Depth, sampled rather than copied, into a texture of this add-on's own.
    // Recorded before the raw Vulkan copies below so that everything ReShade
    // records and everything this file records stay in two contiguous runs
    // rather than interleaved -- they land in the same VkCommandBuffer either
    // way, and one order is easier to read in a capture than two.
    const reshade::api::resource dconv = SynthDepthCsRun(cmd, depth, w, h);
    if (dconv.handle == 0)
    {
        // Put the two textures back where ReShade left them before leaving, or
        // the next thing to touch them transitions from a state they are not in.
        cmd->barrier(back, resource_usage::copy_source, resource_usage::render_target);
        if (!ofa_mv)
            cmd->barrier(mv, resource_usage::copy_source, resource_usage::shader_resource);
        return;
    }

    SVkCommandBuffer cb =
        reinterpret_cast<SVkCommandBuffer>(static_cast<uintptr_t>(cmd->get_native()));

    // Which pair this frame arms, which pair it reads back, and whether either is
    // usable. -1 says nothing is copied back: the first frame of a session, the
    // first after a rebuild, or a frame that could not arm at all.
    int pipe_p = -1, out_pair = -1;
    // The handle the arm at the foot of this function records its set on, carried
    // out of the test below rather than read again there: the same local-read rule
    // the park's arm follows, and the reason is the same teardown.
    SVkHandle pipe_evi = 0;
    if (pipe)
    {
        // Into locals before anything is recorded, exactly as the park's arm does
        // and for its reason: SynthVkParkStop zeroes these handles and is called
        // from DllMain with no lock held, so a render thread already past the
        // stopping test can otherwise record on VK_NULL_HANDLE. Both pairs, not
        // just the one being armed, because this frame names both.
        const int p = g_svp.next, prev = p ^ 1;
        const SVkHandle evi_p    = g_svp.ev_in[p],    evo_p    = g_svp.ev_out[p];
        const SVkHandle evi_prev = g_svp.ev_in[prev], evo_prev = g_svp.ev_out[prev];
        if (evi_p != 0 && evo_p != 0 && evi_prev != 0 && evo_prev != 0)
        {
            pipe_p    = p;
            pipe_evi  = evi_p;
            // THE WAIT, and its position is the whole of this mode. It is on the
            // pair the PREVIOUS frame armed, so the evaluate it waits for started a
            // frame ago; and it is recorded FIRST, above the copies and above the
            // vkCmdSetEvent that announces them, which is what lets the worker
            // treat a set ev_in[p] as proof that this wait has executed. See the
            // header: with the set before the wait, as the park has it, the
            // worker's reset of ev_out[prev] races a queue that may already be at
            // the next frame's wait.
            //
            // Recorded whether or not anything is copied back and whatever
            // g_cfg.mode says, because it is also what orders the copies below
            // against the worker that is still reading that pair's inputs.
            if (g_svp.pending[prev])
            {
                // The only direct evidence that the queue did not stop here, and
                // one call rather than a clock: an event observed set before the
                // command was even RECORDED was set long before the GPU could
                // reach it.
                if (g_svk.EvStatus(g_svk.dev, evo_prev) == kSVkEventSet)
                    ++g_svp.free_n;

                SVkMemoryBarrier mb = {};
                mb.sType         = kSVkStMemoryBarrier;
                mb.srcAccessMask = kSVkAccessMemoryRead | kSVkAccessMemoryWrite;
                mb.dstAccessMask = kSVkAccessMemoryRead | kSVkAccessMemoryWrite;
                const SVkHandle ev = evo_prev;
                g_svk.CmdWaitEvents(cb, 1, &ev, kSVkStageHost, kSVkStageAllCommands,
                                    1, &mb, 0, nullptr, 0, nullptr);
                g_svp.pending[prev] = false;
                out_pair = prev;
            }
        }
    }

    for (int i = 0; i < SLOT_COUNT; ++i)
    {
        // Out of UNDEFINED once, and a GENERAL to GENERAL barrier on every frame
        // after that -- which is not a no-op. It is the memory dependency that
        // makes what the other API wrote visible here and the other way round,
        // and with no shared fence between the two APIs it is the only one there
        // is on this side.
        SynthVkBarrier(cb, g_svk.img[i],
                       g_svk.laid_out ? kSVkLayoutGeneral : kSVkLayoutUndefined,
                       kSVkLayoutGeneral);
    }
    g_svk.laid_out = true;
    // The pipeline's second Output, out of UNDEFINED once and given the same
    // memory dependency on every frame after. Its own flag rather than laid_out
    // becoming an array, because vkmirror.inc reads laid_out and has no second
    // Output to lay out.
    if (g_svk.img[kOutB] != 0)
    {
        SynthVkBarrier(cb, g_svk.img[kOutB],
                       g_svk.laid_out_b ? kSVkLayoutGeneral : kSVkLayoutUndefined,
                       kSVkLayoutGeneral);
        g_svk.laid_out_b = true;
    }

    SynthVkCopyImage(cb, static_cast<SVkHandle>(back.handle), kSVkLayoutTransferSrc,
                     g_svk.img[SLOT_COLOR], kSVkLayoutGeneral, w, h);
    // Skipped entirely when the driver's engine is the source: the evaluate then
    // reads the engine's own texture instead of this one, so copying a ReShade
    // effect's vectors in would be work nothing reads -- and on a frame the
    // engine refuses, would be the estimator swap this transport refuses to
    // make.
    if (!ofa_mv)
        SynthVkCopyImage(cb, static_cast<SVkHandle>(mv.handle), kSVkLayoutTransferSrc,
                         g_svk.img[SLOT_MV], kSVkLayoutGeneral, w, h);

    // The converted depth, which is now an ordinary R32_FLOAT colour image on
    // both sides, so it travels as an ordinary image copy. The buffer round trip
    // this replaces existed only because a DEPTH aspect cannot be copied into a
    // colour image; nothing samples an aspect any more by the time we get here.
    SynthVkCopyImage(cb, static_cast<SVkHandle>(dconv.handle), kSVkLayoutTransferSrc,
                     g_svk.img[SLOT_DEPTH], kSVkLayoutGeneral, w, h);

    bool ok;
    if (!park && !pipe)
    {
        // The whole of the CPU synchronisation on this side, and the reason there
        // is no fence in this transport. wait_idle flushes ReShade's immediate
        // command list and then waits the queue out, so everything above has
        // finished on the GPU before the D3D12 evaluate below reads any of it.
        rq->wait_idle();

        // The optical flow engine copies the colour that has just crossed into its
        // own texture on the D3D12 side, reads it through the private D3D11 device,
        // and writes the vectors the evaluate below hands to NGX. It has to sit here
        // and nowhere else: above the wait the colour is not there yet, and below the
        // evaluate it would be a frame late.
        //
        // A frame the engine does not deliver -- the first one, which has no
        // previous frame to compare against, or one whose execute was refused -- is
        // not delivered at all. That is the same rule the D3D11 path follows and for
        // the same reason: the alternative is handing DLSS somebody else's vectors
        // for one frame.
        // Both on the default budget, and taking it as an argument rather than
        // off a global is the point: nothing is parked behind this path, so four
        // seconds is the generous ceiling it wants, and it cannot inherit the 600
        // the park set or the 900 the mirror set on some earlier frame.
        //
        // And under g_ngx_cs, as the park's worker and the mirror's worker both
        // are. This branch was the one NGX caller in the process without it: the
        // present thread cannot collide with the game's own evaluate, but the
        // MIRROR's worker can, and it drives the same g_s12.list. A fallback
        // offered as the safe answer to "the park misbehaves" must not be the
        // weaker of the two. No inversion: every NGX forward releases g_ngx_cs
        // before it takes g_bridge_cs, and nothing holding g_ngx_cs waits for it.
        if (g_ngx_cs_ready) EnterCriticalSection(&g_ngx_cs);
        const bool mv_ready = !ofa_mv || SynthOfaVkFrame();
        ok = mv_ready && Synth12EvaluateShared(sp);
        if (g_ngx_cs_ready) LeaveCriticalSection(&g_ngx_cs);

        // Read again, and this is not defensive. The flush inside wait_idle advances
        // ReShade's immediate command list to its next command buffer and begins
        // recording into that one; the handle taken before it has been closed and
        // submitted, and recording into it now would be recording into nothing.
        cb = reinterpret_cast<SVkCommandBuffer>(static_cast<uintptr_t>(cmd->get_native()));
    }
    else if (pipe)
    {
        // The arm, and it is the LAST thing this transport records: the wait it
        // pairs with went in above the copies, and a worker that sees this event
        // has therefore seen a command recorded after that wait. That is the
        // whole of the ordering argument, and the header states both halves.
        if (pipe_p >= 0)
        {
            // Written while nothing reads this pair -- the wait on its free event
            // at the top of this function is what says so -- and copied rather
            // than pointed at, for the reason the park already gives: the caller's
            // SynthParams is a stack object in OnSynthBeginEffects and is gone
            // before the worker runs.
            g_svp.arm[pipe_p].sp      = *sp;
            g_svp.arm[pipe_p].ofa_mv  = ofa_mv;
            g_svp.arm[pipe_p].pipe    = true;
            g_svp.arm[pipe_p].pair    = pipe_p;
            g_svp.arm[pipe_p].ordinal = ++g_svp.armed_n;

            ResetEvent(g_svp.free_ev[pipe_p]);
            g_svk.CmdSetEvent(cb, pipe_evi, kSVkStageAllCommands);
            // The count before the wake, because the wake is auto reset and the
            // count is what the worker believes: a signal that arrives with
            // nothing counted is a turn of an empty loop, a count that arrives
            // with no signal is an arm that waits for the next one.
            InterlockedIncrement(&g_svp.queued);
            SetEvent(g_svp.wake);
            g_svp.pending[pipe_p] = true;
            // Read at the top of the NEXT frame's SynthMaybeDeliver, which is
            // what makes the line above mean "one frame ago" instead of "at some
            // point". Every other way out of this function leaves it false.
            g_svp.armed_last      = true;
            g_svp.next = pipe_p ^ 1;
        }

        // Decided on the pair the wait above named, and the value read here is
        // usually a pair-generation old: at the moment this frame is RECORDED the
        // worker for the previous frame's arm has typically not started, because
        // the GPU has not reached that frame's ev_in yet. What it therefore
        // answers is "has an evaluate ever written this pair's Output", which is
        // the question that matters -- undefined memory must never reach the
        // screen -- while the vkCmdWaitEvents above is what answers "are this
        // frame's pixels there". A stale true after a failed evaluate repeats
        // this pair's last SUCCESSFUL frame, which is three presents back and not
        // one, because the pairs alternate and nothing orders this read against
        // the worker's write: the wait at the top of the frame covers only the
        // pair being armed, never prev.
        //
        // A false hands the back buffer home as the game rendered it. The park
        // pays that too, one present later rather than on the same frame -- its
        // ok is read for the PREVIOUS pair while the image it copies is the one
        // this frame's worker fills, so one refusal there costs a repeated frame
        // and then an un-upscaled one. So the difference is not what reaches the
        // screen but how much of it: the pipeline shows the un-upscaled frame
        // without the duplicate in front of it, because its ok is read for the
        // same pair whose picture it copies. The order is what forces it, the
        // same order that costs a present of latency: by the time a refusal is
        // known here, the pair it belongs to is the one being read back.
        ok = out_pair >= 0 && g_svp.out_valid[out_pair];
        if (ok)
        {
            // Both ordinals, for the one line that can show the lag is a frame and
            // not two: the frame being recorded now against the frame whose
            // evaluate its copy back is carrying.
            g_svp.shown_on  = g_svp.armed_n;
            g_svp.shown_for = g_svp.arm[out_pair].ordinal;
        }
    }
    else
    {
        // Read BEFORE the worker is woken, because the worker writes it: this
        // frame's copy back is recorded on the evidence of the last frame's
        // evaluate. There is no other order available -- the copy back has to be
        // in the command buffer before the evaluate that fills the image runs.
        // The pair the LAST frame took, which is the one the next line is about
        // to flip away from.
        //
        // So a frame whose own evaluate is then refused still copies back, and
        // what it copies is the previous frame's picture: SLOT_OUTPUT still holds
        // it, because a refusal writes nothing. That is deliberate and it is the
        // only choice this order leaves -- by the time the refusal is known the
        // copy back is already in the game's queue.
        //
        // It costs BOTH, one present apart, which this used to price as one. The
        // refused frame writes out_valid[pair] = false; the frame after it reads
        // exactly that entry, since next has flipped, so ok is false and the
        // game's own un-upscaled render goes to the screen behind the duplicate.
        // The alternative is the un-upscaled frame on its own and a present
        // sooner, which is what the pipeline does.
        ok = g_svp.out_valid[g_svp.next ^ 1];

        // The pair, armed. Alternating, and the reset is safe because the
        // previous frame on this pair has been released -- by its worker, which
        // sets ev_out on every branch of its loop.
        const int pair = g_svp.next;
        // Into locals before anything is recorded, and this is what actually
        // survives a teardown racing this block. SynthVkParkStop zeroes these
        // four handles, and it is called from DllMain with no lock held, so a
        // render thread already past the stopping test can otherwise read 0 here
        // and record vkCmdWaitEvents on VK_NULL_HANDLE -- which parks the game on
        // an event that does not exist. No lock instead: that unlocked caller
        // holds the loader lock, and this function's own section covers code that
        // calls GetProcAddress.
        const SVkHandle evi = g_svp.ev_in[pair], evo = g_svp.ev_out[pair];
        if (evi == 0 || evo == 0)
        {
            // Nothing is recorded and nothing is copied back. The frame goes home
            // as the game rendered it, which is the whole of the cost.
            ok = false;
        }
        else
        {
            g_svp.next ^= 1;
            // Both resets on this thread, which is sound HERE and only here: the
            // wait recorded below is on the pair this frame just armed, so no
            // queued command names either event. The pipeline cannot do this,
            // which is why its worker does it instead.
            g_svk.EvReset(g_svk.dev, evi);
            g_svk.EvReset(g_svk.dev, evo);

            // Everything the worker reads, written while it is idle. The contract is
            // copied rather than pointed at: the caller's SynthParams is a stack
            // object in OnSynthBeginEffects and is gone before the worker runs.
            g_svp.arm[pair].sp      = *sp;
            g_svp.arm[pair].ofa_mv  = ofa_mv;
            g_svp.arm[pair].pipe    = false;
            g_svp.arm[pair].pair    = pair;
            g_svp.arm[pair].ordinal = ++g_svp.armed_n;
            ResetEvent(g_svp.free_ev[pair]);

            // The park. Everything above has to have executed before the worker
            // submits the evaluate that reads it, and the copy back below has to run
            // after that evaluate has finished -- and ReShade records the rest of the
            // frame after this function returns, into this same command buffer, so
            // the copy back cannot wait for a later one.
            g_svk.CmdSetEvent(cb, evi, kSVkStageAllCommands);
            InterlockedIncrement(&g_svp.queued);
            SetEvent(g_svp.wake);

            SVkMemoryBarrier mb = {};
            mb.sType         = kSVkStMemoryBarrier;
            mb.srcAccessMask = kSVkAccessMemoryRead | kSVkAccessMemoryWrite;
            mb.dstAccessMask = kSVkAccessMemoryRead | kSVkAccessMemoryWrite;
            const SVkHandle ev = evo;
            g_svk.CmdWaitEvents(cb, 1, &ev, kSVkStageHost, kSVkStageAllCommands,
                                1, &mb, 0, nullptr, 0, nullptr);
        }
    }

    // Which of the two Output images this frame reads back. Only the pipeline
    // ever names the second one, and only for the pair whose wait it recorded.
    const int out_img = out_pair == 1 ? kOutB : SLOT_OUTPUT;

    if (ok && g_cfg.mode == 2 && draw)
    {
        if (SynthVkPresentOpen(rt->get_device(), back_fmt) && SynthVkPresentSize(w, h, back_fmt))
            SynthVkPresentDraw(cmd, cb, back, back_view, w, h, g_svk.img[out_img]);
        else
        {
            // Nothing can put the result into this back buffer, so the session
            // stops rather than deliver frames nobody sees. The reason is the
            // line the present pass logged.
            cmd->barrier(back, resource_usage::copy_source, resource_usage::render_target);
            if (!ofa_mv)
                cmd->barrier(mv, resource_usage::copy_source, resource_usage::shader_resource);
            Log("[synth] the present pass could not be built, so this transport stops. "
                "The game keeps rendering on its own.");
            g_svk.ready = false;
            return;
        }
    }
    else if (ok && g_cfg.mode == 2)
    {
        cmd->barrier(back, resource_usage::copy_source, resource_usage::copy_dest);
        SynthVkBarrier(cb, g_svk.img[out_img], kSVkLayoutGeneral, kSVkLayoutGeneral);
        SynthVkCopyImage(cb, g_svk.img[out_img], kSVkLayoutGeneral,
                         static_cast<SVkHandle>(back.handle), kSVkLayoutTransferDst, w, h);
        cmd->barrier(back, resource_usage::copy_dest, resource_usage::render_target);
    }
    else
    {
        // The back buffer goes home in the state ReShade left it in whether or
        // not anything was written into it. Only mode 2 writes back here, and
        // mode 1 reads the same as mode 0 -- not the mirror's three modes; see
        // the note on the same branch in Synth12Frame.
        cmd->barrier(back, resource_usage::copy_source, resource_usage::render_target);
    }
    // Depth is not here: it was never moved out of shader_resource. Nor are the
    // motion vectors when the driver's engine is the source, because then that
    // texture was never transitioned either.
    if (!ofa_mv)
        cmd->barrier(mv, resource_usage::copy_source, resource_usage::shader_resource);

    // The present thread's own cost, which is the number this change exists to
    // move: serialised it holds the queue drain, the optical flow and the
    // evaluate; parked or pipelined it is the recording alone. Taken on every
    // frame that got this far, delivered or not, and printed by the worker's line
    // above -- or by the one below.
    g_svp.frame_ms += SynthNowMs() - frame_t0;
    ++g_svp.frame_n;

    // The park counts its own delivered frames in the worker, where the result is
    // the only thing known in time. The pipeline counts them here instead,
    // because here is where the copy back it is counting was actually recorded --
    // the worker's own success is a frame ahead of the picture.
    if (park || !ok) return;

    const UINT64 n = ++g_svk.frames;
    CountDelivered();
    if (n <= 1 || (n % 600) == 0)
    {
        if (pipe)
            // No parked duration in this line, and that is deliberate: the queue
            // is not parked, and printing a zero for it would be a claim about
            // something nothing measured. What is printed instead is the worker's
            // own body, the count of frames whose wait was already satisfied when
            // it was recorded -- which is the evidence the queue did not stop --
            // and the two ordinals, whose difference is the one present of lag
            // this mode costs.
            Log("[synth] Vulkan frame %llu delivered (%ux%u); the worker took %.2f ms over "
                "the last %ld frames it ran on while the game's queue ran on, the present "
                "thread spent %.2f ms in the transport over the last %ld frames, and the "
                "wait was already satisfied when it was recorded on %ld of the frames "
                "that recorded one. Frame "
                "%llu shows the result evaluated for frame %llu.",
                n, w, h, g_svp.work_n != 0 ? g_svp.work_ms / g_svp.work_n : 0.0, g_svp.work_n,
                g_svp.frame_n != 0 ? g_svp.frame_ms / g_svp.frame_n : 0.0, g_svp.frame_n,
                g_svp.free_n, g_svp.shown_on, g_svp.shown_for);
        else
            Log("[synth] Vulkan frame %llu delivered (%ux%u); the present thread spent %.2f ms "
                "in the transport, averaged over the last %ld frames.",
                n, w, h, g_svp.frame_n != 0 ? g_svp.frame_ms / g_svp.frame_n : 0.0, g_svp.frame_n);
        g_svp.frame_ms = 0.0;
        g_svp.frame_n  = 0;
        g_svp.work_ms  = 0.0;
        g_svp.work_n   = 0;
        g_svp.free_n   = 0;
    }
}

// ---------------------------------------------------------------------------
// Arming and the latch
// ---------------------------------------------------------------------------

static bool  g_synth_delivering;
static LONG  g_synth_frames;
static bool  g_synth_armed_said;
static bool  g_synth_shape_said;


// The counters as they stood when the source latch was last released, and the
// release bookkeeping beside them.
//
// g_eval_count and g_create_count are never zeroed -- ReportIdle re-hooks the
// host executable when both read zero past sixty seconds, which is the action
// recorded as killing The Elder Scrolls Online, and g_frames_delivered is the
// one cumulative liveness signal the whole add-on has. So the arming gate is
// given a baseline instead: "has the game used DLSS SINCE we handed the frame
// back", which is the question that test was always trying to ask. Zero for the
// whole life of every session that never releases -- every D3D11 session and
// every session that never mirrored -- so the gate is byte-identical to what
// shipped until the moment a release happens.
//
// Written only by SynthReleaseLatch, on the present thread, inside g_bridge_cs.
static volatile LONG g_eval_base;
static volatile LONG g_create_base;
static volatile LONG g_release_n;      // episodes released this session

// Stage 1's latch, seen from this side. The game's own DLSS wins from any state
// at any time, so this asks only whether it has arrived and never argues about
// it. The two are never both live: this runs before anything else in the
// delivery path, and the mirror sets SRC_MIRROR in the evaluate detour before it
// reaches BridgeFrame at all.
static void SynthStandDown()
{
    if (!g_synth_delivering) return;
    g_synth_delivering = false;

    EnterCriticalSection(&g_bridge_cs);
    // The feature standing in g_bridge was created for the synthetic contract --
    // DLAA at back buffer size, over ReShade's depth and a third-party shader's
    // motion vectors. The game's first frame may well be the same shape, in
    // which case nothing in BridgeFrameInner would notice and the mirror would
    // run the game's textures through somebody else's feature. Clearing
    // frame_ready forces the rebuild; need_reset throws away a temporal history
    // accumulated from motion vectors the game did not produce.
    g_bridge.frame_ready = false;
    g_bridge.need_reset  = true;
    // The D3D12 session's undo is NOT repeated here. The Vulkan mirror's evaluate
    // detour already does it -- same two fields, same critical section --
    // immediately before it calls VkMirrorFrame, which is the only moment it is
    // correct at. This runs on ReShade's present thread, strictly after that
    // detour has released g_bridge_cs, so by the time it could zero the width
    // the mirror has already built; all a second copy achieved was to un-satisfy
    // the build guard behind the mirror's back and buy a rebuild nobody asked
    // for. need_reset is left alone here for the same reason.
    g_s12.need_reset = true;
    // The Vulkan park's copy-back gate, cleared with the rest of it. After this
    // stand-down the MIRROR evaluates the game's own contract into the same
    // g_s12.tex[SLOT_OUTPUT] the substitute reads back from. If the substitute
    // arms again at an unchanged shape the rebuild test in SynthVkFrame does not
    // fire, so nothing else would clear this -- and the first re-armed frame would
    // blit the mirror's last output, the game's own DLSS result, over the back
    // buffer as if the substitute had produced it. Both pairs, and the pipeline's
    // pending state with them: after a stand-down no further frame of this
    // transport's is recorded.
    //
    // A narrowing and not a guarantee, and worth saying so because the next
    // reader will lean on it: this runs on the present thread under g_bridge_cs,
    // a section the worker never takes, and a worker that armed on an earlier
    // frame writes out_valid after this line -- nothing here waits the free
    // events. What actually holds is the rebuild SynthReleaseLatch forces on the
    // way back to arming (g_s12.w = 0), which clears both arrays again with the
    // queue idled, and that is on every SRC_MIRROR -> SRC_NONE path there is.
    // Making this sufficient on its own is a bounded wait on the present thread
    // for a case the rebuild already covers.
    g_svp.out_valid[0] = g_svp.out_valid[1] = false;
    g_svp.pending[0]   = g_svp.pending[1]   = false;
    // With them, because the mirror's own frames never reach the clear at the top
    // of SynthMaybeDeliver -- it returns here, above it -- and a flag left true
    // through a whole mirrored stretch would tell the substitute's first frame
    // back that the frame before it armed.
    g_svp.armed_last   = false;
    LeaveCriticalSection(&g_bridge_cs);

    // What this says is what happens, which is not what it used to say. The
    // synthetic feature is not discarded here: nothing in this function frees an
    // NGX feature or a texture. The release lives in Synth12Build, so it happens
    // when -- and only when -- a later mirrored frame actually reaches a rebuild.
    // Every path that refuses the mirror before then leaves the feature standing
    // for the rest of the process, and a line claiming otherwise is how a leak
    // gets looked for in the wrong place.
    // Not "for good" any more, and the word had to go rather than be softened:
    // the source latch can now be released when the game's own DLSS goes quiet,
    // so a sentence promising the synthetic contract is finished for the session
    // would be contradicted by the next line the same log prints.
    Log("[synth] the game called DLSS itself, so the synthetic contract stands down "
        "after %ld frame%s. It arms again only if the game's own DLSS goes quiet. The "
        "feature it built is released by the next mirrored frame that rebuilds, not here.",
        g_synth_frames, g_synth_frames == 1 ? "" : "s");
}

// Has the game used DLSS since the latch was last handed back? Against the
// BASELINE, not against zero -- see g_eval_base. Both latches increment their
// counter before they write SRC_MIRROR, so SRC_MIRROR strictly implies a
// non-zero count: a release that reset only g_source would be refused forever
// and deliver nothing, which is the reported bug wearing a different panel
// string.
//
// A function rather than two lines inline, so the arming gate and the check
// below read the same test. Putting it back to "!= 0" is the one edit that
// would silently restore the bug, and it now fails an assertion at attach
// instead of shipping.
static bool SynthCountsMovedSince(LONG creates, LONG evals, LONG create_base, LONG eval_base)
{
    return creates != create_base || evals != eval_base;
}

static bool SynthGameUsedDlssSinceBase()
{
    return SynthCountsMovedSince(InterlockedCompareExchange(&g_create_count, 0, 0),
                                 InterlockedCompareExchange(&g_eval_count, 0, 0),
                                 g_create_base, g_eval_base);
}

// Run once at attach. The failure mode being guarded is not a crash: it is the
// un-latch shipping while the game still delivers nothing, which looks exactly
// like working code until somebody toggles DLSS.
static void SynthLatchSelfCheck()
{
    // Shipped behaviour, baselines zero: any use of DLSS refuses arming.
    assert(!SynthCountsMovedSince(0, 0, 0, 0));
    assert( SynthCountsMovedSince(1, 0, 0, 0));
    assert( SynthCountsMovedSince(0, 1, 0, 0));
    // After a release, the counts that stood at release time must NOT refuse --
    // this is the assertion that fails if the test goes back to "!= 0".
    assert(!SynthCountsMovedSince(7, 400, 7, 400));
    // And the game coming back must refuse again.
    assert( SynthCountsMovedSince(7, 401, 7, 400));
    assert( SynthCountsMovedSince(8, 400, 7, 400));
}

// Every condition, each read off something rather than assumed, and the caller
// says which one refused.
//
// The depth readback is not among them, and that is deliberate twice over.
// SynthProbeDepth stands aside whenever ReShade hands over the game's own
// depth-stencil texture, because D3D11 copies such a resource only whole -- which
// is the common case, not an edge one, so requiring a successful readback would
// mean never arming in the configuration that works. Nor can it disprove, which
// this comment used to claim: the readback runs in OnSynthBeginEffects AFTER the
// delivery decision, so depth_read is zero every time this function is asked, and
// the clause that refused a flat buffer could never fire. It has been removed
// rather than left to read as a test that runs. A bound, right-sized, provably
// flat buffer arms; the report, which does run after the readback, is the only
// place "flat" is said.
//
// Not fixed by reordering: moving the probe above the gate would stall the GPU
// every frame, and carrying the verdict forward in a static risks refusing a good
// buffer on a stale reading -- a regression on an untested path, on the last
// release.
// ignore_mv skips the four motion-vector shape tests at the end and nothing
// else. It exists for one caller: the pre-gate that decides whether to run the
// driver's optical flow engine at all, which is asked BEFORE there is a motion
// vector texture because it is what produces one. Without it every mirror
// session in the nine shipped titles would pay 1.6 ms a frame building a flow
// field nobody reads -- and Skyrim SE spent five seconds with depth "present but
// NOT BOUND" before arming, which is 300 frames of it. Every other caller passes
// false and gets exactly the function that was here.
static const char *SynthArmRefusal(const SynthFacts *f, bool ignore_mv)
{
    // First, above every other test: once the transport that would carry the frame
    // has refused this contract, no answer below is the reason any more. It also
    // stops the two optical-flow pre-gates opening an engine for a contract that
    // nothing will deliver.
    if (g_synth_build_dead) return kSynthContractDead;

    // Two questions wear this one key, and only one of them can be answered
    // without evidence.
    //
    // A game whose own DLSS was running and has been switched off is not a
    // guess. g_release_n counts the episodes where the mirror held the session
    // and then went quiet for five seconds and thirty presents, which is the
    // game itself saying it has stopped asking. That case consults no key.
    //
    // A game that has simply not called DLSS YET is a guess. It used to be a
    // second opt-in, synth_after, because on 2026-08-31 arming ten seconds in
    // and then handing over to the game's own contract raised 0xC0000005 inside
    // the DLSS 5 add-on. That fault was the parameter block naming four freed
    // resources at the rebuild, fixed on 2026-09-01 and measured 12 rebuilds out
    // of 12 -- so the guess now costs what a guess should: one rebuild when the
    // game does ask, since its own DLSS wins from any state. One switch covers
    // both cases, and synth_after is only how long the wait is.
    if (!SynthOptedIn())
        return "synth=0: a substitute contract is off by default. It is built from "
               "ReShade's depth and the driver's optical flow, and it looks like it: "
               "text goes soft and dense foliage smears, because optical flow does not "
               "know what a glyph or a leaf is. Set synth=1 to allow it";
    if (g_cfg.source != CFG_SRC_SYNTH && g_cfg.source != CFG_SRC_AUTO)
        return "source is not synth or auto";

    // The super-resolution snippet this process actually loaded. NGX takes the
    // game folder's copy before the driver's, and a game can pin a very old one.
    //
    // This contract is DLAA by construction -- its render size equals its output
    // size -- and DLAA arrived in DLSS SDK 3.1.13. A 2.x runtime has no network
    // for it, and measured in Red Dead Redemption 2 on 2026-09-01 it does not say
    // so: the create returned success at PerfQualityValue 5 against 2.2.10.0 and
    // delivered 1800 frames whose picture degraded progressively into something
    // unrecognisable. The retry at PerfQualityValue 2 further down never fires
    // here, because nothing was refused.
    //
    // The mirror is unaffected: it copies the game's own value and its ratio is
    // whatever the game asked for, which is what that runtime was built to serve.
    {
        static char ver[64];
        const int major = SnippetMajorVersion(ver, sizeof(ver));
        if (major != 0 && major < 3)
        {
            static char why[448];
            _snprintf_s(why, sizeof(why), _TRUNCATE,
                        "the super-resolution snippet in this process is nvngx_dlss.dll %s, "
                        "which predates DLAA (DLSS SDK 3.1.13). A substitute contract is DLAA "
                        "by ratio, that runtime has no network for it and accepts the request "
                        "anyway, and the picture degrades progressively. Replacing the game "
                        "folder's nvngx_dlss.dll with 3.1.13 or newer is what makes this "
                        "usable here; the game's own DLSS is unaffected either way", ver);
            return why;
        }
        // No snippet loaded yet, which is every game without DLSS before the
        // first create. NGX loads nvngx_dlss.dll from the executable's folder
        // and the driver store carries none (measured 2026-09-01: nvngx_dlssg
        // for frame generation, nothing for super resolution), so a folder
        // without the file cannot build the contract at all. Said here, before
        // the create, so the panel and the log name the file to copy rather
        // than reporting FeatureNotFound after the fact. The first sentence
        // stands alone: the panel prints up to the first full stop.
        if (major == 0)
        {
            static wchar_t snippet[MAX_PATH] = {};
            if (snippet[0] == 0)
            {
                GetModuleFileNameW(nullptr, snippet, MAX_PATH);
                wchar_t *sl = wcsrchr(snippet, L'\\');
                if (sl != nullptr) wcscpy_s(sl + 1, MAX_PATH - (sl + 1 - snippet), L"nvngx_dlss.dll");
            }
            if (GetFileAttributesW(snippet) == INVALID_FILE_ATTRIBUTES)
                return "no nvngx_dlss.dll beside the game's executable, so the replacement "
                       "cannot be built. Copy one, 3.1.13 or newer, from any game that has "
                       "DLSS into that folder; the NVIDIA driver does not carry it";
        }
    }
    if (g_cfg.stage < 3)
        return "stage is below 3, so no evaluate is run at all";

    // Not "no evaluate": a game that created a feature has DLSS and has simply
    // not drawn with it yet, and taking the field in front of it would be taken
    // back one frame later.
    if (SynthGameUsedDlssSinceBase())
        return "the game has already used DLSS itself";

    // Only for the FIRST arming. g_hook_time is restarted whenever a new NGX
    // module is hooked, and on the synthetic path this add-on loads the driver's
    // NGX loader itself -- so arming caused new modules to be hooked, which
    // restarted the clock, which un-armed it one frame later. Skyrim Special
    // Edition on 2026-08-30: feature created, frame 1 delivered, and then "the
    // configured delay has not elapsed" for the rest of the session.
    //
    // What the delay is for is deciding whether the game has DLSS of its own,
    // and that question is settled once. What ends a synthetic session is the
    // mirror latch, not a timer.
    if (InterlockedCompareExchange(&g_source, 0, 0) != SRC_SYNTH &&
        GetTickCount64() - g_hook_time < static_cast<ULONGLONG>(SynthAfterSec()) * 1000)
        return "the configured delay has not elapsed";

    if (!f->depth_bound)     return "no depth buffer is bound";
    if (f->depth_samples != 1) return "the depth buffer is multisampled";
    // The same test the motion vectors get below, and it was missing here. NGX
    // takes depth at the render resolution, and the render resolution this
    // contract declares is the back buffer -- ReShade.fxh declares DepthBufferTex
    // at back buffer size, but the binding is not always that texture: generic
    // depth hands over the game's own depth-stencil directly whenever it has no
    // copy to give, and that one is whatever size the game renders at. Handing
    // NGX a depth surface of a size no key in the contract states is a broken
    // contract that only NGX can refuse, one evaluate at a time.
    if (f->depth_w != f->back_w || f->depth_h != f->back_h)
        return "the depth buffer is not the size of the back buffer";


    // Two transport questions rather than contract ones, and the only ones there
    // are, so they are asked here where every other refusal is said.
    //
    // WHAT USED TO BE HERE, and is not any more: a refusal for a Vulkan depth
    // buffer that was not 32-bit float, and a refusal for a Vulkan depth binding
    // that was not a copy source. Both existed because the transport carried
    // depth out through a staging buffer, which is exact for a float depth
    // aspect and for nothing else, and which no transfer may issue at all
    // against an image with no VK_IMAGE_USAGE_TRANSFER_SRC_BIT. The synthetic
    // transport now SAMPLES the depth in a compute pass instead -- see
    // SynthDepthCsRun -- and a sampled depth-aspect texel is a normalised float
    // whatever the storage format, from an image whose usage says
    // shader_resource. So the format question has no answer to refuse on, and
    // the usage question moved from copy_source to shader_resource, which is
    // what generic_depth's backup texture is created with.
    //
    // The MOTION VECTOR copy_source test stays exactly as it was: that texture
    // is still copied, not sampled.
    if (f->device_api == static_cast<uint32_t>(reshade::api::device_api::vulkan))
    {
        const uint32_t shader_r = static_cast<uint32_t>(reshade::api::resource_usage::shader_resource);
        if ((f->depth_usage & shader_r) == 0)
            return "the Vulkan depth binding was not created as a shader resource, so the "
                   "compute pass that reads it cannot sample it -- on Vulkan that binding "
                   "is ReShade's own depth backup texture, which generic_depth creates "
                   "shader_resource and copy_dest, so this is not the expected case";
    }

    if (!ignore_mv)
    {
        if (!f->mv_bound)        return "no motion vector texture is bound";
        // BELOW the bound test, and that is the whole point of where it sits.
        // Above it, this fired on a texture that was declared and never bound --
        // mv_usage is zero because there was no view to read it from, not because
        // the texture was created without copy_source -- and it beat the true
        // sentence one line up. A user with no motion-vector shader enabled was
        // told the creation flags of a texture that had never been created.
        //
        // mv_which 0 is the driver's own optical flow engine -- kSynthMv[0], the
        // row with no ReShade texture behind it -- whose vectors never travel
        // through a Vulkan transfer: they are written into the shared D3D12
        // texture from the other side.
        //
        // Both the device_api test and the constant are restated here rather than
        // reused: copy_src above is scoped to the block this moved out of, and
        // dropping the Vulkan guard would refuse every D3D11 session, where
        // SynthBound::usage is deliberately left zero. That is the nine shipped
        // titles.
        if (f->device_api == static_cast<uint32_t>(reshade::api::device_api::vulkan) &&
            f->mv_which != 0 &&
            (f->mv_usage &
             static_cast<uint32_t>(reshade::api::resource_usage::copy_source)) == 0)
            return "the Vulkan motion vector texture was not created as a copy source, so "
                   "no transfer may read it";
        if (f->mv_samples != 1)  return "the motion vectors are multisampled";
        if (f->mv_fmt != DXGI_FORMAT_R16G16_FLOAT) return "the motion vectors are not R16G16_FLOAT";
        if (f->mv_w != f->back_w || f->mv_h != f->back_h)
            return "the motion vectors are not the size of the back buffer";
    }

    if (f->def_value[SYNTH_DEF_UPSIDE_DOWN])
        return "the depth buffer is upside down and NGX has no way to be told";
    if (f->def_value[SYNTH_DEF_LOGARITHMIC])
        return "the depth encoding is logarithmic and NGX has no way to be told";

    return nullptr;
}

// ---------------------------------------------------------------------------
// The driver's own optical flow engine
//
// Why this exists: Skyrim Special Edition, 2026-08-30. The synthetic contract
// was built, the feature was created, frames were delivered, and fine detail
// dissolved along the direction of movement -- worse slowly than quickly. Five
// observations settled the cause by experiment: all four mv_sign combinations
// smeared, so not the sign; mode=1 was clean, so not the transport;
// reset_every=1 was clean, so it is the temporal accumulation; worse slowly, so
// the sub-pixel regime; the trail ran ALONG the movement, so under-estimated
// rather than reversed. vort_Shaders estimates at mip 1, its finest pass is
// PS_Motion1, there is no PS_Motion0, and below one pixel it reports ~0.
//
// NVOFA never reports zero. Measured on this machine at 1920x1080, MEDIUM,
// "exactly zero: 0.00%" at every step above 0.10 px, and at a true 0.10 px it
// answers 0.156 at grid 2. That is the whole reason to build this.
//
// D3D11 ONLY. The function list below was recovered empirically for
// NvOFAPICreateInstanceD3D11 and for nothing else: the D3D12 and Vulkan entry
// points are different exports with different, unrecovered slot tables and
// different accepted API versions. On those two runtimes the kSynthMv[1..]
// ReShade providers are the only ones, which is what they already were.
//
// NOT MEASURED and not claimed anywhere below: rotation and zoom against real
// game content, transparency, per-object motion, and temporal hints -- every
// number quoted here was taken with disableTemporalHints = 1 over rigid warps of
// a synthetic pattern.
// ---------------------------------------------------------------------------

typedef long NvOfStatus;
static const NvOfStatus kOfaSuccess = 0;      // NV_OF_SUCCESS
static const NvOfStatus kOfaRaised  = 0x7FFFFFFF;   // our own marker, as NGX_EXCEPTION_MARKER is

// Plain C structs from a published ABI, hand-written for the same reason the
// Vulkan structs next door are: there is no lib to link and no header worth
// vendoring for five exports. This layout is additionally confirmed by
// experiment on this machine -- breaking one field made the driver name that
// exact field back through nvOFGetLastError.
struct NvOfInitParams
{
    uint32_t width;               //  0
    uint32_t height;              //  4
    uint32_t outGridSize;         //  8   1, 2 or 4
    uint32_t hintGridSize;        // 12   same value; unread with hints disabled
    uint32_t mode;                // 16   1 = NV_OF_MODE_OPTICALFLOW
    uint32_t perfLevel;           // 20   5 SLOW, 10 MEDIUM, 20 FAST
    uint32_t enableExternalHints; // 24
    uint32_t enableOutputCost;    // 28
    void    *hPrivData;           // 32
    uint32_t disparityRange;      // 40
    uint32_t enableRoi;           // 44
};

struct NvOfExecuteInputParams
{
    void    *inputFrame;           //  0  the CURRENT frame
    void    *referenceFrame;       //  8  the PREVIOUS frame
    void    *externalHints;        // 16
    uint32_t disableTemporalHints; // 24
    uint32_t padding;              // 28
    void    *hPrivData;            // 32
    uint32_t padding2;             // 40
    uint32_t numRois;              // 44
    void    *roiData;              // 48
};

struct NvOfExecuteOutputParams { void *outputBuffer; void *outputCostBuffer; void *hPrivData; };

// The same discipline as the PanelImGui asserts: a miscounted pad is a compile
// error here rather than a call through the wrong offset at runtime.
static_assert(sizeof(NvOfInitParams) == 48 && offsetof(NvOfInitParams, hPrivData) == 32,
              "NvOfInitParams does not match the layout measured against the driver");
static_assert(sizeof(NvOfExecuteInputParams) == 56 &&
              offsetof(NvOfExecuteInputParams, numRois) == 44,
              "NvOfExecuteInputParams does not match the layout measured against the driver");

typedef NvOfStatus (__stdcall *PFN_OfaCreateInstance)(uint32_t, void *);
typedef NvOfStatus (__stdcall *PFN_OfaCreateD3D11)(ID3D11Device *, ID3D11DeviceContext *, void **);
typedef NvOfStatus (__stdcall *PFN_OfaInit)(void *, const NvOfInitParams *);
typedef NvOfStatus (__stdcall *PFN_OfaRegister)(void *, ID3D11Resource *, void **);
typedef NvOfStatus (__stdcall *PFN_OfaUnregister)(void *);
typedef NvOfStatus (__stdcall *PFN_OfaExecute)(void *, const NvOfExecuteInputParams *,
                                               NvOfExecuteOutputParams *);
typedef NvOfStatus (__stdcall *PFN_OfaDestroy)(void *);
typedef NvOfStatus (__stdcall *PFN_OfaLastError)(void *, char *, uint32_t *);

static const char *OfaStatusName(NvOfStatus s)
{
    switch (s)
    {
    case 0:  return "SUCCESS";
    case 1:  return "INVALID_PTR";
    case 2:  return "INVALID_PARAM";
    case 3:  return "INVALID_CALL";
    case 4:  return "INVALID_VERSION";
    case 5:  return "OUT_OF_MEMORY";
    case 6:  return "NOT_INITIALIZED";
    case 7:  return "UNSUPPORTED_FEATURE";
    case 8:  return "GENERIC";
    case 9:  return "OF_NOT_AVAILABLE";
    case 10: return "UNSUPPORTED_DEVICE";
    case 11: return "DEVICE_DOES_NOT_EXIST";
    // A width or height the engine will not take comes back as 0x80004005,
    // which is E_FAIL and not an NV_OF_STATUS at all. Every test below is
    // != kOfaSuccess for exactly that reason, and this names the case rather
    // than reporting it as an enum value nobody can look up.
    case static_cast<NvOfStatus>(0x80004005): return "E_FAIL, not an NV_OF_STATUS";
    default: return "a status this build has no name for";
    }
}

// 5, 10 and 20 are NVOFA's own values; anything else was refused by the config
// validator, so the default arm can only be reached if that check is changed.
static const char *SynthPerfName(int p)
{
    return p == 5 ? "SLOW" : p == 10 ? "MEDIUM" : p == 20 ? "FAST" : "?";
}

struct SynthOfa
{
    bool    tried;      // the first open was attempted; a failed first open is never retried
    bool    ready;      // dll, instance, session, textures, shaders all in hand
    bool    stopped;    // gave up after being ready; no ReShade provider is substituted
    bool    primed;     // src[prev] holds a real frame, so an execute is possible
    bool    first_said;
    HMODULE lib;
    void   *slot[64];
    void   *session;
    UINT    w, h, grid;
    int     perf;       // the NV_OF_PERF_LEVEL this session was opened with
    DXGI_FORMAT back_fmt;               // the shape the session was opened for
    // The device every texture below was created on, compared rather than
    // assumed. A game that re-creates its D3D11 device -- ReShade answers that
    // with a fresh effect runtime and this add-on keeps running -- would
    // otherwise leave a CopyResource between two devices, which is undefined
    // rather than an error. Held as a bare pointer and never dereferenced: the
    // textures below hold their own references, so the address cannot be reused
    // by a second device while this session exists, and the comparison is exact.
    ID3D11Device *dev;
    void   *reg_src[2], *reg_flow;      // registration handles
    ID3D11Texture2D *copy, *src[2], *flow, *mv;
    ID3D11ShaderResourceView  *copy_srv, *flow_srv;
    ID3D11UnorderedAccessView *src_uav[2], *mv_uav;
    ID3D11ComputeShader *luma_cs, *flow_cs;
    int    cur;         // which of src[] this frame writes
    int    fails;       // consecutive execute failures
    bool   checked;     // the self-check has run for this shape
    int    quiet;       // frames seen with no motion anywhere, for the self-check
    UINT64 frames;
};

static SynthOfa g_ofa;

// Each driver entry point gets its own guarded wrapper, for the reason the NGX
// wrappers in dlss5-bridge.cpp were given theirs: these are undocumented
// exports reached through signatures recovered from disassembly, and a wrong
// guess has to land in the log rather than take the game down. Each is its own
// function with no C++ objects, so __try is legal and no unwinding is needed.
static NvOfStatus OfaSafeCreateInstance(PFN_OfaCreateInstance fn, uint32_t ver, void *slots)
{
    __try { return fn(ver, slots); }
    __except (CaptureFault(GetExceptionInformation())) { return kOfaRaised; }
}

static NvOfStatus OfaSafeCreateSession(PFN_OfaCreateD3D11 fn, ID3D11Device *d,
                                       ID3D11DeviceContext *c, void **out)
{
    __try { return fn(d, c, out); }
    __except (CaptureFault(GetExceptionInformation())) { return kOfaRaised; }
}

static NvOfStatus OfaSafeInit(PFN_OfaInit fn, void *s, const NvOfInitParams *p)
{
    __try { return fn(s, p); }
    __except (CaptureFault(GetExceptionInformation())) { return kOfaRaised; }
}

static NvOfStatus OfaSafeRegister(PFN_OfaRegister fn, void *s, ID3D11Resource *r, void **out)
{
    __try { return fn(s, r, out); }
    __except (CaptureFault(GetExceptionInformation())) { return kOfaRaised; }
}

static NvOfStatus OfaSafeUnregister(PFN_OfaUnregister fn, void *h)
{
    __try { return fn(h); }
    __except (CaptureFault(GetExceptionInformation())) { return kOfaRaised; }
}

static NvOfStatus OfaSafeDestroy(PFN_OfaDestroy fn, void *s)
{
    __try { return fn(s); }
    __except (CaptureFault(GetExceptionInformation())) { return kOfaRaised; }
}

static NvOfStatus OfaSafeExecute(PFN_OfaExecute fn, void *s, const NvOfExecuteInputParams *in,
                                 NvOfExecuteOutputParams *out)
{
    __try { return fn(s, in, out); }
    __except (CaptureFault(GetExceptionInformation())) { return kOfaRaised; }
}

// The driver's own account of what it refused, which is what makes an nvOFInit
// failure actionable: it names the offending field. Empty when the call has
// nothing to say or is not safe to make.
static void OfaLastError(char *dst, size_t cap)
{
    dst[0] = '\0';
    if (g_ofa.session == nullptr || g_ofa.slot[8] == nullptr) return;
    uint32_t n = static_cast<uint32_t>(cap);
    auto fn = reinterpret_cast<PFN_OfaLastError>(g_ofa.slot[8]);
    __try { fn(g_ofa.session, dst, &n); }
    __except (CaptureFault(GetExceptionInformation())) { dst[0] = '\0'; }
    // Nothing in the recovered signature promises a terminator, and every caller
    // hands the result straight to a %s.
    dst[cap - 1] = '\0';
}

// Unregister before release, on every path including a half-built open: the
// driver holds these handles and releasing the texture under one is a use after
// free inside the display driver rather than in this process.
//
// lib, slot and the two compiled shaders are deliberately left alone. They are
// shape-independent, and recompiling two compute shaders on a resolution change
// is a D3DCompile for nothing.
static void OfaClose()
{
    auto unreg = reinterpret_cast<PFN_OfaUnregister>(g_ofa.slot[5]);
    if (unreg != nullptr)
    {
        for (int i = 0; i < 2; ++i)
            if (g_ofa.reg_src[i] != nullptr) OfaSafeUnregister(unreg, g_ofa.reg_src[i]);
        if (g_ofa.reg_flow != nullptr) OfaSafeUnregister(unreg, g_ofa.reg_flow);
    }
    g_ofa.reg_src[0] = g_ofa.reg_src[1] = g_ofa.reg_flow = nullptr;

    if (g_ofa.copy_srv != nullptr) { g_ofa.copy_srv->Release(); g_ofa.copy_srv = nullptr; }
    if (g_ofa.flow_srv != nullptr) { g_ofa.flow_srv->Release(); g_ofa.flow_srv = nullptr; }
    if (g_ofa.mv_uav   != nullptr) { g_ofa.mv_uav->Release();   g_ofa.mv_uav   = nullptr; }
    for (int i = 0; i < 2; ++i)
        if (g_ofa.src_uav[i] != nullptr) { g_ofa.src_uav[i]->Release(); g_ofa.src_uav[i] = nullptr; }

    if (g_ofa.copy != nullptr) { g_ofa.copy->Release(); g_ofa.copy = nullptr; }
    for (int i = 0; i < 2; ++i)
        if (g_ofa.src[i] != nullptr) { g_ofa.src[i]->Release(); g_ofa.src[i] = nullptr; }
    if (g_ofa.flow != nullptr) { g_ofa.flow->Release(); g_ofa.flow = nullptr; }
    if (g_ofa.mv   != nullptr) { g_ofa.mv->Release();   g_ofa.mv   = nullptr; }

    // The two compute shaders, which this function did not release. OfaOpen
    // compiles a fresh pair on every shape change, so each change leaked one of
    // each -- and a shader holds a reference on the D3D11 device it was compiled
    // by, so the private device outlived the session that owned it and could not
    // be reclaimed for the life of the process.
    if (g_ofa.luma_cs != nullptr) { g_ofa.luma_cs->Release(); g_ofa.luma_cs = nullptr; }
    if (g_ofa.flow_cs != nullptr) { g_ofa.flow_cs->Release(); g_ofa.flow_cs = nullptr; }

    if (g_ofa.session != nullptr)
    {
        auto destroy = reinterpret_cast<PFN_OfaDestroy>(g_ofa.slot[7]);
        if (destroy != nullptr) OfaSafeDestroy(destroy, g_ofa.session);
        g_ofa.session = nullptr;
    }

    g_ofa.ready   = false;
    g_ofa.primed  = false;
    g_ofa.checked = false;
    g_ofa.fails   = 0;
    g_ofa.quiet   = 0;
    g_ofa.cur     = 0;
    g_ofa.dev     = nullptr;
}

// There is no teardown at DLL_PROCESS_DETACH, and that is deliberate rather than
// an oversight: g_s12 and g_svk have none either, for the same reasons. Releasing
// GPU objects from DllMain under the loader lock is a hazard, most games never
// reach detach at all, and the process is exiting. OfaClose on a shape change is
// the only teardown that exists.

static bool OfaMakeShader(ID3D11Device *dev11, const char *src, const char *name,
                          ID3D11ComputeShader **out)
{
    if (*out != nullptr) return true;

    PFN_D3DCompile_ compile = LoadSystemD3DCompiler();
    if (compile == nullptr) { Log("[synth] optical flow: d3dcompiler_47.dll is unavailable, so the flow "
                                     "conversion shader cannot be built"); return false; }

    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = compile(src, strlen(src), name, nullptr, nullptr, "main", "cs_5_0", 0, 0,
                         &code, &err);
    if (FAILED(hr) || code == nullptr)
    {
        Log("[synth] optical flow shader compile failed 0x%08X: %s", hr,
            err != nullptr ? static_cast<const char *>(err->GetBufferPointer()) : "");
        if (err != nullptr) err->Release();
        return false;
    }
    if (err != nullptr) err->Release();

    hr = dev11->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, out);
    code->Release();
    if (FAILED(hr))
    { Log("[synth] optical flow CreateComputeShader failed 0x%08X", hr); return false; }
    return true;
}

// Grey rather than colour: the estimator matches on structure and a single
// channel is what it reduces to anyway. Reinhard rather than saturate, because
// an R16G16B16A16_FLOAT back buffer clips every highlight to white under
// saturate and the estimator then has nothing to lock onto in exactly the bright
// regions that carry the most detail. Reinhard is monotone, so nothing it
// matches on is reordered.
// Split in two so the middle line can be chosen from the back buffer format.
//
// The engine's input surface is B8G8R8A8_UNORM -- eight bits of luma. The
// Reinhard curve l/(l+1) was written for a float back buffer that can exceed
// 1.0, where compressing the range is the only way to get it into eight bits at
// all. Applied to a UNORM buffer, whose luma is already in [0,1], it maps that
// range onto [0,0.5] and hands the estimator seven bits instead of eight, for
// nothing. Red Dead Redemption 2 and Baldur's Gate 3 both present
// R10G10B10A2_UNORM, so both were paying it, and so is every 8-bit SDR title.
static const char kOfaLumaHead[] =
    "Texture2D<float4>   src : register(t0);\n"
    "RWTexture2D<float4> dst : register(u0);\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "    uint w, h; dst.GetDimensions(w, h);\n"
    "    if (id.x >= w || id.y >= h) return;\n"
    "    float3 c = src[id.xy].rgb;\n"
    "    float  l = dot(c, float3(0.2126, 0.7152, 0.0722));\n";
static const char kOfaLumaTonemap[] = "    l = l / (l + 1.0);\n";
static const char kOfaLumaClamp[]   = "    l = saturate(l);\n";
static const char kOfaLumaTail[] =
    "    dst[id.xy] = float4(l, l, l, 1.0);\n"
    "}\n";

// Three things here are load-bearing and each one is a measurement.
//
//  1/32   the output is S10.5 fixed point, so the stored value is pixels x 32.
//  /(w,h) the contract stores UV and MV.Scale multiplies by (W,H) to get back to
//         pixels, so the round trip is exact.
//  no sign flip: with inputFrame = this frame and referenceFrame = the previous
//         one, the vector already points from the current pixel back to where
//         that content was, which is verbatim the DLSS definition. Measured over
//         1,166,400 vectors, 100% within 0.5 px, both axes.
//
// The reconstruction filter is NEAREST, not bilinear, and that is the one place
// this departs from what the transport note first sketched. The disocclusion
// band is 12-16 px wide in pixels at every grid, and the values inside it
// correspond to no surface in the scene. Bilinear across that band manufactures
// more such values and spreads it wider; nearest quantises but never invents a
// vector no cell measured. Where the field is smooth it varies far more slowly
// than one grid cell and both filters land inside the 0.1 px noise floor.
//
// id.x * fw / w is the exact integer divide whenever fw*grid == w, which is
// every resolution in practice; the min keeps the sample inside the texture at
// the ones where it is not.
static const char kOfaFlowSrc[] =
    "Texture2D<int2>     flow : register(t0);\n"
    "RWTexture2D<float2> dst  : register(u0);\n"
    "[numthreads(8,8,1)]\n"
    "void main(uint3 id : SV_DispatchThreadID)\n"
    "{\n"
    "    uint w, h, fw, fh;\n"
    "    dst.GetDimensions(w, h);\n"
    "    flow.GetDimensions(fw, fh);\n"
    "    if (id.x >= w || id.y >= h) return;\n"
    "    uint2 c = min(uint2(id.x * fw / w, id.y * fh / h), uint2(fw - 1, fh - 1));\n"
    "    dst[id.xy] = float2(flow[c]) * (1.0 / 32.0) / float2(w, h);\n"
    "}\n";

static ID3D11Texture2D *OfaMakeTexture(ID3D11Device *dev11, UINT w, UINT h, DXGI_FORMAT fmt,
                                       UINT bind, const char *what)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = fmt; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = bind;

    ID3D11Texture2D *t = nullptr;
    const HRESULT hr = dev11->CreateTexture2D(&td, nullptr, &t);
    if (FAILED(hr))
    {
        Log("[synth] optical flow: could not create the %ux%u %s texture for %s: 0x%08X",
            w, h, FormatName(fmt), what, hr);
        return nullptr;
    }
    return t;
}
// The private D3D11 device the Vulkan transport runs the engine on, or null when
// there is none. Declared here because OfaOpen has to name which device it opened
// on and g_ovk is defined far below it.
static ID3D11Device *SynthOfaVkDevice();


static bool OfaOpen(ID3D11Device *dev11, ID3D11DeviceContext *ctx, UINT w, UINT h,
                    DXGI_FORMAT back_fmt)
{
    // Before anything that can fail, exactly as Synth12Open and SynthVkOpen set
    // theirs: a first open that failed is never retried.
    g_ofa.tried = true;
    g_ofa.w = w; g_ofa.h = h; g_ofa.back_fmt = back_fmt; g_ofa.dev = dev11;
    g_ofa.grid = static_cast<UINT>(g_cfg.ofa_grid);
    // Validated where it is divided by rather than only where it is set. Every
    // caller checks for zero before getting here, and that is exactly the kind of
    // agreement that a later caller breaks silently: the two divisions below turn a
    // zero into an integer divide fault, which takes the game with it rather than
    // refusing. The config parser accepts 0 as "engine off", so this value reaches
    // the function from a user's file and from a panel click.
    if (g_ofa.grid == 0)
    {
        Log("[synth] optical flow: ofa_grid is 0, which switches the engine off, so "
            "the session is not opened. The motion vectors come from a ReShade effect.");
        return false;
    }

    if (g_ofa.lib == nullptr)
    {
        g_ofa.lib = LoadLibraryW(L"nvofapi64.dll");
        if (g_ofa.lib == nullptr)
        {
            Log("[synth] optical flow: nvofapi64.dll is not loadable in this process. It is "
                "installed with the display driver, so this means no NVIDIA driver here "
                "rather than a driver too old.");
            return false;
        }
        auto create = reinterpret_cast<PFN_OfaCreateInstance>(
            GetProcAddress(g_ofa.lib, "NvOFAPICreateInstanceD3D11"));
        if (create == nullptr)
        {
            Log("[synth] optical flow: nvofapi64.dll exports no NvOFAPICreateInstanceD3D11.");
            return false;
        }
        // 0x20 is the public 2.0 layout, which is the one the structs above were
        // written to and confirmed against on this machine. The D3D11 entry was
        // measured to accept 0x10 through 0x50.
        const NvOfStatus r = OfaSafeCreateInstance(create, 0x20, g_ofa.slot);
        if (r != kOfaSuccess)
        {
            Log("[synth] optical flow: this driver refused API version 0x20 with status %ld "
                "(%s). The D3D11 entry accepts 0x10 through 0x50 on the driver this was "
                "measured against.", r, OfaStatusName(r));
            memset(g_ofa.slot, 0, sizeof(g_ofa.slot));
            return false;
        }
    }

    // Asked once, before a texture exists. BGRA8 typed UAV store is not in
    // D3D11's guaranteed set, and without it the conversion writes nothing and
    // the engine is handed a blank frame -- which looks like bad flow rather
    // than like a missing feature.
    {
        D3D11_FEATURE_DATA_FORMAT_SUPPORT2 f2 = {};
        f2.InFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        if (FAILED(dev11->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &f2, sizeof(f2))) ||
            (f2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0)
        {
            Log("[synth] optical flow: this device cannot store to a B8G8R8A8_UNORM "
                "unordered access view, so the colour frame cannot be converted into the "
                "only 4-channel format the flow engine accepts.");
            return false;
        }
    }

    {
        const bool hdrish = SynthFormatCanExceedOne(back_fmt);
        char luma[sizeof(kOfaLumaHead) + sizeof(kOfaLumaTonemap) + sizeof(kOfaLumaTail) + 8];
        _snprintf_s(luma, sizeof(luma), _TRUNCATE, "%s%s%s", kOfaLumaHead,
                    hdrish ? kOfaLumaTonemap : kOfaLumaClamp, kOfaLumaTail);
        if (!OfaMakeShader(dev11, luma, "ofaluma", &g_ofa.luma_cs)) return false;
        Log("[synth] optical flow: luma is %s, because the back buffer is %s.",
            hdrish ? "tone-mapped with l/(l+1) before the 8-bit conversion"
                   : "clamped rather than tone-mapped, so all eight bits of the "
                     "engine's input carry signal",
            FormatName(back_fmt));
    }
    if (!OfaMakeShader(dev11, kOfaFlowSrc, "ofaflow", &g_ofa.flow_cs)) return false;

    const UINT fw = (w + g_ofa.grid - 1) / g_ofa.grid;
    const UINT fh = (h + g_ofa.grid - 1) / g_ofa.grid;

    // The back buffer is not guaranteed to carry BIND_SHADER_RESOURCE -- a DXGI
    // swapchain only promises a render target view -- so a compute shader cannot
    // read it directly. CopyResource into a texture we created with the same
    // format always works.
    g_ofa.copy = OfaMakeTexture(dev11, w, h, TypedFormat(back_fmt),
                                D3D11_BIND_SHADER_RESOURCE, "the colour copy");
    // BGRA8 because the engine's legal input list is BGRA8, NV12 and R8_UNORM,
    // and Skyrim SE presents R8G8B8A8_UNORM. SHADER_RESOURCE alongside the UAV
    // because that is the binding the registration was proven with in the
    // harness, and it costs nothing.
    for (int i = 0; i < 2; ++i)
        g_ofa.src[i] = OfaMakeTexture(dev11, w, h, DXGI_FORMAT_B8G8R8A8_UNORM,
                                      D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
                                      i == 0 ? "the current frame" : "the previous frame");
    g_ofa.flow = OfaMakeTexture(dev11, fw, fh, DXGI_FORMAT_R16G16_SINT,
                                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                                "the flow field");
    // Exactly the shape SynthArmRefusal already demands of a motion vector
    // texture, so the gate needs no new rule. That is the whole point of
    // converting here rather than handing NGX the engine's own output.
    g_ofa.mv = OfaMakeTexture(dev11, w, h, DXGI_FORMAT_R16G16_FLOAT,
                              D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
                              "the motion vectors");
    if (g_ofa.copy == nullptr || g_ofa.src[0] == nullptr || g_ofa.src[1] == nullptr ||
        g_ofa.flow == nullptr || g_ofa.mv == nullptr)
    { OfaClose(); return false; }

    HRESULT hr = dev11->CreateShaderResourceView(g_ofa.copy, nullptr, &g_ofa.copy_srv);
    if (SUCCEEDED(hr)) hr = dev11->CreateShaderResourceView(g_ofa.flow, nullptr, &g_ofa.flow_srv);
    for (int i = 0; i < 2 && SUCCEEDED(hr); ++i)
        hr = dev11->CreateUnorderedAccessView(g_ofa.src[i], nullptr, &g_ofa.src_uav[i]);
    if (SUCCEEDED(hr)) hr = dev11->CreateUnorderedAccessView(g_ofa.mv, nullptr, &g_ofa.mv_uav);
    if (FAILED(hr))
    {
        Log("[synth] optical flow: a view over one of the four textures could not be "
            "created: 0x%08X", hr);
        OfaClose();
        return false;
    }

    LogVideoMemory("before opening the optical flow session");

    auto create_ses = reinterpret_cast<PFN_OfaCreateD3D11>(g_ofa.slot[0]);
    NvOfStatus r = OfaSafeCreateSession(create_ses, dev11, ctx, &g_ofa.session);
    if (r != kOfaSuccess || g_ofa.session == nullptr)
    {
        // The real case this covers: nvofapi64.dll is present because an NVIDIA
        // GPU is installed, while the game renders on the iGPU.
        Log("[synth] optical flow: the driver will not open a session on this D3D11 device: "
            "status %ld (%s). This is what a game rendering on an adapter other than the "
            "NVIDIA one looks like, which happens on a hybrid laptop.", r, OfaStatusName(r));
        g_ofa.session = nullptr;
        OfaClose();
        return false;
    }

    NvOfInitParams ip;
    memset(&ip, 0, sizeof(ip));
    ip.width = w; ip.height = h;
    ip.outGridSize = g_ofa.grid;
    ip.hintGridSize = g_ofa.grid;      // unread with hints disabled; set to match
    ip.mode = 1;                       // NV_OF_MODE_OPTICALFLOW
    // From the config, validated there to 5, 10 or 20 -- NVOFA's own
    // NV_OF_PERF_LEVEL values, so a number quoted in a report reads against
    // NVIDIA's own table. FAST by default because it is the only level that
    // fits a frame at 4K: measured at 3840x1600 grid 2, FAST costs 1.6 ms
    // where MEDIUM costs 6.2 and SLOW 11.5. The slower levels estimate more
    // accurately, which is the knob to reach for when fine detail drifts.
    ip.perfLevel = static_cast<uint32_t>(g_cfg.ofa_perf);
    g_ofa.perf   = g_cfg.ofa_perf;
    auto init = reinterpret_cast<PFN_OfaInit>(g_ofa.slot[1]);
    r = OfaSafeInit(init, g_ofa.session, &ip);
    if (r != kOfaSuccess)
    {
        // Tested against SUCCESS rather than switched on the enum, because a
        // width or height error comes back as E_FAIL, which is not one.
        char msg[512];
        OfaLastError(msg, sizeof(msg));
        Log("[synth] optical flow: nvOFInit refused %ux%u at grid %u with 0x%08lX (%s). %s",
            w, h, g_ofa.grid, static_cast<unsigned long>(r), OfaStatusName(r), msg);
        if (r == 5)   // NV_OF_ERR_OUT_OF_MEMORY
            Log("[synth]   Grid 2 needs about 109 MB driver-side at 3840x1600; grid 4 needs "
                "about 63 MB. Try ofa_grid=4 in dlss5-bridge.cfg.");
        OfaClose();
        return false;
    }

    auto reg = reinterpret_cast<PFN_OfaRegister>(g_ofa.slot[4]);
    NvOfStatus r0 = OfaSafeRegister(reg, g_ofa.session, g_ofa.src[0],  &g_ofa.reg_src[0]);
    NvOfStatus r1 = OfaSafeRegister(reg, g_ofa.session, g_ofa.src[1],  &g_ofa.reg_src[1]);
    NvOfStatus r2 = OfaSafeRegister(reg, g_ofa.session, g_ofa.flow,    &g_ofa.reg_flow);
    if (r0 != kOfaSuccess || r1 != kOfaSuccess || r2 != kOfaSuccess ||
        g_ofa.reg_src[0] == nullptr || g_ofa.reg_src[1] == nullptr || g_ofa.reg_flow == nullptr)
    {
        char msg[512];
        OfaLastError(msg, sizeof(msg));
        Log("[synth] optical flow: registering the three buffers was refused (%ld, %ld, %ld). %s",
            r0, r1, r2, msg);
        OfaClose();
        return false;
    }

    g_ofa.ready   = true;
    g_ofa.stopped = false;

    // Which device, said rather than assumed. This named "the game's own D3D11
    // device" unconditionally, and on the Vulkan transport that is false: the
    // game has no D3D11 device there and the session opens on a private one this
    // add-on created. The two are told apart by the pointer, because g_ovk.dev is
    // non-null only when this file made it.
    Log("[synth] optical flow: nvofapi64.dll loaded, instance at API 0x20, session open on "
        "%s. Grid %u, perf %s, flow %ux%u R16G16_SINT -> %ux%u "
        "R16G16_FLOAT. This is kSynthMv[0] and the three ReShade motion-vector names are "
        "not consulted while it is running.",
        (SynthOfaVkDevice() != nullptr && SynthOfaVkDevice() == dev11)
            ? "a private D3D11 device this add-on created, because the game is on Vulkan"
            : "the game's own D3D11 device",
        g_ofa.grid, SynthPerfName(g_ofa.perf), fw, fh, w, h);
    Log("[synth] optical flow conversion shaders ready (cs_5_0)");
    LogVideoMemory("after opening the optical flow session");
    return true;
}

// One compute dispatch, with the game's own compute bindings saved and put back
// exactly as BridgeBlitDepth does it. This runs on the immediate context in the
// middle of the game's frame. Two separate calls rather than one block spanning
// both dispatches, because nvOFExecute runs between them on the same context.
static void OfaDispatch(ID3D11DeviceContext *ctx, ID3D11ComputeShader *cs,
                        ID3D11ShaderResourceView *srv, ID3D11UnorderedAccessView *uav,
                        UINT w, UINT h)
{
    ID3D11ComputeShader       *old_cs  = nullptr;
    ID3D11ShaderResourceView  *old_srv = nullptr;
    ID3D11UnorderedAccessView *old_uav = nullptr;
    ctx->CSGetShader(&old_cs, nullptr, nullptr);
    ctx->CSGetShaderResources(0, 1, &old_srv);
    ctx->CSGetUnorderedAccessViews(0, 1, &old_uav);

    UINT keep = static_cast<UINT>(-1);
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->CSSetShaderResources(0, 1, &srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, &keep);
    ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

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
}

// The one runnable check this stage leaves behind, and it ships enabled. It
// checks the arithmetic rather than the plumbing -- the plumbing announces its
// own failures above -- by recomputing what the conversion shader should have
// written from the flow texture the DRIVER actually filled, not from anything
// this add-on assumed.
//
// It runs once per shape, and only on a frame whose flow field contains a
// non-zero vector: a frame with no motion proves nothing, because 0 == 0 passes
// a shader that has the scale wrong, the grid ratio wrong, or is not running at
// all.
static void OfaSelfCheck(ID3D11Device *dev11, ID3D11DeviceContext *ctx)
{
    // The first attempt is taken immediately, because the frame right after
    // priming is the one most likely to have motion in it. Every attempt after
    // that waits for a multiple of sixteen: each one stages and maps about 30 MB
    // and blocks until the GPU has caught up, and a game that opens on a static
    // menu would otherwise take that stall on a hundred frames in a row with
    // nothing in the log to attribute the hitch to.
    if (g_ofa.quiet > 0 && (g_ofa.frames & 15) != 0) return;

    // Three assertions off GetDesc alone, which cost nothing and catch a
    // mis-sized texture before a single texel is read.
    D3D11_TEXTURE2D_DESC fd = {}, md = {};
    g_ofa.flow->GetDesc(&fd);
    g_ofa.mv->GetDesc(&md);
    const UINT want_fw = (g_ofa.w + g_ofa.grid - 1) / g_ofa.grid;
    const UINT want_fh = (g_ofa.h + g_ofa.grid - 1) / g_ofa.grid;
    if (fd.Width != want_fw || fd.Height != want_fh || fd.Format != DXGI_FORMAT_R16G16_SINT ||
        md.Width != g_ofa.w || md.Height != g_ofa.h || md.Format != DXGI_FORMAT_R16G16_FLOAT ||
        g_ofa.flow == reinterpret_cast<ID3D11Texture2D *>(g_ofa.mv))
    {
        g_ofa.checked = true;
        Log("[synth] DEFECT: the optical flow textures are not the shape this build claims. "
            "Flow is %ux%u fmt %u, expected %ux%u R16G16_SINT; motion vectors are %ux%u "
            "fmt %u, expected %ux%u R16G16_FLOAT.",
            fd.Width, fd.Height, fd.Format, want_fw, want_fh,
            md.Width, md.Height, md.Format, g_ofa.w, g_ofa.h);
        return;
    }

    D3D11_TEXTURE2D_DESC sd = fd;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D *fs = nullptr, *ms = nullptr;
    if (FAILED(dev11->CreateTexture2D(&sd, nullptr, &fs))) return;
    sd = md;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    if (FAILED(dev11->CreateTexture2D(&sd, nullptr, &ms))) { fs->Release(); return; }

    ctx->CopyResource(fs, g_ofa.flow);
    ctx->CopyResource(ms, g_ofa.mv);

    D3D11_MAPPED_SUBRESOURCE fm = {}, mm = {};
    if (FAILED(ctx->Map(fs, 0, D3D11_MAP_READ, 0, &fm)) ||
        FAILED(ctx->Map(ms, 0, D3D11_MAP_READ, 0, &mm)))
    { fs->Release(); ms->Release(); return; }

    int   sampled = 0, bad = 0, nonzero = 0;
    float worst = 0.0f;
    // How far past its own tolerance the worst offender was, so a real defect
    // report carries a number that is not confusable with rounding.
    float worst_over = 0.0f;
    int   bx = 0, by = 0, bfx = 0, bfy = 0;
    float bex = 0.0f, bey = 0.0f, bax = 0.0f, bay = 0.0f;

    for (UINT cy = 0; cy < fd.Height; cy += 16)
    {
        const int16_t *frow = reinterpret_cast<const int16_t *>(
            static_cast<const unsigned char *>(fm.pData) + static_cast<size_t>(cy) * fm.RowPitch);
        const UINT py = cy * g_ofa.grid + g_ofa.grid / 2;
        if (py >= md.Height) continue;
        const unsigned short *mrow = reinterpret_cast<const unsigned short *>(
            static_cast<const unsigned char *>(mm.pData) + static_cast<size_t>(py) * mm.RowPitch);

        for (UINT cx = 0; cx < fd.Width; cx += 16)
        {
            const UINT px = cx * g_ofa.grid + g_ofa.grid / 2;
            if (px >= md.Width) continue;

            const int fx = frow[cx * 2], fy = frow[cx * 2 + 1];
            if (fx != 0 || fy != 0) ++nonzero;

            // expected: what the shader should have written, computed from the
            // flow texture the driver actually filled.
            const float ex = static_cast<float>(fx) * (1.0f / 32.0f) / static_cast<float>(g_ofa.w);
            const float ey = static_cast<float>(fy) * (1.0f / 32.0f) / static_cast<float>(g_ofa.h);
            const float ax = DirectX::PackedVector::XMConvertHalfToFloat(mrow[px * 2]);
            const float ay = DirectX::PackedVector::XMConvertHalfToFloat(mrow[px * 2 + 1]);

            // Compared in PIXELS, which is the unit a reader of the log can act
            // on. The tolerance has to scale, and the flat 0.02 px it used to be
            // was below the storage precision:
            //
            // The texture holds the vector in UV, in half-float. Half has a
            // 10-bit mantissa, so the step between representable values at
            // magnitude m is about m * 2^-10 -- and multiplied back by the axis
            // that is (m * axis) * 2^-10, which is a PIXEL magnitude times 2^-10.
            // At 36.5 px one step is 0.0293 px, larger than the 0.02 px this
            // compared against. Measured in Red Dead Redemption 2 on 2026-09-01:
            // expected -36.500 px, stored -36.475 px, a difference of 0.025 px
            // that is less than one representable step -- and the log reported a
            // DEFECT, told the user the image would smear, and blamed this
            // add-on for arithmetic that was exact.
            //
            // The comment that justified 0.02 said rounding was "about 5e-4 px at
            // these magnitudes", which is the relative precision without the
            // multiplication back into pixels.
            //
            // 2^-9 rather than 2^-10 leaves one step of headroom, and still
            // catches every way this can be wrong: a missing /32 errs by 32x, a
            // wrong grid ratio picks another cell's vector entirely, a Y flip
            // errs by twice the value, and a shader that never ran leaves zeros.
            const float dx = fabsf(ax - ex) * static_cast<float>(g_ofa.w);
            const float dy = fabsf(ay - ey) * static_cast<float>(g_ofa.h);
            const float d  = dx > dy ? dx : dy;
            const float mx = fabsf(ex) * static_cast<float>(g_ofa.w);
            const float my = fabsf(ey) * static_cast<float>(g_ofa.h);
            const float tol = 0.02f + (mx > my ? mx : my) * (1.0f / 512.0f);
            ++sampled;
            if (d > worst)
            {
                worst = d;
                bx = static_cast<int>(cx); by = static_cast<int>(cy);
                bfx = fx; bfy = fy;
                bex = ex * static_cast<float>(g_ofa.w); bey = ey * static_cast<float>(g_ofa.h);
                bax = ax * static_cast<float>(g_ofa.w); bay = ay * static_cast<float>(g_ofa.h);
            }
            if (d > tol) ++bad;
            if (d > tol && d > worst_over) worst_over = d - tol;
        }
    }

    ctx->Unmap(fs, 0);
    ctx->Unmap(ms, 0);
    fs->Release();
    ms->Release();

    if (nonzero == 0)
    {
        // A still frame disproves nothing, so this is not a verdict. Same voice
        // as the depth report's "unverified".
        // Eight attempts, one immediately and the rest sixteen frames apart, so
        // the window is still about 120 frames wide -- the count is of what was
        // actually looked at, because that is the number a reader can check.
        if (++g_ofa.quiet >= 8)
        {
            g_ofa.checked = true;
            Log("[synth] optical flow self-check: 8 checks spread over the first 120 or so "
                "frames found no non-zero vector anywhere, so the conversion could not be "
                "checked. Nothing was disproved.");
        }
        return;
    }

    g_ofa.checked = true;
    if (bad == 0)
        Log("[synth] optical flow self-check: %d cells sampled, flow %ux%u R16G16_SINT -> "
            "%ux%u R16G16_FLOAT, worst disagreement %.3f px. The conversion, the 1/32 scale "
            "and the grid mapping are what this build claims they are.",
            sampled, fd.Width, fd.Height, g_ofa.w, g_ofa.h, worst);
    else
        // Delivery continues. This is a defect report about this add-on, and
        // refusing to deliver would hide the numbers a report needs. The text
        // names the likely cause by construction, because it prints both numbers
        // in pixels rather than a verdict: a factor of exactly 32 is the missing
        // scale.
        Log("[synth] DEFECT: the optical flow conversion disagrees with its own input at "
            "cell (%d,%d): flow (%+d,%+d) is %+.3f,%+.3f px, and the motion vector texture "
            "holds %+.3f,%+.3f px. %d of %d cells sampled disagree by more than their own "
            "storage precision, the worst by %.3f px beyond it. The image will smear; this "
            "is a defect in this add-on and not in the estimator.",
            bx, by, bfx, bfy, bex, bey, bax, bay, bad, sampled, worst_over);
}

// One frame. Returns true when out holds a motion vector texture NGX may read.
//
// The copy, both dispatches and the execute are timed together rather than the
// execute alone, because all of it is new cost -- and because nvOFExecute blocks
// the calling thread, which here is ReShade's present thread, so this is CPU
// time inside the game's frame rather than merely GPU time. The one-off session
// open is outside the bracket; see the note at the QueryPerformanceCounter.
// The body, over a colour texture and a device the caller supplies. Two callers:
// OfaFrame below, which is the D3D11 game's own device and its back buffer, and
// SynthOfaVkFrame, which is a private D3D11 device and the D3D12 shared texture
// the Vulkan frame was already copied into. Extracted rather than duplicated --
// everything here except where the two pointers come from is the same code, and
// a second copy of an nvOFExecute call with its own failure counting is exactly
// the thing that drifts.
//
// dev11, ctx and src are BORROWED. This function releases none of them.
static bool OfaFrameOn(ID3D11Device *dev11, ID3D11DeviceContext *ctx,
                       ID3D11Texture2D *src, DXGI_FORMAT src_fmt,
                       UINT w, UINT h, SynthBound *out)
{
    SynthBound blank = {};
    *out = blank;
    if (dev11 == nullptr || ctx == nullptr || src == nullptr) return false;

    LARGE_INTEGER t0, t1;
    bool delivered  = false;
    bool want_check = false;

    // A resolution change, a format change, or an ofa_grid or ofa_perf edit
    // rebuilds the session in place. tried is not consulted here: it governs the
    // first open only, and a failed re-open is recorded by stopped instead.
    //
    // perf was absent from this test until the panel gained a control for it.
    // NV_OF_PERF_LEVEL is only read at nvOFInit, so editing ofa_perf mid-session
    // was accepted by the parser, passed the validator, was reported as a config
    // change -- and estimated at the old level for the rest of the session, with
    // the log still printing the old name. One clause, and the teardown below is
    // the one ofa_grid already proved.
    if (g_ofa.ready && (g_ofa.w != w || g_ofa.h != h || g_ofa.back_fmt != src_fmt ||
                        g_ofa.dev != dev11 ||
                        g_ofa.grid != static_cast<UINT>(g_cfg.ofa_grid) ||
                        g_ofa.perf != g_cfg.ofa_perf))
    {
        Log("[synth] optical flow: the back buffer became %ux%u %s at grid %d perf %s on "
            "device %p, so the session is rebuilt.", w, h, FormatName(src_fmt),
            g_cfg.ofa_grid, SynthPerfName(g_cfg.ofa_perf),
            static_cast<void *>(dev11));
        OfaClose();
        if (!OfaOpen(dev11, ctx, w, h, src_fmt))
        {
            Log("[synth] optical flow: the session could not be rebuilt for the new size, "
                "so this stops.");
            g_ofa.stopped = true;
        }
    }
    else if (!g_ofa.ready && !g_ofa.tried)
    {
        OfaOpen(dev11, ctx, w, h, src_fmt);
    }

    // Nothing below ran, so nothing below is charged for. Counting a frame here
    // would put an "optical flow" clause on the timing line of a session that
    // fell back to a ReShade provider and never ran the engine at all, which is
    // the one thing that line exists to be able to deny.
    if (!g_ofa.ready) return false;

    // Started here rather than at the top of the function, so a one-off session
    // open -- two shader compiles, five textures and an nvOFInit -- is not
    // averaged into the per-frame figure it would otherwise dominate. What is
    // measured from here is the copy, both dispatches and the execute: all of
    // it new cost, and all of it on ReShade's present thread.
    QueryPerformanceCounter(&t0);

    // The colour frame, converted into the only 4-channel format the engine
    // takes. One copy and one dispatch; the copy is ~0.03 ms of bandwidth at
    // 3840x1600 against 1.6 ms of execute, which is why there is one path here
    // and not a second that tries an SRV over the back buffer first.
    ctx->CopyResource(g_ofa.copy, src);
    OfaDispatch(ctx, g_ofa.luma_cs, g_ofa.copy_srv, g_ofa.src_uav[g_ofa.cur], w, h);

    if (!g_ofa.primed)
    {
        g_ofa.primed = true;
        g_ofa.cur ^= 1;
        if (!g_ofa.first_said)
        {
            g_ofa.first_said = true;
            Log("[synth] optical flow: the first frame has no previous frame to compare "
                "against, so nothing is delivered on it. Flow starts on the next one.");
        }
        goto done;
    }

    {
        NvOfExecuteInputParams  in;
        NvOfExecuteOutputParams eo;
        memset(&in, 0, sizeof(in));
        memset(&eo, 0, sizeof(eo));
        in.inputFrame           = g_ofa.reg_src[g_ofa.cur];       // the CURRENT frame
        in.referenceFrame       = g_ofa.reg_src[g_ofa.cur ^ 1];   // the PREVIOUS frame
        in.disableTemporalHints = 1;   // what every measurement for this was taken with
        eo.outputBuffer         = g_ofa.reg_flow;

        auto exec = reinterpret_cast<PFN_OfaExecute>(g_ofa.slot[6]);
        const NvOfStatus r = OfaSafeExecute(exec, g_ofa.session, &in, &eo);
        ++g_ofa.frames;

        if (r == kOfaRaised)
        {
            // Calling back into a component that faulted is what turned a
            // disabled add-on into a killed process on Final Fantasy VII Remake.
            Log("[synth] optical flow: nvOFExecute raised an exception, so this stops here. "
                "The game keeps rendering on its own.");
            g_ofa.ready   = false;
            g_ofa.stopped = true;
            goto done;
        }
        if (r != kOfaSuccess)
        {
            if (g_ofa.fails == 0 || g_ofa.fails == 2)
            {
                char msg[512];
                OfaLastError(msg, sizeof(msg));
                Log("[synth] optical flow: execute %llu refused with status %ld (%s). %s",
                    g_ofa.frames, r, OfaStatusName(r), msg);
            }
            // This frame is not delivered and the game's own frame goes to screen
            // untouched.
            //
            // The slot still advances, and it has to. The luma dispatch above has
            // ALREADY written this frame into src[cur], destroying what was there --
            // so the frame this comment used to call "the previous frame, kept" is
            // gone whatever happens next. Leaving cur alone made the following
            // frame overwrite src[cur] again and compare against a reference two
            // frames old, reporting two frames of motion as one. Advancing it
            // costs the one comparison that was already lost and puts the next
            // pair back in step.
            //
            // Never fired in the sessions measured so far -- Red Dead Redemption 2
            // delivered 1800 frames out of 1800 with no refusal -- so this is a
            // defect found by reading rather than one seen.
            g_ofa.cur ^= 1;
            if (++g_ofa.fails >= 3)
            {
                Log("[synth] optical flow: three consecutive failures, so this stops. No "
                    "ReShade motion-vector texture is substituted: the temporal history "
                    "DLSS has accumulated was built from these vectors and swapping the "
                    "estimator under it is the defect this path was rebuilt to avoid.");
                g_ofa.ready   = false;
                g_ofa.stopped = true;
            }
            goto done;
        }

        g_ofa.fails = 0;
        OfaDispatch(ctx, g_ofa.flow_cs, g_ofa.flow_srv, g_ofa.mv_uav, w, h);
        // Wanted, not run: the check stages and maps about 30 MB and blocks
        // until the GPU catches up, and inside the bracket below that stall
        // would be charged to the per-frame flow figure -- the same error the
        // bracket was moved down the function to avoid for the session open.
        want_check = !g_ofa.checked;

        // No AddRef, and for the reason written over the Get in SynthParams:
        // SynthOfa owns mv for the life of the shape and nothing downstream
        // releases what it takes out of the contract.
        out->res     = reshade::api::resource { reinterpret_cast<uintptr_t>(g_ofa.mv) };
        out->tex11   = nullptr;
        out->w       = w;
        out->h       = h;
        out->fmt     = DXGI_FORMAT_R16G16_FLOAT;
        out->samples = 1;
        out->usage   = 0;

        // Advanced HERE and not at the exit, and the difference is not
        // housekeeping. On a refused execute this is deliberately skipped, so
        // the next frame overwrites src[cur] and keeps src[cur^1] -- which is
        // the input of the last execute that SUCCEEDED, and therefore the last
        // frame DLSS was actually given. A refused frame is not delivered, so
        // DLSS's history is still at that same frame, and the vector it needs is
        // the one spanning the whole gap rather than the last hop of it.
        // Advancing unconditionally would hand a two-frame gap a one-frame
        // vector and under-reproject the history by exactly one frame, which is
        // the smear this path was rebuilt to remove, reintroduced on the
        // recovery frame.
        g_ofa.cur ^= 1;
        delivered = true;
    }

done:
    QueryPerformanceCounter(&t1);
    g_bridge.ofa_ticks += (t1.QuadPart - t0.QuadPart);
    ++g_bridge.ofa_frames;

    // Outside the bracket on purpose. It runs at most a handful of times in a
    // session and it blocks on the GPU; charging it to the per-frame figure
    // would make the first report of a working session read three times its
    // real cost.
    if (want_check) OfaSelfCheck(dev11, ctx);

    // TimingTick is reached only through BridgeFrame, so a session that runs the
    // engine every frame and never delivers one would spend this and never
    // report it -- and a blocking call in the game's present thread that no line
    // in the log accounts for is precisely what this measurement exists to make
    // impossible. Said here when 600 flow frames have gone by with not one
    // delivered frame to carry the figure.
    if (g_bridge.ofa_frames >= 600 && g_bridge.timed_frames == 0)
    {
        // Asked for here rather than read off g_bridge.qpf: that field is filled
        // by TimingTick, and TimingTick not having run is the whole condition
        // this branch is testing for.
        LARGE_INTEGER qpf;
        QueryPerformanceFrequency(&qpf);
        Log("[synth] optical flow: %.2f ms/frame over %llu frames, and not one of them "
            "reached the bridge's own frame path -- so nothing else in this log accounts "
            "for that time. It is spent on ReShade's present thread.",
            1000.0 * double(g_bridge.ofa_ticks) / double(qpf.QuadPart)
                   / double(g_bridge.ofa_frames),
            g_bridge.ofa_frames);
        g_bridge.ofa_ticks  = 0;
        g_bridge.ofa_frames = 0;
    }

    return delivered;
}

// The D3D11 game's own device and its own back buffer, resolved off the runtime.
// Everything this does beyond that is in OfaFrameOn.
static bool OfaFrame(reshade::api::effect_runtime *rt, reshade::api::command_list *cmd,
                     reshade::api::resource_view rtv, UINT w, UINT h, SynthBound *out)
{
    SynthBound blank = {};
    *out = blank;

    auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(
        static_cast<uintptr_t>(cmd->get_native()));
    if (ctx == nullptr) return false;

    SynthBound back = {};
    SynthResolveView(rt->get_device(), static_cast<uint32_t>(reshade::api::device_api::d3d11),
                     rtv, &back);
    if (back.tex11 == nullptr) return false;
    if (back.w != w || back.h != h) { back.tex11->Release(); return false; }

    ID3D11Device *dev11 = nullptr;
    ctx->GetDevice(&dev11);
    if (dev11 == nullptr) { back.tex11->Release(); return false; }

    const bool delivered = OfaFrameOn(dev11, ctx, back.tex11, back.fmt, w, h, out);

    dev11->Release();
    back.tex11->Release();
    return delivered;
}

// ---------------------------------------------------------------------------
// The driver's optical flow engine on the VULKAN transport
//
// WHAT THE AUDIT ASKED FOR AND WHAT THIS IS. The audit chose NVOFA -- the
// driver's own optical flow engine -- as the motion vector source for the
// synthetic Vulkan path, in place of a third-party ReShade shader whose
// estimator smeared fine detail along the direction of movement in Skyrim on
// 2026-08-30. That choice is what is implemented here. What is NOT implemented
// is the route the audit picked to reach it: a second NVOFA session on the
// private D3D12 device, at API 0x30, with the two-argument constructor, the
// four-argument register, a zeroed 192-byte init block, mandatory fence points
// on both execute structs, two new D3D12 root signatures and pipeline state
// objects, a descriptor heap, and a session that faults on every teardown order
// tried and therefore has to be leaked.
//
// The reason is the instruction this work was given: if you find yourself
// writing a second copy of something that already exists, the seam is in the
// wrong place. That route is a second copy of OfaOpen, OfaClose, OfaFrame,
// OfaSelfCheck and the whole status/failure/rebuild discipline around them,
// written against six function signatures recovered from one driver build on
// one GPU, none of which can be exercised on the only Vulkan title available to
// test with. It would be the largest untested block in this file.
//
// The seam this was built on turned out not to be the seam the mirror uses, and
// that is why none of it works on Vulkan. The reasoning was: a D3D12 texture on
// a SHARED heap with ALLOW_SIMULTANEOUS_ACCESS, and an ID3D11Texture2D alias of
// it opened through the NT handle -- MakeSharedPair in bridge.inc, in every
// session. MakeSharedPair does try that direction first. It has never once
// succeeded: on 2026-08-31 every D3D11 log on this machine shows all four
// textures refused with the same 0x80070057, every format, followed by
// "trying the other direction", and the mirror running on attempt B -- created
// on D3D11, opened on D3D12. The claim above was read off the function name
// rather than off its log.
//
// Attempt B is not available here: these textures are imported into the game's
// Vulkan device through the D3D12 NT handle and NGX evaluates on the D3D12
// device, so they must be D3D12-created. That is the whole story of why the
// engine runs on D3D11 and not on Vulkan, and it is stated in the README's
// known limits rather than fixed, on a final release. The four
// textures this transport already creates are exactly that: shared heap,
// simultaneous access, NT handle, because the Vulkan side imports them. So the
// colour that has just crossed from Vulkan into g_s12.tex[SLOT_COLOR] can be
// read by a D3D11 device without anything new being created, and the vectors
// can be written back into g_s12.tex[SLOT_MV] the same way.
//
// What that costs is one private D3D11 device with no swapchain, which is the
// same object the private D3D12 device already is and is safe for the same
// reason: ReShade creates a runtime on a swapchain, and there is no swapchain
// here. What it buys is that the session, the init parameters, the register
// calls, the two compute shaders, the arming, the failure counting, the
// rebuild-on-resize and the known-answer self check are the code that is
// already proven working, at the API version -- 0x20 -- whose 48-byte
// NV_OF_INIT_PARAMS layout was confirmed field by field against the driver.
//
// If a measurement ever shows the extra device or the CPU barrier costing more
// than it is worth, the audit's route (b) is the upgrade and every signature it
// needs is recorded in the specification. This is not a claim that route (b) is
// wrong; it is a claim that it is not yet earned.
//
// KNOWN CEILING, and it is real. The engine cannot open until the four shared
// textures exist, and those are created by Synth12Build, which needs the
// contract, which the arming gate has to have passed -- and the arming gate
// wants a motion vector texture. So the FIRST arming of a Vulkan session still
// needs one of the three ReShade motion-vector providers to be installed and
// bound; from the frame after that the engine supplies the vectors and no
// ReShade texture is consulted again. The audit's own route (b) places the
// engine at the top of Synth12EvaluateShared and therefore has the identical
// bootstrap, so this is a property of where the engine sits and not of how it
// is reached.
// ponytail: the first Vulkan arming needs a ReShade motion-vector provider.
// Breaking that needs the engine to own a fifth shared texture and a fifth
// Vulkan import so it can run before the contract exists -- and its vectors
// would then be one frame old, which is the defect this source replaces. Revisit
// if a Vulkan title with no motion-vector shader is ever reported.
// ---------------------------------------------------------------------------

typedef HRESULT (WINAPI *PFN_D3D11CreateDevice_)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                 const D3D_FEATURE_LEVEL *, UINT, UINT,
                                                 ID3D11Device **, D3D_FEATURE_LEVEL *,
                                                 ID3D11DeviceContext **);

struct SynthOfaVk
{
    bool tried;
    bool ready;
    bool stopped;          // a barrier that never cleared; never retried
    HMODULE lib;

    ID3D11Device        *dev;
    ID3D11DeviceContext *ctx;
    // The engine's OWN colour and motion-vector textures, created on this private
    // D3D11 device and opened on the private D3D12 one -- not aliases of the four
    // g_s12 textures, which is what this used to be and why it never worked.
    //
    // That crossing runs D3D11 -> D3D12, which is the direction this driver
    // accepts. The other one, D3D12 -> D3D11, is refused for every texture and
    // every format: the mirror's MakeSharedPair tries it first in every session
    // and falls back 16 times out of 16. Aliasing the g_s12 textures needed the
    // refused direction, and nothing made that necessary -- those four have to be
    // D3D12-created because the game's Vulkan device imports them, but the
    // engine's two are imported into nothing.
    //
    // Colour is copied in from g_s12.tex[SLOT_COLOR] on the D3D12 side each
    // frame. The vectors need no copy out: the evaluate hands NGX mv12 directly.
    ID3D11Texture2D     *color;
    ID3D11Texture2D     *mvout;
    ID3D12Resource      *color12;
    ID3D12Resource      *mv12;
    HANDLE               hcolor;
    HANDLE               hmv;
    // The only synchronisation between this private D3D11 device and the private
    // D3D12 one, and it is a CPU barrier rather than a shared fence. That is
    // enough here and nowhere else: this runs between the transport's own wait
    // for the copies -- rq->wait_idle() serialised, the park's ev_in on the
    // worker -- and the D3D12 evaluate, and the previous frame's D3D12 work was
    // waited to completion on the CPU at the end of that frame, on whichever
    // thread ran it. So at the moment this executes there is no GPU work in
    // flight on either device that names either texture. The park did not change
    // that: it moved both halves onto one worker thread, and the render thread
    // waits for that worker before a second frame can reach here. A shared fence
    // would be the upgrade the day two frames are ever in flight at once.
    //
    // The PIPELINE does not reach that day either, and it is worth saying why
    // rather than leaving a reader to check: it puts two frames in flight between
    // the GAME's queue and the worker, and never two on these private devices.
    // One worker thread services one arm at a time and Synth12EvaluateShared
    // waits its own D3D12 fence out before it returns, so the sentence above
    // still holds word for word.
    ID3D11Query         *done;

    UINT        w, h;
    DXGI_FORMAT fmt;
    UINT64      frames;
};

static SynthOfaVk g_ovk;

static ID3D12Resource *SynthOfaVkMvTexture() { return g_ovk.mv12; }
static ID3D11Device   *SynthOfaVkDevice()    { return g_ovk.dev; }

static bool SynthOfaVkIsSource() { return g_ovk.ready || g_ovk.stopped; }
static bool SynthOfaVkReady()    { return g_ovk.ready; }
static bool SynthOfaVkStopped()  { return g_ovk.stopped; }

static void SynthOfaVkClose()
{
    // The GPU first. color12 and mv12 are D3D12 resources on the private device,
    // and the last command list submitted there names them -- D3D12 keeps no
    // reference to what a submitted list names, so releasing before the fence has
    // reached that submission is a use after free. Every other teardown on this
    // device drains first; this one did not.
    Synth12WaitFence(g_s12.fence_value, 4000);

    // Then D3D12 aliases, then the NT handles, then the D3D11 textures that own
    // the memory. Releasing the owner while the other device still holds a view
    // of it is the one ordering that is wrong here.
    if (g_ovk.color12 != nullptr) { g_ovk.color12->Release(); g_ovk.color12 = nullptr; }
    if (g_ovk.mv12    != nullptr) { g_ovk.mv12->Release();    g_ovk.mv12    = nullptr; }
    if (g_ovk.hcolor  != nullptr) { CloseHandle(g_ovk.hcolor); g_ovk.hcolor = nullptr; }
    if (g_ovk.hmv     != nullptr) { CloseHandle(g_ovk.hmv);    g_ovk.hmv    = nullptr; }
    if (g_ovk.color != nullptr) { g_ovk.color->Release(); g_ovk.color = nullptr; }
    if (g_ovk.mvout != nullptr) { g_ovk.mvout->Release(); g_ovk.mvout = nullptr; }
    if (g_ovk.done  != nullptr) { g_ovk.done->Release();  g_ovk.done  = nullptr; }
    if (g_ovk.ctx   != nullptr) { g_ovk.ctx->Release();   g_ovk.ctx   = nullptr; }
    if (g_ovk.dev   != nullptr) { g_ovk.dev->Release();   g_ovk.dev   = nullptr; }
    g_ovk.ready = false;
}

// Everything this add-on put on the GAME'S VkDevice, released because that device
// is about to be destroyed. Called from ReShade's destroy_effect_runtime, which is
// the last moment the device is still valid.
//
// Not the same as SynthVkReleaseImages: that one is the per-rebuild teardown and
// leaves the session able to import again. This ends it.
//
// The Khronos validation layer reported what its absence cost, in as many words:
// "vkDestroyDevice(): VkDevice has 14 leaked objects that have not been destroyed",
// and the fourteen are this side's -- four VkImages, their memory, and the buffer
// the depth aspect travels through. The same shutdown recorded 0xC0000005 with this
// add-on last doing "running the D3D12 evaluate", which is the worker still inside
// a frame while the device under it goes away.
//
// The optical flow engine goes too: its D3D11 aliases are opened over shared handles
// the private D3D12 side owns, and left to DllMain they would be torn down against a
// device the game destroyed long before.
//
// dev is cleared LAST, and deliberately: every release above is gated on it, so a
// second call becomes a no-op rather than a use-after-free.
static void SynthVkShutdownForDevice()
{
    if (g_svk.dev == nullptr) return;

    // A breadcrumb per step. The first crash on this path was reported under one set
    // three seconds earlier, on a completely different function, because nothing here
    // updated it -- and a fault attributed to the wrong place is worse than one
    // attributed to nowhere.
    //
    // The park goes FIRST, above the idle wait: a queue stopped at a
    // vkCmdWaitEvents never goes idle, so releasing it after that wait would be
    // releasing it after a wait that never returns.
    Breadcrumb("shutdown: releasing and stopping the transport's park");
    SynthVkParkStop();
    if (g_svp.orphaned)
    {
        // The join expired and the worker is still running, so nothing below may
        // run: it waits a queue that thread is still feeding, frees the private
        // D3D11 device it is inside, releases the textures and the NGX feature its
        // evaluate names, and clears the device its own release calls through.
        // Every one of those is a use after free while it lives, and the thread
        // has no bound this side can enforce -- so the whole set is leaked, and
        // the game's queue has already been released twice by SynthVkParkStop,
        // which is the part that had to happen.
        //
        // dev is left standing on purpose: the orphan's release at the foot of its
        // loop calls through it, and vkSetEvent(NULL, NULL) is a fault. That is
        // the best available and not a safety net -- on this caller the game
        // destroys the VkDevice as soon as the handler returns, so the orphan's
        // event poll and its release are calls into a destroyed device rather
        // than into a live one. What matters is already done: SynthVkParkStop
        // released the game's queue twice before it gave up, and g_svp.orphaned
        // makes every later SynthVkParkStop -- including the one in DllMain -- a
        // no-op, so nothing sets or destroys an event on that device again.
        // ready goes false so no later frame records against it either.
        Breadcrumb("shutdown: the park's worker did not join, so nothing is released");
        g_svk.ready = false;
        return;
    }
    Breadcrumb("shutdown: waiting the game's Vulkan queue idle");
    const bool vulkan_idle = g_svk.DeviceWaitIdle != nullptr && g_svk.DeviceWaitIdle(g_svk.dev) == 0;
    if (!vulkan_idle || !Synth12Drain()) {
        g_s12.resources_retained = true;
        g_s12.ready = false;
        g_svk.ready = false;
        g_svk.dev = nullptr;
        Log("[synth] shutdown completion unproven; retaining imported images and private resources.");
        return;
    }

    // And only now the four park events, for the reason SynthVkParkStop's header
    // gives: setting an event does not retire the queued vkCmdWaitEvents that
    // names it, so destroying them above this line destroys an object a submitted
    // command buffer still refers to -- and if the driver does not treat that as
    // signalled, the wait just above never returns.
    Breadcrumb("shutdown: destroying the park's Vulkan events");
    SynthVkParkDestroyEvents();

    // The rebuild, not the bare close: it closes the engine's own session on the
    // private D3D11 device first, and it collapses g_ovk.tried to g_ovk.stopped.
    // The bare close left tried standing, so after ReShade recreated its runtime
    // -- every swapchain change in a Vulkan game -- SynthOfaVkOpen returned false
    // without a word, the gate in OnSynthBeginEffects never found a motion source
    // again, and the substitute contract did not come back for the session.
    // The transport underneath had been given its second attempt (g_svk.tried,
    // below) on 2026-09-01; the engine on top of it had not. Baldur's Gate 3 on
    // Vulkan, borderless to fullscreen, 2026-09-02.
    Breadcrumb("shutdown: closing the optical flow session");
    SynthOfaVkRebuild();
    Breadcrumb("shutdown: releasing the depth compute");
    SynthDepthCsRelease();
    Breadcrumb("shutdown: releasing the present pass");
    SynthVkPresentClose();
    Breadcrumb("shutdown: destroying the imported Vulkan images");
    SynthVkReleaseImages();
    Breadcrumb("shutdown: releasing the private D3D12 textures and feature");
    Synth12ReleaseTextures();
    Breadcrumb("shutdown: done");

    g_svk.ready = false;
    g_svk.dev   = nullptr;
    // The one-shot guard at the top of SynthVkOpen goes with the device it was
    // set for. Left standing, it made every reopen return false without a word:
    // ReShade destroys and recreates its effect runtime on any swapchain change,
    // so the first resize or mode toggle in a Vulkan game retired the transport
    // for the session -- silently, because tried short-circuits before any log
    // line. Measured in ngxGym on 2026-09-01.
    //
    // It stays one-way WITHIN a device: a transport that refused once is not
    // retried every frame. This only says a new device gets a new attempt.
    g_svk.tried = false;
    // And the park's own two latches with it, on this caller and on this caller
    // only. SynthVkParkStop leaves them standing precisely because its other
    // caller is DllMain, where clearing them lets a present thread build a new
    // worker inside a module the loader is unloading; here everything they were
    // guarding is destroyed, the whole function ran under g_bridge_cs -- the
    // section SynthVkFrame itself runs inside, so no render thread has been in it
    // -- and a device that comes back is entitled to a new attempt.
    g_svp.stopping = 0;
    g_svp.retired  = false;
}

// Torn down and allowed to open again, which is what a resolution change needs:
// the two textures this reads and writes are ALIASES of g_s12.tex[SLOT_COLOR]
// and [SLOT_MV], and Synth12Build releases those and creates new ones. An alias
// of a released D3D12 resource is a use after free that neither API reports.
// stopped is not cleared: a session that gave up gave up.
static void SynthOfaVkRebuild()
{
    if (!g_ovk.tried) return;
    // The optical flow session's own textures live on the device this is about
    // to release, so it goes first.
    if (g_ofa.dev != nullptr && g_ofa.dev == g_ovk.dev) OfaClose();
    SynthOfaVkClose();
    g_ovk.tried = g_ovk.stopped;
}

// One shot. A failure is latched and never retried, and every branch names its
// own reason -- a session that falls back to a ReShade provider has to be able
// to say why, because that estimator is the one that smears.
// The engine's own shared texture, created on the private D3D11 device and opened
// on the private D3D12 one. D3D11 -> D3D12: the direction this driver accepts.
//
// Transcribed from MakeSharedPair's attempt B in bridge.inc rather than called,
// deliberately. That function writes g_bridge.tex11[i], tex12[i] and shared[i] by
// slot index and is load-bearing for nine shipped titles; borrowing it for a
// fifth and sixth texture would put this file's failures into the mirror's state.
// Thirty lines is cheaper than that coupling.
//
// Measured on this machine 2026-08-31: R11G11B10_FLOAT, R16G16_FLOAT,
// B8G8R8A8_UNORM and R16G16_SINT all cross with and without the UAV bind and
// arrive carrying ALLOW_SIMULTANEOUS_ACCESS (flags 0x24), which is what makes the
// COMMON -> X -> COMMON barrier pairs around the copy correct. R32G32_FLOAT is
// refused at CreateTexture2D -- the kernel's shared-format list is narrower than
// DXGI's, the same note bridge.inc already carries.
static bool SynthOfaShare(UINT w, UINT h, DXGI_FORMAT fmt, bool uav,
                          ID3D11Texture2D **out11, ID3D12Resource **out12,
                          HANDLE *out_handle, const char *what)
{
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

    HRESULT hr = g_ovk.dev->CreateTexture2D(&td, nullptr, out11);
    if (FAILED(hr))
    {
        Log("[synth] optical flow: the engine's %s texture could not be created on the "
            "private D3D11 device, 0x%08X %s %ux%u.", what, hr, FormatName(fmt), w, h);
        return false;
    }

    IDXGIResource1 *dxgi_res = nullptr;
    hr = (*out11)->QueryInterface(__uuidof(IDXGIResource1),
                                  reinterpret_cast<void **>(&dxgi_res));
    if (SUCCEEDED(hr))
    {
        hr = dxgi_res->CreateSharedHandle(nullptr,
                                          DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                          nullptr, out_handle);
        dxgi_res->Release();
    }
    if (SUCCEEDED(hr))
        hr = g_s12.dev->OpenSharedHandle(*out_handle, __uuidof(ID3D12Resource),
                                         reinterpret_cast<void **>(out12));
    if (FAILED(hr))
    {
        Log("[synth] optical flow: the engine's %s texture was created but could not be "
            "opened on the private D3D12 device, 0x%08X %s.", what, hr, FormatName(fmt));
        if (*out11 != nullptr) { (*out11)->Release(); *out11 = nullptr; }
        if (*out_handle != nullptr) { CloseHandle(*out_handle); *out_handle = nullptr; }
        return false;
    }
    return true;
}

static bool SynthOfaVkOpen(UINT w, UINT h, DXGI_FORMAT back_fmt)
{
    if (g_ovk.tried) return g_ovk.ready;
    g_ovk.tried = true;
    g_ovk.w = w; g_ovk.h = h;
    // The TYPED spelling, because that is the format g_s12.tex[SLOT_COLOR] was
    // actually created with and therefore the format the alias has. Handing
    // OfaFrameOn anything else makes its own rebuild test fire on every frame.
    g_ovk.fmt = TypedFormat(back_fmt);

    if (g_cfg.ofa_grid == 0)
    {
        Log("[synth] optical flow: ofa_grid=0 in dlss5-bridge.cfg, so the driver's "
            "optical flow engine is not used on this transport either and the motion "
            "vectors come from a ReShade effect.");
        return false;
    }
    if (g_s12.dev == nullptr)
    {
        Log("[synth] optical flow: the private D3D12 device is not open, so there is "
            "nothing to open the engine's own textures on.");
        return false;
    }

    // Loaded, never linked, for the same reason vulkan-1.dll and d3d12.dll are:
    // this add-on must still load in a process with none of them.
    //
    // ONCE. This ran on every open, and SynthOfaVkClose frees no library, so the
    // module's reference count grew by one per rebuild -- and the engine rebuilds
    // on a resolution change, a grid change and a display-mode change, which is
    // several times in an ordinary session. It is never freed on purpose: the
    // private D3D11 device created from it may still be alive when the last
    // session closes, and unloading d3d11.dll under a live device is worse than
    // holding one reference for the life of the process.
    if (g_ovk.lib == nullptr) g_ovk.lib = LoadLibraryW(L"d3d11.dll");
    auto create = g_ovk.lib != nullptr
        ? reinterpret_cast<PFN_D3D11CreateDevice_>(GetProcAddress(g_ovk.lib, "D3D11CreateDevice"))
        : nullptr;
    if (create == nullptr)
    {
        Log("[synth] optical flow: d3d11.dll is not present or exports no D3D11CreateDevice, "
            "so the engine cannot be given a device to run on. The motion vectors come from "
            "a ReShade effect.");
        return false;
    }

    // ponytail: the default adapter, exactly as the private D3D12 device above
    // takes it and for the same reason -- ReShade exposes no VkPhysicalDevice,
    // so there is no LUID to match. Both private devices therefore land on the
    // same adapter as each other, which is what the shared handle below needs;
    // whether that is the adapter the game renders on is the question the
    // Vulkan memory import already answers by failing.
    const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, want, 1,
                        D3D11_SDK_VERSION, &g_ovk.dev, nullptr, &g_ovk.ctx);
    if (FAILED(hr) || g_ovk.dev == nullptr || g_ovk.ctx == nullptr)
    {
        Log("[synth] optical flow: D3D11CreateDevice failed 0x%08X, so there is no device "
            "to open an optical flow session on.", hr);
        SynthOfaVkClose();
        return false;
    }

    // The engine's own two textures, created here and opened on the private D3D12
    // device -- see SynthOfaShare for why this direction and not the other.
    //
    // The colour takes the same typed format the transport's own colour texture
    // has, so the per-frame CopyResource is a straight copy with no conversion.
    // The vectors are R16G16_FLOAT, which is what the conversion writes and what
    // NGX reads, and they need the UAV bind because the conversion shader writes
    // them through an unordered access view.
    if (!SynthOfaShare(w, h, g_ovk.fmt, /*uav=*/false,
                       &g_ovk.color, &g_ovk.color12, &g_ovk.hcolor, "colour") ||
        !SynthOfaShare(w, h, DXGI_FORMAT_R16G16_FLOAT, /*uav=*/true,
                       &g_ovk.mvout, &g_ovk.mv12, &g_ovk.hmv, "motion vector"))
    {
        SynthOfaVkClose();
        return false;
    }

    D3D11_QUERY_DESC qd = {};
    qd.Query = D3D11_QUERY_EVENT;
    if (FAILED(g_ovk.dev->CreateQuery(&qd, &g_ovk.done)) || g_ovk.done == nullptr)
    {
        Log("[synth] optical flow: the private D3D11 device would not create an event "
            "query, and without one there is nothing to wait on before the D3D12 evaluate "
            "reads what this wrote.");
        SynthOfaVkClose();
        return false;
    }

    // The session itself: the same OfaOpen the D3D11 path uses, at the same API
    // version, with the same 48-byte NV_OF_INIT_PARAMS whose layout was
    // confirmed against the driver field by field.
    if (!OfaOpen(g_ovk.dev, g_ovk.ctx, w, h, g_ovk.fmt))
    {
        Log("[synth] optical flow: the session did not open on the private D3D11 device -- "
            "the reason is the line above this one. The motion vectors come from a ReShade "
            "effect, and that estimator is what produced the smearing this source was added "
            "to fix, so a report of melting detail from here is expected rather than new.");
        SynthOfaVkClose();
        return false;
    }

    g_ovk.ready = true;
    Log("[synth] optical flow: running on a private D3D11 device that shares this "
        "transport's own colour and motion vector textures with the private D3D12 one. "
        "Nothing new is allocated for it: those two textures are already on a shared heap "
        "with simultaneous access because the Vulkan side imports them. From the next "
        "frame the vectors come from the driver and no ReShade motion-vector texture is "
        "read again.");
    return true;
}

// One frame, run after the copies in have executed and before the D3D12
// evaluate: on the present thread between rq->wait_idle() and the evaluate when
// vk_sync=1, and on the park's worker thread between ev_in and the evaluate
// otherwise. Returns true when g_s12.tex[SLOT_MV] now holds this frame's vectors.
static bool SynthOfaVkFrame(DWORD wait_ms)
{
    if (!g_ovk.ready) return false;

    // Fill the engine's own colour texture from the transport's, on the D3D12
    // side, and WAIT for it on the CPU before the D3D11 read below.
    //
    // This did not exist while g_ovk.color was an alias of the same memory. Now
    // that the engine owns its texture, the copy is the only thing that makes it
    // this frame's colour -- and the wait is the only thing that orders it before
    // the engine reads it. Skipping the wait is the one failure this transport
    // cannot report: the flow would be computed against a stale or torn frame,
    // giving subtly wrong vectors and ghosting, with nothing in the log and
    // OfaSelfCheck still passing, because that checks the conversion against the
    // flow texture the driver filled and not whether the colour was current.
    //
    // The barriers are COMMON -> COPY -> COMMON on both sides. Correct by the
    // resources' own declared flags: both carry ALLOW_SIMULTANEOUS_ACCESS, the
    // transport's because Synth12MakeTexture sets it and the engine's because it
    // survives the D3D11 -> D3D12 crossing, measured as flags 0x24.
    if (g_ovk.color12 != nullptr && g_s12.tex[SLOT_COLOR] != nullptr)
    {
        if (!Synth12Begin(wait_ms))
        {
            Log("[synth] optical flow: no command list to copy this frame's colour into "
                "the engine's texture, so nothing is delivered this frame.");
            return false;
        }
        D3D12_RESOURCE_BARRIER rb[2] = {};
        for (int i = 0; i < 2; ++i)
        {
            rb[i].Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            rb[i].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            rb[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        rb[0].Transition.pResource   = g_s12.tex[SLOT_COLOR];
        rb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        rb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        rb[1].Transition.pResource   = g_ovk.color12;
        rb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        rb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        g_s12.list->ResourceBarrier(2, rb);

        g_s12.list->CopyResource(g_ovk.color12, g_s12.tex[SLOT_COLOR]);

        for (int i = 0; i < 2; ++i)
        {
            const D3D12_RESOURCE_STATES was = rb[i].Transition.StateAfter;
            rb[i].Transition.StateBefore = was;
            rb[i].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
        }
        g_s12.list->ResourceBarrier(2, rb);

        const UINT64 v = Synth12End();
        {
            // The caller's budget, for the same reason Synth12EvaluateShared takes
            // one: who is waiting decides what a safe wait is. On the serialised
            // transport this runs on the present thread with nothing behind it and
            // four seconds is a generous ceiling; under the park the game's queue
            // is stopped at a vkCmdWaitEvents until this returns, and four seconds
            // there is twice the Windows TDR window.
            if (!Synth12WaitFence(v, wait_ms))
            {
                Log("[synth] optical flow: the colour copy into the engine's texture did "
                    "not complete within %lu ms, so the engine is not run on it. Nothing is "
                    "delivered this frame rather than flow computed on a stale frame.",
                    wait_ms);
                return false;
            }
        }
    }

    SynthBound mv = {};
    if (!OfaFrameOn(g_ovk.dev, g_ovk.ctx, g_ovk.color, g_ovk.fmt, g_ovk.w, g_ovk.h, &mv))
    {
        // Not delivered: the first frame has no previous one, or the execute was
        // refused. Either way the shared texture is left holding what the last
        // successful frame put there rather than being filled from somewhere
        // else -- swapping estimator under a running feature is the defect this
        // whole source exists to avoid -- and the caller does not deliver.
        if (!g_ofa.ready && !g_ovk.stopped)
        {
            g_ovk.stopped = true;
            Log("[synth] optical flow: the session on the private D3D11 device has stopped, "
                "so this transport stops delivering rather than change estimator underneath "
                "a feature whose temporal history was built from these vectors.");
        }
        return false;
    }

    g_ovk.ctx->CopyResource(g_ovk.mvout, g_ofa.mv);

    // The CPU barrier. See the note on the query in the struct: this is the only
    // thing ordering the two private devices, and it is enough because both
    // halves of this transport are already serialised on this thread.
    g_ovk.ctx->End(g_ovk.done);
    g_ovk.ctx->Flush();
    BOOL fin = FALSE;
    const ULONGLONG deadline = GetTickCount64() + 200;
    while (g_ovk.ctx->GetData(g_ovk.done, &fin, sizeof(fin), 0) != S_OK)
    {
        if (GetTickCount64() > deadline)
        {
            Log("[synth] optical flow: the private D3D11 device did not finish writing the "
                "motion vectors within 200 ms, so this transport stops rather than let the "
                "D3D12 evaluate read a texture that is still being written.");
            g_ovk.stopped = true;
            // Before the device goes, not after: the optical flow session's five
            // textures and its handle live on THIS device, and SynthOfaVkClose
            // nulls g_ovk.dev -- after which SynthOfaVkRebuild's "same device"
            // test can never match again and the session would be left holding
            // that device alive for the life of the process.
            if (g_ofa.dev != nullptr && g_ofa.dev == g_ovk.dev) OfaClose();
            SynthOfaVkClose();
            return false;
        }
        SwitchToThread();
    }

    const UINT64 n = ++g_ovk.frames;
    if (n == 1)
        Log("[synth] optical flow: first frame of driver-computed motion vectors written "
            "into the shared texture the D3D12 evaluate reads, %ux%u R16G16_FLOAT.",
            g_ovk.w, g_ovk.h);
    return true;
}

// Runs on every frame, above the gates that hold the stage-2 report down to one
// per change: a report is news and a frame is not.
static void SynthMaybeDeliver(reshade::api::effect_runtime *rt,
                              reshade::api::command_list *cmd,
                              reshade::api::resource_view rtv,
                              const SynthFacts *f,
                              const SynthBound *depth, const SynthBound *mv)
{
    // First, and before any test that could let this run.
    if (InterlockedCompareExchange(&g_source, 0, 0) == SRC_MIRROR) { SynthStandDown(); return; }

    // The config, on every synthetic transport rather than only on D3D11, and
    // ABOVE every test that reads it. BridgeFrameInner's call is reached from
    // the mirror detour and from the D3D11 arm at the bottom of this function,
    // so a D3D12 or Vulkan session -- and any session that has not armed yet --
    // read this file once at attach and never again. Below SynthArmRefusal this
    // call could
    // never lift the refusal that was keeping it from being reached: a source
    // set to auto in a file nothing would then read stayed unread for the rest
    // of the session. Rate limited inside; on the
    // D3D11 transport BridgeFrameInner's own call finds the gate closed and
    // does nothing.
    //
    // Under g_bridge_cs like the mirror's own call, so g_cfg keeps one writer at
    // a time. The mirror returned one line above, so this section is never taken
    // from here on the path the nine shipped titles use.
    EnterCriticalSection(&g_bridge_cs);
    CfgReload();
    LeaveCriticalSection(&g_bridge_cs);

    // The pipeline's pair state does not survive a gap in delivery, and this is
    // the one place every path routes through -- above SynthArmRefusal, above the
    // binding tests, above the shape and usage tests, and above SynthVkFrame's
    // own dozen early returns. Put on a single refusal branch it guarded one door
    // of twelve: the first frame back would find pending[prev] true, record a wait
    // on an ev_out signalled arbitrarily long ago, and copy back that pair's
    // Output -- a real frame, from before the gap. It also inflated the free_n
    // "already satisfied" counter, which is the one number the pipeline's log line
    // offers as evidence that the wait is doing anything.
    //
    // Two statements rather than one because the read is a frame later than the
    // write: armed_last says whether the frame BEFORE this one armed, so the test
    // has to happen before this frame clears it. An ordinal on the arm cannot do
    // this -- armed_n does not advance during a gap either, so the arm's own
    // ordinal still matches.
    //
    // Vulkan runtimes only. A process holding a Vulkan and a D3D12 effect runtime
    // calls this function twice per frame, and the D3D12 call must not be read as
    // a frame in which the Vulkan transport failed to arm. Two VULKAN runtimes
    // would still confuse it, and that configuration is the open residual named
    // above OnSynthDestroyRuntime rather than something this line can settle.
    if (f->device_api == static_cast<uint32_t>(reshade::api::device_api::vulkan))
    {
        if (!g_svp.armed_last) g_svp.pending[0] = g_svp.pending[1] = false;
        g_svp.armed_last = false;
    }

    const char *why = SynthArmRefusal(f, false);
    // Split, and the motion-vector half asks about the ReShade providers only.
    //
    // Row 0 of kSynthMv is the driver's own optical flow engine, which has no
    // ReShade texture behind it: the Vulkan branch that selects it leaves
    // mv->res null ON PURPOSE, because on that transport the vectors are already
    // in the shared D3D12 texture and a ReShade resource handle there would be a
    // trap rather than a value. Compounded with the depth test, this refused
    // every frame after the one that opened the engine -- so that branch has
    // shipped without ever executing. mv_which != 0 is the sound discriminator:
    // kSynthMvReShadeFirst is 1, so no ReShade provider can occupy row 0.
    if (why == nullptr && depth->res.handle == 0)
        why = "the depth binding did not resolve to a texture";
    if (why == nullptr && mv->res.handle == 0 && f->mv_which != 0)
        why = "the motion vector binding did not resolve to a texture";
    if (why != nullptr)
    {
        // The synthetic claim is given back HERE, and only for this one reason, on
        // the present thread -- which is the only writer of SRC_SYNTH and SRC_NONE.
        // Keyed on the POINTER and not on "any refusal": the depth binding really
        // does flap as generic_depth swaps its backup, and dropping the latch on
        // that would re-arm the delay test above -- the Skyrim Special Edition bug
        // its own comment describes. Compare-and-swap, so this can only ever
        // release the claim and never take it.
        if (why == kSynthContractDead)
        {
            InterlockedCompareExchange(&g_source, SRC_NONE, SRC_SYNTH);
            g_synth_delivering = false;
        }
        // SynthArmRefusal derives this sentence and nothing was saying it, so a
        // user who set synth_after and got nothing had every test in
        // SynthArmRefusal to guess between -- and a report whose verdict can
        // read "every precondition holds"
        // while a different test -- the delay, or the game's own DLSS -- is what
        // is holding it.
        //
        // Said in every session. It used to be withheld unless the user had set
        // synth_after, which made the one configuration that most needed to
        // explain itself -- the default one -- the only silent one.
        //
        // On change rather than once, because the reason moves as the session
        // does. The compare is on the pointer: every string here is a literal
        // returned from that one function, so identity is the reason and the
        // text is only how it reads.
        static const char *said;
        if (why != said)
        {
            said = why;
            Log("[synth] not delivering: %s.", why);
        }
        return;
    }

    SynthBound back = {};
    SynthResolveView(rt->get_device(), f->device_api, rtv, &back);
    if (back.res.handle == 0) return;

    // The report measured against ReShade's screenshot size; the contract is
    // built from the texture that is actually handed over. Where the two
    // disagree the motion vectors were checked against the wrong number, so the
    // shape was never really verified and there is nothing to deliver from.
    if (back.w != f->back_w || back.h != f->back_h || back.samples != 1)
    {
        if (!g_synth_shape_said)
        {
            g_synth_shape_said = true;
            Log("[synth] not delivering: the render target ReShade supplies is %ux%u "
                "with %u sample%s, and the back buffer everything else was measured "
                "against is %ux%u. The motion vectors were checked against the second "
                "of those, so their shape is unverified.",
                back.w, back.h, back.samples,
                back.samples == 1 ? "" : "s", f->back_w, f->back_h);
        }
        if (back.tex11 != nullptr) back.tex11->Release();
        return;
    }

    // The same question SynthArmRefusal asks of depth and motion vectors, asked
    // of the render target here because this is where it is first resolved. The
    // Vulkan transport copies out of it and back into it, and both directions
    // need the corresponding usage on the image ReShade created.
    // Out of it by a copy, always: ReShade gives every swapchain TRANSFER_SRC.
    // Into it by a copy where the game asked for TRANSFER_DST, and by a draw
    // where it did not (#23), unless vk_present=1 pins the copy.
    if (f->device_api == static_cast<uint32_t>(reshade::api::device_api::vulkan))
    {
        const uint32_t need = static_cast<uint32_t>(reshade::api::resource_usage::copy_source) |
            (g_cfg.vk_present == 1 ? static_cast<uint32_t>(reshade::api::resource_usage::copy_dest) : 0u);
        if ((back.usage & need) != need)
        {
            static bool said;
            if (!said)
            {
                said = true;
                Log("[synth] not delivering: the render target ReShade supplies has usage "
                    "0x%08X, and this transport needs it to be a copy source%s. Nothing is "
                    "copied out of an image that was not created to be copied out of.",
                    back.usage, g_cfg.vk_present == 1 ? " and, with vk_present=1, a copy destination" : "");
            }
            if (back.tex11 != nullptr) back.tex11->Release();
            return;
        }
    }

    // Only the D3D11 transport needs a device context; the D3D12 one records
    // into a command list of its own. Resolved before the latch is taken so a
    // runtime that hands over neither refuses without claiming the source.
    auto *ctx = reinterpret_cast<ID3D11DeviceContext *>(
        static_cast<uintptr_t>(cmd->get_native()));
    const bool on_d3d11 =
        f->device_api == static_cast<uint32_t>(reshade::api::device_api::d3d11);
    const bool on_vulkan =
        f->device_api == static_cast<uint32_t>(reshade::api::device_api::vulkan);
    if (on_d3d11 && ctx == nullptr) { if (back.tex11 != nullptr) back.tex11->Release(); return; }

    // Only from SRC_NONE. The mirror is never overwritten from here; the check
    // at the top already returned, and this closes the window between them.
    if (InterlockedCompareExchange(&g_source, SRC_SYNTH, SRC_NONE) == SRC_MIRROR)
    { if (back.tex11 != nullptr) back.tex11->Release(); return; }

    const unsigned int flags = SynthCreateFlags(back.fmt, f->def_value[SYNTH_DEF_REVERSED]);
    if (!g_synth_armed_said)
    {
        g_synth_armed_said = true;
        // What it is, before what it does. A user who reaches this has switched on
        // a substitute and is entitled to know it is a substitute -- two titles
        // were reported looking wrong under it before the key existed, and both
        // reports were of the technique working as well as it can.
        Log("[synth] NOTE: this is a SUBSTITUTE DLSS contract, not the game's own. Its "
            "motion vectors come from the NVIDIA driver's optical flow and its depth "
            "from ReShade, so it is measurably worse than real DLSS: optical flow does "
            "not know what a glyph or a leaf is, so text goes soft and dense foliage "
            "smears. Red Dead Redemption 2 looks like it is melting under it; Baldur's "
            "Gate 3 holds its geometry and loses its text. That is the technique rather "
            "than a fault. Set synth=0 in dlss5-bridge.cfg to switch it off.");
        // "since this add-on started" is not always the window, and the counter is
        // not D3D11's alone -- the Vulkan create increments it too. After a source
        // latch release the question is what the game has done SINCE the release,
        // which is the case that path exists for.
        Log("[synth] arming. The configured %d s delay has passed, no NGX create and no "
            "evaluate has come from the game since %s, source=%s allows it, and every "
            "precondition in the report above holds.",
            SynthAfterSec(),
            InterlockedCompareExchange(&g_release_n, 0, 0) != 0
                ? "the source latch was released" : "this add-on started",
            kCfgSourceName[g_cfg.source]);
        // Which transport, read off the device rather than off the build. Both
        // deliver the same contract; they differ in how many devices the frame
        // touches, which is the first thing a report needs to say.
        Log("[synth]   transport: %s.", on_d3d11
            ? "D3D11 -- the frame is copied onto this add-on's own D3D12 device and the "
              "evaluate runs there"
            : on_vulkan
                ? "Vulkan -- four textures created on this add-on's own D3D12 device are "
                  "imported into the game's VkDevice, the frame is copied into them there, "
                  "and the evaluate runs on the D3D12 side"
                : "D3D12 -- the evaluate runs on the game's own device, over the game's own "
                  "depth and motion vectors, with no copy between devices");
        Log("[synth]   contract: %ux%u -> %ux%u, preset 5 (DLAA), create flags %u, "
            "MV.Scale %.1f,%.1f, jitter 0,0.",
            back.w, back.h, back.w, back.h, flags,
            static_cast<double>(g_cfg.mv_sign_x != 0 ? g_cfg.mv_sign_x
                                : (f->mv_which >= 0 && kSynthMv[f->mv_which].sign_x != 0
                                       ? kSynthMv[f->mv_which].sign_x : 1)) * back.w,
            static_cast<double>(g_cfg.mv_sign_y != 0 ? g_cfg.mv_sign_y
                                : (f->mv_which >= 0 && kSynthMv[f->mv_which].sign_y != 0
                                       ? kSynthMv[f->mv_which].sign_y : 1)) * back.h);
        // Both sizes on one line, once per session, so a field report shows the
        // flow field AND the texture NGX was handed and proves the upsample
        // happened rather than being assumed. That is the ambiguity -- "the
        // transport is right and the flow is bad" against "the transport is
        // wrong" -- which cost this project the vort_Shaders diagnosis.
        // g_ofa.grid != 0 as well as mv_which == 0, and the second test is not
        // redundant any more. It used to be: mv_which 0 meant the engine had
        // already opened, because that was the only way to select row 0. The
        // Vulkan arming gate now selects it on the PROMISE of the engine -- so
        // this can run before OfaOpen has set grid, and the two divisions below
        // crashed the game with an integer divide by zero on the first session
        // that armed without a ReShade shader, 2026-08-31.
        if (f->mv_which == 0 && g_ofa.grid != 0)
        {
            Log("[synth]   motion vectors: NVIDIA optical flow, grid %u, perf %s. Flow "
                "%ux%u R16G16_SINT -> NGX %ux%u R16G16_FLOAT, scaled by 1/32 into UV.",
                g_ofa.grid, SynthPerfName(g_ofa.perf),
                (g_ofa.w + g_ofa.grid - 1) / g_ofa.grid,
                (g_ofa.h + g_ofa.grid - 1) / g_ofa.grid, g_ofa.w, g_ofa.h);
            Log("[synth]   direction: %+d,%+d, measured on this machine and not a "
                "provider's claim.",
                g_cfg.mv_sign_x != 0 ? g_cfg.mv_sign_x : kSynthMv[0].sign_x,
                g_cfg.mv_sign_y != 0 ? g_cfg.mv_sign_y : kSynthMv[0].sign_y);
        }
        else if (f->mv_which == 0)
        {
            Log("[synth]   motion vectors: the driver's optical flow engine, which has not "
                "opened yet -- this session armed on the expectation of it. The flow field "
                "sizes are reported by the line that opens it, on one of the next frames.");
        }
        Log("[synth]   IsHDR is %s because the back buffer is %s%s; DepthInverted is %s "
            "because RESHADE_DEPTH_INPUT_IS_REVERSED is %d.",
            (flags & 0x01) ? "set" : "clear", FormatName(back.fmt),
            PqBackBuffer(back.fmt)
                ? " presented as HDR10 (PQ), which is decoded to linear BT.709 for the "
                  "evaluate and encoded again after"
                : "",
            (flags & 0x08) ? "set" : "clear", f->def_value[SYNTH_DEF_REVERSED]);

        // A known defect, stated rather than hidden, because it is the first
        // thing that would be blamed on the sign knob. This runs at
        // reshade_begin_effects, which fires BEFORE any technique; the provider
        // writes its texture from inside its own technique. So the vectors read
        // here were computed during the previous frame's effect chain, and DLSS
        // is handed this frame's colour against last frame's motion.
        //
        // Not fixed by moving to reshade_finish_effects, which is why it is
        // still here: all three transports read the resource states that hold at
        // begin_effects -- back buffer in render_target, depth and motion
        // vectors in shader_resource, the last of those left there by
        // generic_depth's own begin_effects handler. Those are not the states
        // after the chain has run, and on D3D12 a barrier from a state a
        // resource is not in is a device removal rather than a warning. The move
        // needs the states re-derived for the new point, not a changed enum.
        if (f->mv_which == 0)
            // Not one frame old on this provider, and the difference is worth
            // stating rather than leaving a reader to infer it from the note
            // below being absent: the flow is computed here, at this same point
            // in this same frame, from this frame's back buffer against the one
            // captured at the same point last frame. Colour and motion are the
            // same two frames.
            Log("[synth]   note: the motion vectors are computed here, from this frame's "
                "back buffer against the previous frame's, so they are not one frame "
                "behind the colour the way a ReShade effect's are.");
        else
            Log("[synth]   note: the motion vectors are one frame old. This delivers at "
                "reshade_begin_effects, before any technique runs, and the provider writes its "
                "texture from inside its own technique.");
    }
    g_synth_delivering = true;

    SynthParams sp;
    SynthBuild(&sp, back.w, back.h, back.fmt, back.res, depth->res, mv->res,
               f->def_value[SYNTH_DEF_REVERSED], f->mv_which);
    // mv_which 0 is the driver's optical flow engine, whose vectors reach NGX
    // through the shared D3D12 texture rather than through this contract.
    SynthSelfCheck(&sp, f->mv_which == 0);

    // The same two steps the game's evaluate detour takes, in the same order and
    // under the same section: opening the session and running the frame are one
    // unit, or a source that took the lock between them finds session_ready true
    // and every texture still null.
    EnterCriticalSection(&g_bridge_cs);
    // Asked again here, and this is the line that makes the latch a latch. The
    // compare above it is outside the section, and everything between the two --
    // the arming log with its file write, the build, the self-check -- is long
    // enough for the game's first DLSS call to land in the middle of it. The
    // evaluate detour publishes SRC_MIRROR before it takes this section and does
    // its undo inside it, so inside the section there are only two states and
    // both are correct: either the detour has not arrived, and it will find
    // prev_source SRC_SYNTH and throw away whatever this frame builds, or it has
    // already run, and this frame would evaluate the game's own freshly built
    // feature under a contract describing somebody else's textures -- delivering
    // one wrong image into the back buffer and a temporal history to match.
    // Left delivering rather than stood down here: the compare at the top is
    // what says so, on the next frame, with the log line that goes with it.
    if (InterlockedCompareExchange(&g_source, 0, 0) == SRC_MIRROR)
    {
        LeaveCriticalSection(&g_bridge_cs);
        if (back.tex11 != nullptr) back.tex11->Release();
        return;
    }
    // The seam is exactly here and nowhere else: everything above this line is
    // the same contract, the same self-check, the same latch and the same
    // report, and everything below is the transport that carries it.
    if (on_d3d11)
    {
        if (g_cfg.stage >= 1 && InterlockedCompareExchange(&g_probe_done, 1, 0) == 0)
        {
            ID3D11Device *dev = nullptr;
            ctx->GetDevice(&dev);
            if (dev != nullptr) { BridgeInitSession(dev, ctx); dev->Release(); }
        }
        BridgeFrame(ctx, &sp);
    }
    else if (on_vulkan)
    {
        // Same rule as the D3D12 arm below: SynthArmRefusal has already refused
        // anything under stage 3, and neither of these transports has a step to
        // isolate at 1 or 2 the way the mirror's cross-device copies do.
        SynthVkFrame(rt, cmd, &sp, back.res, rtv, back.usage, depth->res, mv->res,
                     back.w, back.h, back.fmt);
    }
    else if (g_cfg.stage >= 3)
    {
        // stage is the same switch it is on the mirror, and the D3D12 transport
        // has nothing below 3 to do: there are no cross-device copies to run on
        // their own, so 1 and 2 have nothing to isolate here.
        Synth12Frame(rt, &sp,
                     reinterpret_cast<ID3D12Resource *>(static_cast<uintptr_t>(back.res.handle)),
                     reinterpret_cast<ID3D12Resource *>(static_cast<uintptr_t>(depth->res.handle)),
                     reinterpret_cast<ID3D12Resource *>(static_cast<uintptr_t>(mv->res.handle)),
                     back.w, back.h, back.fmt);
    }
    LeaveCriticalSection(&g_bridge_cs);

    ++g_synth_frames;
    if (back.tex11 != nullptr) back.tex11->Release();
}

// ---------------------------------------------------------------------------
// The three callbacks
// ---------------------------------------------------------------------------

static uint64_t g_synth_signature;
static bool     g_synth_reported;
static ULONGLONG g_synth_last_probe;
static ULONGLONG g_synth_last_print;
// SynthVerdictKey of the last report actually printed.
//
// This was SynthArmRefusal's return pointer, which was wrong twice over and is
// recorded here because the mistake is an easy one to make again. That function
// short-circuits on configuration before it looks at a single measured fact, so
// under a configuration that refuses it returns the same literal all session and
// the comparison was constant-true -- an unconditional thirty-second gag on a
// report that is supposed to speak the moment the story changes. And it is not
// the verdict anyway: SynthReport computes its own from depth_ok/mv_ok/aside and
// never calls it, so the two could diverge completely.
static uint32_t g_synth_last_verdict;

// The last measurement, kept so the status panel can put it back through
// SynthArmRefusal rather than carry a second copy of those tests that would
// drift from this one. Published from the frame path and read from ReShade's UI
// thread with no lock: SynthFacts is all plain integers and holds no pointer, so
// the worst a reader can see is a mix of two frames' numbers -- which for a
// panel is one frame of a stale digit, and never a pointer to anything.
static SynthFacts g_synth_panel_facts;

// stage=0 has to remain an off switch that stops everything, and this file is
// new enough to be the thing that quietly broke it. Nothing here creates a
// device or opens a session, so the only cost to guard is the work itself --
// guarded at the top of every entry point rather than at registration alone, so
// editing the config mid-session still turns it off.
static bool SynthEnabled()
{
    return g_cfg.stage != 0 && g_cfg.source != CFG_SRC_OFF;
}

static void SynthForget()
{
    g_synth_reported     = false;
    g_synth_signature    = 0;
    g_synth_last_probe   = 0;
    g_synth_last_print   = 0;
    g_synth_last_verdict = 0;
}

// The colour space the game presents in, as ReShade reads it off the swapchain
// it just created or resized. Kept for the feature-build line and the panel.
struct PresentSwapchainContract {
    reshade::api::device* device;
    void* window;
    uint64_t first_buffer;
    reshade::api::color_space space;
};
static std::mutex g_present_swapchain_mutex;
static PresentSwapchainContract g_present_swapchain = {};

static void OnSynthInitSwapchain(reshade::api::swapchain *sc, bool)
{
    if (sc == nullptr) return;
    const auto space = sc->get_color_space();
    InterlockedExchange(&g_present_space, static_cast<LONG>(space));
    if (PresentationAdapterEnabled() && sc->get_device()->get_api() == reshade::api::device_api::vulkan) {
        const PresentSwapchainContract observed = {
            sc->get_device(), sc->get_hwnd(),
            sc->get_back_buffer_count() != 0 ? sc->get_back_buffer(0).handle : 0, space
        };
        std::lock_guard<std::mutex> lock(g_present_swapchain_mutex);
        g_present_swapchain = observed;
    }
}

static void OnSynthInitEffectRuntime(reshade::api::effect_runtime *)
{
    // A new runtime has its own texture list, so whatever was reported belonged
    // to something else.
    SynthForget();
}

// The substitute's own teardown hook, and it exists because the mirror's is not
// the substitute's. destroy_effect_runtime was registered in vkmirror.inc alone,
// and VkmRegisterEvents returns before registering anything when vk_mirror=0 --
// while SynthRegisterEvents runs unconditionally, above the line that even reads
// that key. So with vk_mirror=0 a Vulkan game got the substitute, the park and
// its worker thread, and nothing at all to stop them: the first swapchain change
// -- which ReShade answers by destroying and recreating the effect runtime -- took
// the VkDevice away with the worker still polling four VkEvents on it.
//
// It defers to the mirror's handler whenever that one is registered, and the
// reason is ordering rather than tidiness: OnVkmDestroyRuntime stops the mirror's
// own worker FIRST and calls this file's teardown second, because the mirror can
// have the game's queue parked at a vkCmdWaitEvents and the vkDeviceWaitIdle
// inside SynthVkShutdownForDevice would then never return. This file registers
// earlier, so running it here as well would run it in exactly the wrong order.
// Defined in vkmirror.inc, which is included after this file. The deferral below
// is the one place that has to know WHICH runtime the mirror will act on.
bool VkmOwnsRuntime(const reshade::api::effect_runtime *rt);
// And the mirror's own stop, for the case the deferral does NOT cover -- see the
// call below. Idempotent: it returns true on a null worker and on vk_mirror=0,
// where the mirror has no worker to stop. Answers false only when its join
// expired, which means nothing may be released at all.
static bool VkmShutdown();

static void OnSynthDestroyRuntime(reshade::api::effect_runtime *rt)
{
    // Deferred on the fact the mirror's own guard tests -- the runtime it
    // latched -- and not merely on the mirror being registered. The two are the
    // same thing with one Vulkan runtime and are not with two, which
    // vkmirror.inc's header records as a real case: the mirror latches whichever
    // arrived last, this file's device came from whichever SynthVkFrame saw
    // first, and on the mismatch neither handler ran and the park kept polling
    // four events on a device that was going away. The device test below then
    // decides the rest, which is what it is there for.
    if (g_vk_mirror != 0 && VkmOwnsRuntime(rt)) return;
    if (g_svk.dev == nullptr || rt == nullptr) return;
    // This runtime and not another one. A process can hold two -- vkmirror.inc's
    // header records the two-ReShades case -- and tearing the Vulkan side down
    // because a D3D12 runtime went away would be releasing a live device's images.
    reshade::api::device *rdev = rt->get_device();
    if (rdev == nullptr ||
        reinterpret_cast<SVkDevice>(static_cast<uintptr_t>(rdev->get_native())) != g_svk.dev)
        return;

    Log("[synth] the game is destroying the effect runtime, so the substitute's Vulkan "
        "transport stops: the park is released and joined and everything imported onto "
        "that VkDevice is released before the device is.");

    // The MIRROR's worker, stopped here as well, and this call is the whole of
    // what the deferral above does not cover. Reaching this line means the mirror
    // does not own the runtime that is going away but the substitute's device came
    // from it, so the mirror's own handler will never run for it -- and the
    // teardown below releases exactly what the mirror's worker is inside: the NGX
    // feature, g_s12.tex[], the imported VkImages, the optical flow's private
    // D3D11 device. That worker holds g_ngx_cs alone; g_bridge_cs is not a section
    // it ever enters, so the lock below excludes it from nothing.
    //
    // Two lines rather than the shared idempotent teardown the note here used to
    // ask for: VkmShutdown already IS idempotent -- it returns true on a null
    // worker, which covers vk_mirror=0 and a mirror already stopped -- and calling
    // it is exactly what OnVkmDestroyRuntime does, in this position, above its own
    // Enter. Nothing recurses: VkmShutdown reaches no destroy_effect_runtime
    // handler. Nothing deadlocks: it is called with no lock held, it takes none,
    // and its join waits a worker whose only lock is g_ngx_cs, which this thread
    // does not hold. A shared function would have been one wrapper around these
    // two calls in one of the two orders they already use.
    if (!VkmShutdown())
    {
        // The gate shuts even though nothing is released; see the same two lines
        // in OnVkmDestroyRuntime for why. g_svk.ready is written nowhere else on
        // the way down, so returning here without it leaves this transport armed
        // over a device that is going away.
        g_svk.ready   = false;
        g_svp.retired = true;
        Log("[synth] the mirror's worker did not join, so nothing imported onto that "
            "VkDevice is released: the reason is the line above this one.");
        return;
    }

    // Under the same section the mirror's handler takes it under, and for the same
    // reason: SynthVkFrame runs the whole transport inside it.
    EnterCriticalSection(&g_bridge_cs);
    SynthVkShutdownForDevice();
    LeaveCriticalSection(&g_bridge_cs);
}

static void OnSynthReloadedEffects(reshade::api::effect_runtime *)
{
    // Every effect_texture_variable handle ReShade handed out before this is now
    // a pointer into a freed vector. Nothing is cached across frames here for
    // exactly that reason; this only forces the answer to be stated again,
    // because a reload is the most likely moment for it to have changed.
    SynthForget();
}

// The ReShade motion-vector providers. Lifted out of OnSynthBeginEffects
// unchanged so the driver's optical flow engine can be tried first and this can
// be the else -- the loop and its reasoning are the code that was already here.
//
// The first name that exists is the answer only if it is also bound. Marty's
// qUINT_of.fx and his current iMMERSE Launchpad are routinely installed side
// by side, and the older file declares texMotionVectors at index 0 whether
// or not its technique is on -- so stopping at the first declaration reports
// "present but NOT BOUND" over a Launchpad that is running and bound at
// index 1. A declaration is kept as the fallback so "not present at all"
// stays a distinct answer.
//
// From kSynthMvReShadeFirst: row 0 of that table is the driver's own optical
// flow engine, this add-on creates the texture it writes into, and there is no
// name to hand find_texture_variable. See the note on the table.
static void OnSynthMvWalk(reshade::api::effect_runtime *rt, reshade::api::device *dev,
                          SynthFacts *f, SynthBound *mv_b)
{
    reshade::api::resource_view mv_srv = { 0 };
    for (int i = kSynthMvReShadeFirst; i < static_cast<int>(_countof(kSynthMv)); ++i)
    {
        reshade::api::resource_view v = { 0 };
        if (!SynthLookup(rt, kSynthMv[i].name, &v)) continue;
        if (f->mv_which < 0) { f->mv_which = i; mv_srv = v; }
        if (v.handle != 0)   { f->mv_which = i; mv_srv = v; break; }
    }
    f->mv_bound = (mv_srv.handle != 0) ? 1 : 0;
    SynthResolveView(dev, f->device_api, mv_srv, mv_b);
}

// The third parameter is the render target ReShade is about to apply its effects
// to, which on a D3D11 device is a plain ID3D11RenderTargetView over the back
// buffer. That is the only colour surface this add-on can reach, so it is both
// the Color the synthetic contract reads and the Output it writes back into.
// The fourth is the same target viewed as sRGB and describes no different
// resource, so nothing here wants it.
// ---------------------------------------------------------------------------
// Releasing the source latch
//
// The mirror latch is taken by the render thread the first time the game calls
// NGX, and until now nothing ever put it back. That was right while the game's
// DLSS was a fixed property of the session and wrong the moment a user can
// toggle it in a settings menu: turning DLSS off left g_source at SRC_MIRROR
// with nothing to mirror, and the synthetic path could not re-arm because
// arming requires SRC_NONE. Both paths silent, the panel still saying Mirror.
// Reported on Baldur's Gate 3, 2026-08-31.
//
// Detection is g_eval_count standing still. It is the only signal there is: no
// *_ReleaseFeature entry point is hooked anywhere in the tree -- the Vulkan set
// is Evaluate/Evaluate_C/Create/Create1 -- and adding one means a new patch site
// in the machinery nine shipped D3D11 titles run on, for a signal that is
// ambiguous anyway, because a resolution change is a release followed at once by
// a create.
//
// This runs on the present thread only, which is already the only writer of
// SRC_SYNTH, so it is also the only writer of SRC_NONE. The render thread stays
// the only writer of SRC_MIRROR.
// ---------------------------------------------------------------------------

static void SynthReleaseLatch(ULONGLONG quiet_ms)
{
    // Before anything is written. g_s12.ready and g_svk.ready are one-way behind
    // their tried flags, and the Vulkan mirror shares both with the synthetic
    // transport -- so a transport the mirror killed is dead for the process.
    // Releasing into that hands the frame to something that cannot deliver: the
    // panel would flip to "Synthetic" and still show nothing, which is the same
    // bug one layer down. tried && !ready is the test, not !ready: a transport
    // that was never opened may still open.
    const char *dead = g_bridge.disabled               ? "D3D11 bridge"
                     : (g_s12.tried && !g_s12.ready) ? "private D3D12"
                     : (g_svk.tried && !g_svk.ready) ? "Vulkan import" : nullptr;
    if (dead != nullptr)
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            Log("[synth] the game's own DLSS has gone quiet, but the %s session failed "
                "earlier this run and is not retried, so releasing the latch would hand "
                "the frame to a transport that cannot deliver. The latch stays with the "
                "mirror and the game keeps rendering on its own.", dead);
        }
        return;
    }

    bool released = false;
    LONG n = 0;

    EnterCriticalSection(&g_bridge_cs);
    const LONG evals   = InterlockedCompareExchange(&g_eval_count, 0, 0);
    const LONG creates = InterlockedCompareExchange(&g_create_count, 0, 0);
    g_eval_base   = evals;
    g_create_base = creates;

    // The mirror's own SRC_SYNTH -> SRC_MIRROR undo, run in the other direction:
    // a zero width forces one ordered rebuild on whichever side builds next,
    // because a feature built for the game's contract is not a feature for a
    // DLAA contract over ReShade's depth even when the shapes happen to match.
    g_s12.w = 0;
    g_s12.need_reset = true;

    // And the D3D11 side, which is the same two fields the mirror's own handover
    // uses in the other direction. Without them a D3D11 game whose DLSS is
    // switched off keeps the feature the mirror built for the game's contract and
    // the synthetic path arms onto it: a feature made for one contract, fed the
    // other. Clearing frame_ready forces the rebuild; need_reset throws away a
    // temporal history accumulated from vectors the new source did not produce.
    g_bridge.frame_ready = false;
    g_bridge.need_reset  = true;

    // CAS and not an exchange, so this can only ever release the mirror.
    if (InterlockedCompareExchange(&g_source, SRC_NONE, SRC_MIRROR) == SRC_MIRROR)
    {
        // The one window there is: the increment at the top of the evaluate
        // detour happens before its latch write, so an evaluate that raced this
        // has already moved the counter. Put the latch back and say nothing --
        // both interleavings end at SRC_MIRROR, and the zeroed width costs that
        // frame the same rebuild the mirror's own handover would have cost.
        if (InterlockedCompareExchange(&g_eval_count, 0, 0) != evals)
            InterlockedExchange(&g_source, SRC_MIRROR);
        else
        {
            released = true;
            n = ++g_release_n;
        }
    }
    LeaveCriticalSection(&g_bridge_cs);

    // Outside the section: no file I/O under a lock the render thread holds
    // across GPU work.
    if (!released) return;
    if (n <= 3)
        Log("[synth] the game's own DLSS has not evaluated for %llu ms, so it is "
            "switched off rather than between frames. The source latch is released: the "
            "synthetic contract may arm again, and the game's next evaluate takes the "
            "mirror straight back.", quiet_ms);
    else if (n == 4)
        Log("[synth] the source latch has been released four times this session; further "
            "releases are not reported. The status panel shows which source is running now.");
}

#include "present-adapter.inc"

static void OnSynthBeginEffects(reshade::api::effect_runtime *rt,
                                reshade::api::command_list   *cmd,
                                reshade::api::resource_view   rtv,
                                reshade::api::resource_view)
{
    // Said at most once, and from here because this is the first place in the
    // process where every add-on has certainly finished loading. See the note on
    // the function: it warns, it does not prevent.
    WarnIfOldCopyLoaded();
    // The file, from here, whatever holds the session. With the game's DLSS off
    // nothing else runs the D3D11 frame path, so a key the panel wrote --
    // synth=1 from the switch -- stayed unread until the game's DLSS came back,
    // and the switch read as unticked one frame after every click. Rate
    // limited inside; still protected by g_bridge_cs, but this optional poll
    // must not block the frame-generation/present thread. Endfield's Vulkan
    // mirror can hold this lock while waiting for its worker, whose GPU input
    // is queued behind FG completion. ETW confirmed that blocking this callback
    // closes that wait cycle until the worker's 300 ms timeout fires.
    if (!TryReloadPresentConfig(g_bridge_cs, [] { CfgReload(); })) return;

    if (PresentationAdapterEnabled())
    {
        PresentAdapterFrame(rt, cmd, rtv);
        return;
    }

    if (!SynthEnabled() || rt == nullptr || cmd == nullptr) return;

    // The only per-frame present-thread code this add-on has, so it is the only
    // place the question can be asked: the panel draw runs only while the
    // overlay is open, and there is no timer thread by design. Everything BELOW
    // the mirror return measures the frame; this measures the game, which is the
    // one thing that can end a mirror session.
    {
        static LONG      last_evals;
        static ULONGLONG quiet_since;
        static LONG      quiet_ticks;
        const LONG       evals = InterlockedCompareExchange(&g_eval_count, 0, 0);
        const ULONGLONG  now   = GetTickCount64();

        if (evals != last_evals || quiet_since == 0)
        { last_evals = evals; quiet_since = now; quiet_ticks = 0; }
        else ++quiet_ticks;

        // Five seconds AND thirty observed presents. The wall clock alone is not
        // enough: five seconds of a game at three frames a second is a loading
        // screen, and thirty ticks means the game is really presenting before
        // the silence is believed. Five seconds is also 3.5x the worker's whole
        // bounded window (kVkmIdleMs), so by construction the worker is parked on
        // its wake event before anything here writes.
        //
        // Two tests repeat SynthArmRefusal's on purpose. Without the source one a
        // session pinned to the mirror would release, gain nothing, and pay a
        // rebuild hitch after every loading screen. If either moves, this is the
        // other copy.
        //
        // SynthOptedIn is the second, and it was deliberately absent here until
        // 2026-09-01: by the time this runs the game has demonstrably had DLSS and
        // stopped calling it, so nothing is being taken from anybody and the delay
        // that exists to decide whether a game has DLSS is already answered. That
        // reasoning is about safety and it is still correct. It is the wrong
        // question. Measured in two titles: Red Dead Redemption 2 melts under the
        // substitute contract and Baldur's Gate 3 keeps its geometry and loses its
        // text. A user who switches DLSS off in a menu has not asked to be handed
        // that instead, so now they have to.
        //
        // synth_after is still NOT repeated here: it is the delay for a game that
        // has never asked, and this path has watched one ask and stop.
        if (InterlockedCompareExchange(&g_source, 0, 0) == SRC_MIRROR &&
            now - quiet_since >= 5000 && quiet_ticks >= 30 && SynthOptedIn() &&
            (g_cfg.source == CFG_SRC_SYNTH || g_cfg.source == CFG_SRC_AUTO) &&
            g_reshade_api >= kSynthApiFloor)
        {
            // D3D11 as well as Vulkan. It was Vulkan-only when this was written,
            // and the first D3D11 user to switch DLSS off in a settings menu found
            // what that meant: the panel kept saying Mirror and nothing was
            // delivered, because arming needs SRC_NONE and nothing put it back.
            // The undo is the same two fields on either transport.
            reshade::api::device *d = rt->get_device();
            const bool releasable =
                d != nullptr &&
                (d->get_api() == reshade::api::device_api::vulkan ||
                 d->get_api() == reshade::api::device_api::d3d11);
            if (releasable)
                SynthReleaseLatch(now - quiet_since);
            quiet_since = now; quiet_ticks = 0;   // one attempt per window
        }
    }

    // While the mirror owns the session nothing this function measures can arm
    // anything -- every report during it is a true answer to a question no longer
    // being asked. The latch is not permanent any more: SynthReleaseLatch hands
    // the session back when the game's own DLSS goes quiet, and the probe resumes
    // with it, because this returns on the state rather than on a one-time flag. Baldur's Gate 3 on 2026-08-31 printed six of them in forty seconds
    // AFTER "the synthetic contract stands down for good", each one honest and
    // each one noise: the depth binding really does alternate as generic_depth
    // swaps its backup, so keying the rate limit on the verdict cannot help when
    // it is the verdict that is oscillating. The fix is not a longer floor, it
    // is not asking.
    if (InterlockedCompareExchange(&g_source, 0, 0) == SRC_MIRROR) return;

    SynthFacts f = {};
    f.api      = g_reshade_api;
    f.mv_which = -1;
    f.depth_which = -1;

    if (f.api < kSynthApiFloor)
    {
        // Said once and then never again: below the floor no effect_runtime call
        // is safe, so there is nothing further to measure and nothing that can
        // change.
        if (!g_synth_reported) { SynthReport(&f); g_synth_reported = true; }
        return;
    }

    reshade::api::device *dev = rt->get_device();
    f.device_api = (dev != nullptr) ? static_cast<uint32_t>(dev->get_api()) : 0u;
    if (!SynthApiHasBackend(f.device_api))
    {
        if (!g_synth_reported) { SynthReport(&f); g_synth_reported = true; }
        return;
    }

    rt->get_screenshot_width_and_height(&f.back_w, &f.back_h);
    SynthReadDefs(rt, &f);

    // Two name lookups per frame, three when the first motion-vector name is
    // absent or declared without a binding.
    // Each is a walk of ReShade's texture list. That is the whole per-frame cost
    // of this file during an ordinary mirror session.
    // Same shape as the motion-vector walk below and for the same reason: two
    // effects can each declare a ": DEPTH" texture while only one of them is
    // bound, so a declaration is kept only as the fallback answer.
    reshade::api::resource_view depth_srv = { 0 };
    for (int i = 0; i < static_cast<int>(_countof(kSynthDepthNames)); ++i)
    {
        reshade::api::resource_view v = { 0 };
        if (!SynthLookup(rt, kSynthDepthNames[i], &v)) continue;
        if (f.depth_which < 0) { f.depth_which = i; depth_srv = v; }
        if (v.handle != 0)     { f.depth_which = i; depth_srv = v; break; }
    }
    f.depth_found = (f.depth_which >= 0) ? 1 : 0;
    f.depth_bound = (depth_srv.handle != 0) ? 1 : 0;

    // Moved above the motion-vector walk, unchanged otherwise. The optical flow
    // pre-gate below asks SynthArmRefusal, and every test it does not skip is a
    // depth test, so the depth facts have to be filled before it is asked.
    SynthBound depth_b = {};
    SynthResolveView(dev, f.device_api, depth_srv, &depth_b);

    f.depth_w = depth_b.w; f.depth_h = depth_b.h;
    f.depth_fmt = depth_b.fmt; f.depth_samples = depth_b.samples;
    f.depth_is_dsv = depth_b.is_dsv;
    f.depth_usage  = depth_b.usage;

    // The driver's own optical flow engine first, and it is not consulted in a
    // session that would not deliver anyway: the pre-gate is every arming test
    // except the four about the motion vector texture, which is the one thing it
    // is about to produce. Without it every mirror session in the nine shipped
    // titles would pay 1.6 ms a frame building a flow field nobody reads.
    //
    // Once it is running the three ReShade names are not consulted again --
    // including on a frame whose execute failed, and including after it has
    // given up for good. Swapping the estimator under a running DLSS feature
    // would hand it a temporal history accumulated from one estimator and a new
    // frame from another, which is the same class of defect as the smear this
    // whole path was rebuilt to fix. g_ofa.stopped is what says so; ready alone
    // cannot, because it is also false before the first open.
    // (ready || !tried) is the whole of "may this be attempted". It says three
    // things at once and each one is a case: running, so keep going; never
    // attempted, so open it; attempted once and failed, so never again -- the
    // same rule Synth12Open and SynthVkOpen follow. stopped is a different
    // question and is asked below, because a first open that failed must still
    // fall back to a ReShade provider while a session that gave up must not.
    // Asked before the two branches below rather than inside either, because
    // neither of them can answer it. With a session running and ofa_grid edited
    // to 0 the pre-gate refuses on ofa_grid, the fallback refuses on ready, and
    // the result is no motion vectors from anywhere and no line saying why --
    // synthesis stops for the rest of the session on a documented off switch.
    // tried is cleared with it so setting the key back to 1, 2 or 4 opens the
    // session again: that flag is there to stop a failed open being retried
    // every frame, not to make a user's own edit one-way.
    if (g_ofa.ready && g_cfg.ofa_grid == 0)
    {
        Log("[synth] optical flow: ofa_grid=0 in dlss5-bridge.cfg, so the session is "
            "closed and the motion vectors come from a ReShade effect again.");
        OfaClose();
        g_ofa.tried = false;
    }

    SynthBound mv_b = {};
    if (f.device_api == static_cast<uint32_t>(reshade::api::device_api::d3d11) &&
        g_cfg.ofa_grid != 0 && (g_ofa.ready || !g_ofa.tried) &&
        SynthArmRefusal(&f, /*ignore_mv=*/true) == nullptr &&
        OfaFrame(rt, cmd, rtv, f.back_w, f.back_h, &mv_b))
    {
        f.mv_which = 0;
        f.mv_bound = 1;
    }
    // g_ovk.ready OR still allowed to try, which is what breaks the circle.
    //
    // The engine opens inside SynthVkFrame, and SynthVkFrame is only reached once
    // the arming gate has passed, and the gate wanted motion vectors -- which the
    // engine is what supplies. So on a machine with no ReShade motion-vector
    // shader installed, nothing ever armed and the engine never got a first
    // chance: the user had to enable a shader once purely to open the door, then
    // remove it. Measured on Baldur's Gate 3, 2026-08-31.
    //
    // Requiring only that it is ALLOWED to try lets the first frames build the
    // transport and open the engine. Those frames deliver nothing -- the first
    // execute has no previous frame to compare against, which is already this
    // path's rule -- and from the next one the vectors are real. If the engine
    // then refuses to open, SynthOfaVkIsSource stays false and SynthVkFrame
    // stands the transport down rather than evaluating over an unwritten texture;
    // that branch already existed and is what makes this safe to try.
    //
    // ofa_grid=0 is still an off switch, and a session that STOPPED is not
    // retried -- g_ovk.stopped is one-way and is what tried collapses to.
    else if (f.device_api == static_cast<uint32_t>(reshade::api::device_api::vulkan) &&
             g_cfg.ofa_grid != 0 && !g_ovk.stopped &&
             (g_ovk.ready || !g_ovk.tried) &&
             SynthArmRefusal(&f, /*ignore_mv=*/true) == nullptr)
    {
        // The Vulkan transport's own session, which opened inside SynthVkFrame
        // once the shared textures existed. Its vectors are already where the
        // evaluate reads them, so there is no ReShade texture to look up and
        // none is looked up: the walk below is not run and the three provider
        // names are not consulted again for the rest of the session.
        //
        // res stays null on purpose. On this transport the contract's
        // MotionVectors handle is never dereferenced -- SynthVkFrame skips the
        // copy and Synth12EvaluateShared sets the key to g_s12.tex[SLOT_MV]
        // itself -- and putting a D3D12 pointer into a field every other line in
        // this file reads as a ReShade Vulkan resource would be a trap rather
        // than a value.
        f.mv_which = 0;
        f.mv_bound = 1;
        mv_b.w       = f.back_w;
        mv_b.h       = f.back_h;
        mv_b.fmt     = DXGI_FORMAT_R16G16_FLOAT;
        mv_b.samples = 1;
        mv_b.usage   = 0;
    }
    else if (!g_ofa.ready && !g_ofa.stopped)
    {
        // Said once, and only after the same pre-gate the engine itself is held
        // behind, so a mirror session that never wanted synthesis never prints
        // it. Which of the three reasons applies is read off the same values the
        // branch above tested, not off a second lookup.
        static bool said;
        if (!said && SynthArmRefusal(&f, /*ignore_mv=*/true) == nullptr)
        {
            said = true;
            if (f.device_api == static_cast<uint32_t>(reshade::api::device_api::vulkan))
                Log("[synth] optical flow: this runtime is on Vulkan, where the engine runs "
                    "on a private D3D11 device that shares this transport's own textures -- "
                    "and those textures do not exist until the first frame is built, which "
                    "needs a contract, which needs this gate to pass. So the FIRST arming "
                    "still needs a ReShade motion-vector texture; from the frame after it "
                    "the driver supplies the vectors and no ReShade texture is read again.");
            else if (f.device_api != static_cast<uint32_t>(reshade::api::device_api::d3d11))
                Log("[synth] optical flow: this runtime is on %s, and the optical flow "
                    "function list this add-on recovered is the D3D11 one. The motion "
                    "vectors come from a ReShade effect.", SynthApiName(f.device_api));
            else if (g_cfg.ofa_grid == 0)
                Log("[synth] optical flow: ofa_grid=0 in dlss5-bridge.cfg, so the "
                    "driver's optical flow engine is not used and the motion vectors come "
                    "from a ReShade effect.");
            else
                Log("[synth] optical flow: the session did not open -- the reason is the "
                    "line above this one. Falling back to the motion-vector texture a "
                    "ReShade effect provides. That estimator is what produced the smearing "
                    "this path was rebuilt to fix, so a report of melting detail from here "
                    "is expected rather than new.");
        }
        OnSynthMvWalk(rt, dev, &f, &mv_b);
    }

    f.mv_w = mv_b.w; f.mv_h = mv_b.h;
    f.mv_fmt = mv_b.fmt; f.mv_samples = mv_b.samples;
    f.mv_usage = mv_b.usage;

    // Above the gates below, and that is the whole reason it sits here rather
    // than beside the report it depends on: those hold the report down to one
    // per change, and a frame is not news. Both textures are still held, and
    // neither is AddRef'd again on the way into the parameter block.
    // Published here rather than after the depth readback below, so the panel
    // and the delivery decision are looking at one measurement: this is the
    // exact struct SynthArmRefusal is about to be asked about, and the panel
    // asks the same function the same question.
    g_synth_panel_facts    = f;

    SynthMaybeDeliver(rt, cmd, rtv, &f, &depth_b, &mv_b);

    if (mv_b.tex11 != nullptr) mv_b.tex11->Release();
    ID3D11Texture2D *depth_tex = depth_b.tex11;

    // The readback stalls the GPU, so it is not part of what is measured every
    // frame. It runs only when everything cheap already differs from the last
    // report, which means a depth buffer that goes flat without any other fact
    // changing -- a cutscene on a flat backdrop, say -- is not noticed until
    // something else moves. Widening that needs a real sampling policy, not a
    // shorter interval.
    const uint64_t sig = SynthSignature(&f);
    if (g_synth_reported && sig == g_synth_signature) { if (depth_tex) depth_tex->Release(); return; }

    // A game whose depth selection flaps between candidates would otherwise
    // change the signature every frame and stall on every frame with it.
    // Two floors, not one. The five-second floor is how often the facts may be
    // re-measured; the thirty-second one is how often the SAME VERDICT may be
    // re-printed. They are different questions, and running them together
    // reprinted a twenty-line report every fifteen seconds in Baldur's Gate 3 --
    // because generic_depth swaps its backup between frames, depth_usage
    // oscillates between 0x0 and 0x4C0, and the signature covers every byte of
    // SynthFacts, so it flipped forever while the story never changed.
    //
    // The second floor is keyed on the verdict and on nothing else, so a report
    // whose meaning changed prints immediately no matter how recent the last one
    // was. That is the whole claim, and the first attempt at it did not hold it:
    // see g_synth_last_verdict.
    const ULONGLONG now = GetTickCount64();
    if (g_synth_reported && now - g_synth_last_probe < 5000)
    {
        if (depth_tex) depth_tex->Release();
        return;
    }
    g_synth_last_probe = now;

    // The readback happens before the print decision, not after it, because the
    // verdict is not known until it has. depth_read and depth_varies are what
    // separate "flat" from "ok", and a rate limit that ran first would compare
    // two verdicts that both still said "unread" and hold a real change back.
    if (depth_tex != nullptr)
    {
        SynthProbeDepth(cmd, depth_tex, &f);
        depth_tex->Release();
    }

    // Now the verdict exists, so "has anything the report SAYS changed?" is a
    // comparison. Same verdict inside thirty seconds: stay quiet. Anything else:
    // print at once, however recently the last one went out.
    const uint32_t verdict = SynthVerdictKey(&f);
    if (g_synth_reported && now - g_synth_last_print < 30000 &&
        verdict == g_synth_last_verdict)
        return;

    g_synth_last_print   = now;
    g_synth_last_verdict = verdict;

    SynthReport(&f);
    // The signature stored is the one taken before the readback, not a fresh one
    // taken after. Recomputing here folds depth_lo and depth_hi into the stored
    // value while the next frame's comparison still has them at zero, so the two
    // can never match and the report fires again every five seconds for the rest
    // of the session.
    g_synth_signature = sig;
    g_synth_reported  = true;
}

// Registration is a plain GetProcAddress from the same module the add-on
// registration came from, matching what RegisterWithReShade already does. The
// SDK's own register_addon is not used and neither is its register_event:
// ReShadeRegisterEvent resolves the owning add-on from the callback's address,
// which lands in this module, which is registered.
typedef void (*PFN_ReShadeRegisterEvent)(reshade::addon_event, void *);


static void SynthRegisterEvents()
{
    if (!SynthEnabled())
    {
        Log("[synth] not registering for ReShade events: %s.",
            g_cfg.stage == 0 ? "stage=0" : "source=off");
        return;
    }
    if (g_reshade_dll == nullptr) return;

    auto reg = reinterpret_cast<PFN_ReShadeRegisterEvent>(
        GetProcAddress(g_reshade_dll, "ReShadeRegisterEvent"));
    if (reg == nullptr)
    {
        Log("[synth] this ReShade exports no ReShadeRegisterEvent, so no feasibility "
            "report can be produced. The D3D11 mirror is unaffected.");
        return;
    }

    reg(reshade::addon_event::init_effect_runtime,    reinterpret_cast<void *>(&OnSynthInitEffectRuntime));
    // Registered here rather than only in vkmirror.inc, which registers nothing at
    // all when vk_mirror=0 -- and this file's Vulkan transport runs either way.
    reg(reshade::addon_event::destroy_effect_runtime, reinterpret_cast<void *>(&OnSynthDestroyRuntime));
    reg(reshade::addon_event::init_swapchain,         reinterpret_cast<void *>(&OnSynthInitSwapchain));
    reg(reshade::addon_event::reshade_reloaded_effects, reinterpret_cast<void *>(&OnSynthReloadedEffects));
    reg(reshade::addon_event::reshade_begin_effects,  reinterpret_cast<void *>(&OnSynthBeginEffects));


    // Both numbers, because "registered" alone does not say whether the calls
    // this file needs are the ones that will be made. ReShadeUnregisterAddon
    // drops these again at detach, so there is no unregister here.
    SynthLatchSelfCheck();
    Log("[synth] registered for ReShade effect events at add-on API %u (synth floor %u).",
        g_reshade_api, kSynthApiFloor);
}

// ---------------------------------------------------------------------------
// The status panel
//
// There are several paths now -- mirror or synthetic, D3D11 or D3D12 or Vulkan
// -- and until this existed the only way to know which one a session was on was
// to find and read the log. That made "which of these are you on" the first
// question on every support report. This answers it before anyone has to ask.
//
// Read only, and deliberately. The config file stays the single source of truth
// for every value shown; a second way to set one is a second thing that can
// disagree with the first. Every line here says what IS, and where it came from.
//
// It lives at the end of this file rather than in one of its own because
// everything it reads that is not in g_bridge is file-static here: SynthFacts,
// SynthArmRefusal, SynthApiName. The refusal sentence in particular is reused
// rather than restated -- a second copy of those tests would drift from the one
// the delivery path actually applies, and the panel would then contradict the
// log it exists to replace.
// ---------------------------------------------------------------------------

// ReShade hands its ImGui entry points to an add-on as a flat struct of function
// pointers, keyed by the ImGui version the add-on asks for. This is not the
// effect_runtime situation the note at the top of this file argues about. There
// the slot count was implicit, undocumented, and had moved between ReShade 5.9
// and 6.0, so the only defence was letting the compiler count. Here the layout
// is named by a number passed in and validated before anything comes back, the
// historical layouts are kept forever -- imgui_function_table.cpp still serves
// the 18600 table 6.0.0 shipped -- and a wrong number is a clean refusal rather
// than a jump into the wrong slot.
//
// Pinned to 19000 rather than to the newest, 19250. ReShade serves 19250 from
// every build and 19000 from every build except "Release Signed", so 19000 is
// the table that exists on ReShade 6.0.0 through 6.8.0 while 19250 exists on
// 6.7.0 and newer only. 6.0.0 is the floor the rest of this file already
// targets. A panel whose whole reason for being is to answer a support question
// must not be the first thing missing on the build the report came from.
static const uint32_t kPanelImGuiVersion = 19000;

// Six members are named and the rest is padding, because six are all this panel
// calls. Every one takes const char *, bool *, int * or int, returns void, bool
// or const char *, and touches no ImGui type -- no ImVec2, no ImVec4, no flag
// typedef -- which is what makes hand-declaring them exact rather than
// approximate. SmallButton is deliberately absent: there is no Save, no Reload
// and no Reset to defaults, because a radio click IS the save and CfgReload IS
// the reload. BeginDisabled is deliberately absent too: a control that cannot
// take effect is printed as text instead of drawn greyed, which a support report
// can be copied from and which keeps 150 slots of padding out of this struct.
//
// Each ordinal is written exactly once, in the enum, and every pad length is
// derived from it -- so pad arithmetic cannot be got wrong, only an ordinal can,
// and the offsetof asserts below cannot catch that because they compare two
// numbers derived from the same belief. The monotonicity assert is what catches
// a transposed pair, a duplicated constant, or an ordinal past the end.
//
// Ordinals counted from imgui_function_table_19000, which has 412 members. That
// file has exactly two commits in ReShade's history -- 8c49c67 (2024-01-06) and
// af334eb (2024-10-27) -- and the member list is identical across both: 412
// names in the same order, with only type spellings changed, none of them below
// ordinal 297. No widget ordinal has moved in any shipped ReShade 6.x. Re-count
// against source/imgui_function_table_19000.hpp only if this list changes.
// Truncating after the last one used is safe because this struct is only ever
// read through a pointer ReShade owns and never allocated.
enum : size_t {
    kOrdGetVersion      =   3,
    kOrdSameLine        =  85,
    kOrdTextUnformatted = 106,
    kOrdSeparatorText   = 113,
    kOrdCheckbox        = 118,
    kOrdRadioButton2    = 122,
    kOrdTableMembers    = 412,
};

struct PanelImGui
{
    const void *pad_a[kOrdGetVersion - 1];                             // 1..2
    const char *(*GetVersion)();                                       // 3
    const void *pad_b[kOrdSameLine - kOrdGetVersion - 1];              // 4..84
    void (*SameLine)(float, float);                                    // 85
    const void *pad_c[kOrdTextUnformatted - kOrdSameLine - 1];         // 86..105
    void (*TextUnformatted)(const char *, const char *);               // 106
    const void *pad_d[kOrdSeparatorText - kOrdTextUnformatted - 1];    // 107..112
    void (*SeparatorText)(const char *);                               // 113
    const void *pad_e[kOrdCheckbox - kOrdSeparatorText - 1];           // 114..117
    bool (*Checkbox)(const char *, bool *);                            // 118
    const void *pad_f[kOrdRadioButton2 - kOrdCheckbox - 1];            // 119..121
    bool (*RadioButton2)(const char *, int *, int);                    // 122
};

static_assert(offsetof(PanelImGui, GetVersion)      == (kOrdGetVersion      - 1) * sizeof(void *),
              "GetVersion is ordinal 3 of imgui_function_table_19000");
static_assert(offsetof(PanelImGui, SameLine)        == (kOrdSameLine        - 1) * sizeof(void *),
              "SameLine is ordinal 85 of imgui_function_table_19000");
static_assert(offsetof(PanelImGui, TextUnformatted) == (kOrdTextUnformatted - 1) * sizeof(void *),
              "TextUnformatted is ordinal 106 of imgui_function_table_19000");
static_assert(offsetof(PanelImGui, SeparatorText)   == (kOrdSeparatorText   - 1) * sizeof(void *),
              "SeparatorText is ordinal 113 of imgui_function_table_19000");
static_assert(offsetof(PanelImGui, Checkbox)        == (kOrdCheckbox        - 1) * sizeof(void *),
              "Checkbox is ordinal 118 of imgui_function_table_19000");
static_assert(offsetof(PanelImGui, RadioButton2)    == (kOrdRadioButton2    - 1) * sizeof(void *),
              "RadioButton2 is ordinal 122 of imgui_function_table_19000");

static_assert(kOrdGetVersion < kOrdSameLine && kOrdSameLine < kOrdTextUnformatted &&
              kOrdTextUnformatted < kOrdSeparatorText && kOrdSeparatorText < kOrdCheckbox &&
              kOrdCheckbox < kOrdRadioButton2 && kOrdRadioButton2 <= kOrdTableMembers,
              "panel ordinals must be strictly increasing and inside the 412-slot table");

// Null until a table has been obtained, and nothing registers the overlay
// without one. Every call in the draw goes through this pointer, so registering
// with it null would fault on the first widget.
static const PanelImGui *g_panel_gui;

// ReShade's table exports TextV, not Text: every formatting entry point takes a
// va_list, so there is no varargs Text to call. This is the route, not a
// workaround for one.
static void PanelLine(_Printf_format_string_ const char *fmt, ...)
{
    char b[640];
    va_list a;
    va_start(a, fmt);
    _vsnprintf_s(b, sizeof(b), _TRUNCATE, fmt, a);
    va_end(a);
    g_panel_gui->TextUnformatted(b, nullptr);
}

// ImGui derives a widget's identity from its label, so every option string has
// to be unique across the whole window -- "auto" appears in three groups. The
// ## suffix is ImGui's own way to say that: everything after it is identity and
// is not displayed.
static const char *kPanelGridNames[] = { "off##grid", "fine##grid",
                                         "normal##grid", "low##grid" };
static const int   kPanelGridValues[]= { 0, 1, 2, 4 };
static const char *kPanelPerfNames[] = { "careful##perf", "normal##perf", "fast##perf" };
static const int   kPanelPerfValues[]= { 5, 10, 20 };
static const char *kPanelMvxNames[]  = { "auto##mvx", "+1##mvx", "-1##mvx" };
static const char *kPanelMvyNames[]  = { "auto##mvy", "+1##mvy", "-1##mvy" };
static const int   kPanelMvValues[]  = { 0, 1, -1 };

// The only place SameLine and RadioButton2 are called. v is always a local
// snapshot, never &g_cfg.something: RadioButton2 writes through the pointer it
// is given, and pointing it at g_cfg would make the panel a second writer of
// the struct CfgReload owns -- a second source of truth in one line.
static bool PanelRadioRow(const char *label, int *v, const char *const *names,
                          const int *values, int n)
{
    const PanelImGui *ui = g_panel_gui;
    ui->TextUnformatted(label, nullptr);
    bool hit = false;
    for (int i = 0; i < n; ++i)
    {
        ui->SameLine(0.0f, -1.0f);          // ImGui's own defaults for both
        if (ui->RadioButton2(names[i], v, values[i])) hit = true;
    }
    return hit;
}

// One outstanding write, and it is a receipt rather than a setting: nothing in
// this add-on reads it to decide behaviour, it feeds one line of text, and it
// clears itself the moment g_cfg agrees. There is nowhere else for a second copy
// of a setting to live, because an immediate-mode radio drawn from a per-draw
// snapshot holds no state between frames.
static struct {
    const char *key;
    char        word[16];
    int         value;
    ULONGLONG   tick;
    bool        ok;
    char        why[160];
} g_panel_receipt;

// Frames put into the game's output, and when that count last moved. Cumulative
// totals cannot tell a session working now from one that stopped ten minutes
// ago, which is the first thing an "it stopped working" report needs. Display
// only; nothing reads either to decide anything.
static LONG      g_panel_last_out;
static ULONGLONG g_panel_last_out_tick;

// The diagnostics disclosure. A file-static, not a config key: it decides
// nothing but whether the last block draws, and a seventh setting in the file
// would be a second thing to keep in step for no gain.
static bool g_panel_diag;

// Formats the value itself and hands CfgSetKey a string. word is the file's own
// spelling for source=, which is a word rather than a number.
static void PanelSet(const char *key, int value, const char *word)
{
    g_panel_receipt.key   = key;
    g_panel_receipt.value = value;
    g_panel_receipt.tick  = GetTickCount64();
    g_panel_receipt.why[0] = 0;
    if (word != nullptr)
        strcpy_s(g_panel_receipt.word, sizeof(g_panel_receipt.word), word);
    else
        _snprintf_s(g_panel_receipt.word, sizeof(g_panel_receipt.word), _TRUNCATE, "%d", value);
    g_panel_receipt.ok = CfgSetKey(key, g_panel_receipt.word,
                                   g_panel_receipt.why, sizeof(g_panel_receipt.why));
}

// Three separate facts, tested in this order: the write returned, g_cfg agrees,
// and -- for the two flow settings -- the engine agrees, which the session line
// above the control already reports off g_ofa rather than off g_cfg, so the
// two-stage adoption is visible without any machinery here.
//
// g_cfg_last_read is the exact discriminator between "the validator rejected it"
// and "nothing has read the file yet": CfgReload sets it unconditionally once
// past its gate, so a value still zero seconds after a write means nothing
// called CfgReload at all. That is the honest answer for stage=0, for
// source=off, and for a paused game.
static void PanelReceipt(const char *key, int live)
{
    if (g_panel_receipt.key == nullptr || strcmp(g_panel_receipt.key, key) != 0) return;

    if (!g_panel_receipt.ok)
    {
        PanelLine("Not saved: %s.", g_panel_receipt.why);
        return;
    }
    if (live == g_panel_receipt.value) { g_panel_receipt.key = nullptr; return; }

    const ULONGLONG now = GetTickCount64();
    if (now - g_panel_receipt.tick <= 2000)
        PanelLine("Saved. Applies on the next frame.");
    else if (g_cfg_last_read == 0)
        PanelLine("Saved, not read yet: no frame has been through.");
    else
        PanelLine("%s=%s written but not adopted; the log says why.",
                  key, g_panel_receipt.word);
}

// The one line that makes the ten file-only keys safe to leave file-only: the
// escape hatches stay out of reach of a casual click, but they can no longer
// silently corrupt a bug report. Silent when nothing is off its default, because
// absence is the good state.
static void PanelNonDefault(char *out, size_t n)
{
    const struct { const char *k; int v, d; } t[] = {
        { "stage",       g_cfg.stage,       3 },
        { "mode",        g_cfg.mode,        2 },
        { "skip_game",   g_cfg.skip_game,   1 },
        { "flags",       g_cfg.flags,      -1 },
        { "subrects",    g_cfg.subrects,    1 },
        { "reset_every", g_cfg.reset_every, 0 },
        { "pixels",      g_cfg.pixels,      0 },
        { "dred",        g_cfg.dred,        1 },
        { "skip_exe",    g_cfg.skip_exe,    1 },
        { "unwrap",      g_cfg.unwrap,      1 },
        // Both halves of the substitute opt-in. It changes what is on screen more
        // than anything else in this table, so a report that omitted it was
        // omitting the first thing to ask about.
        { "synth",       g_cfg.synth,       0 },
        { "synth_after", g_cfg.synth_after, 0 },
        // And the two Vulkan transport keys, by the same criterion: vk_sync=2
        // shows the result a whole present later and carries a texture no other
        // mode has, and vk_present changes how the result reaches the back buffer
        // at all. A report from either used to read as if it were on the default.
        { "vk_sync",     g_cfg.vk_sync,     0 },
        { "vk_present",  g_cfg.vk_present,  0 },
    };
    out[0] = 0;
    for (size_t i = 0; i < _countof(t); ++i)
    {
        if (t[i].v == t[i].d) continue;
        char one[48];
        _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%s=%d",
                    out[0] != 0 ? ", " : "", t[i].k, t[i].v);
        strncat_s(out, n, one, _TRUNCATE);
    }
    if (g_cfg.source == CFG_SRC_OFF)
        strncat_s(out, n, out[0] != 0 ? ", source=off" : "source=off", _TRUNCATE);
}

// ReShade opens and closes the window around this, so nothing here may open one
// of its own; it draws widgets into the window that is already begun. It runs on
// the game's presenting thread, between ImGui::NewFrame and ImGui::Render, while
// the render thread is free to mutate everything it reads.
//
// So every value is snapshotted once, at the top, and nothing below reads a
// global twice: a panel that read g_source twice could print a mirror session's
// contract under a synthetic heading. No lock is taken -- g_bridge_cs and
// g_hook_cs are both held across GPU work by the render thread, and blocking
// present on either would turn a diagnostic into a stall -- and no resource
// pointer is dereferenced. The two pointers read at all, g_bridge.feature and
// g_s12.feature, are compared against null and never followed.
// Defined in vkmirror.inc, which is included after this file. The panel is the
// one place that has to know: g_source stays SRC_MIRROR after a stand-down, so
// without this the overlay reports a live mirror over a dead one.
bool VkmStoodDown();

static void OnPanelDraw(reshade::api::effect_runtime *rt)
{
    const PanelImGui *ui = g_panel_gui;
    if (ui == nullptr) return;

    const LONG src     = InterlockedCompareExchange(&g_source, 0, 0);
    const LONG releases = InterlockedCompareExchange(&g_release_n, 0, 0);
    const LONG evals   = InterlockedCompareExchange(&g_eval_count, 0, 0);
    const LONG out     = InterlockedCompareExchange(&g_frames_delivered, 0, 0);
    const LONG flags   = InterlockedCompareExchange(&g_panel_flags, 0, 0);

    // A whole-struct copy, so SynthArmRefusal below is asked about one frame's
    // measurement rather than about fields collected across several.
    const SynthFacts facts = g_synth_panel_facts;

    const bool   b_session  = g_bridge.session_ready;
    const bool   b_disabled = g_bridge.disabled;
    const bool   b_frame    = g_bridge.frame_ready;
    const bool   b_feature  = g_bridge.feature != nullptr;
    const UINT   b_w  = g_bridge.width,     b_h  = g_bridge.height;
    const UINT   b_ow = g_bridge.out_width, b_oh = g_bridge.out_height;
    const UINT   b_rw = g_bridge.render_w,  b_rh = g_bridge.render_h;
    const UINT   b_nw = g_bridge.ngx_out_w, b_nh = g_bridge.ngx_out_h;

    const bool   s_tried   = g_s12.tried;
    const bool   s_ready   = g_s12.ready;
    const bool   s_shared  = g_s12.shared;
    const bool   s_feature = g_s12.feature != nullptr;
    const UINT   s_w = g_s12.w, s_h = g_s12.h;
    // Read separately, because they are not the same numbers. The synthetic
    // callers pass w,h for both -- DLAA, no upscale -- so a panel written
    // against that path printed the render size twice and was right by
    // coincidence. The Vulkan mirror carries the game's own ratio, and printed
    // "2560x1067 in, 2560x1067 out" over a log line saying 2560x1067 -> 3840x1600.
    const UINT   s_ow = g_s12.out_w, s_oh = g_s12.out_h;

    // The config as it stands, and the flow engine as it is actually running.
    // All plain scalars, no pointer, exactly the argument SynthFacts already
    // makes above: the worst a reader can see is a mix of two frames' numbers.
    // The two are deliberately separate -- g_cfg is the file, g_ofa is the
    // engine, and a setting written this frame is in the first and not yet in
    // the second, which is a state the panel has to be able to show.
    const int  c_synth  = g_cfg.synth;
    const int  c_grid   = g_cfg.ofa_grid, c_perf = g_cfg.ofa_perf;
    const int  c_mvx    = g_cfg.mv_sign_x, c_mvy = g_cfg.mv_sign_y;
    const UINT o_w = g_ofa.w, o_h = g_ofa.h, o_grid = g_ofa.grid;
    // The frequency is asked of the OS rather than read from g_bridge, which sets
    // it only inside TimingTick -- the D3D11 bridge's per-frame timing, which
    // never runs on a Vulkan session. The ticks and the frame count ARE written
    // there, by OfaFrameOn, which both transports share: so on Vulkan the panel
    // said "not measured yet" over a cost the log was printing in the same
    // second. A process-constant is not something to inherit from another path's
    // start-up.
    LARGE_INTEGER f_freq = {};
    QueryPerformanceFrequency(&f_freq);
    const LONGLONG f_qpf = f_freq.QuadPart, f_ticks = g_bridge.ofa_ticks;
    const UINT64   f_frames = g_bridge.ofa_frames;

    // What the widgets are bound to. Never &g_cfg.anything: RadioButton2 and
    // Checkbox write through the pointer they are given, and the panel writes
    // the file, not the struct.
    int  w_grid = c_grid, w_perf = c_perf;
    int  w_mvx = c_mvx, w_mvy = c_mvy;

    // Whether there is a file to write at all. When the folder was never
    // writable CfgWriteDefault failed at attach, CfgReload has been returning on
    // a failed open ever since, and this session is on built-in defaults -- so
    // every control group is replaced by its text form and the build section
    // says why. One GetFileAttributesA per draw, and only while the overlay is
    // open.
    char cfg_path[MAX_PATH];
    CfgPath(cfg_path);
    const bool cfg_writable = GetFileAttributesA(cfg_path) != INVALID_FILE_ATTRIBUTES;

    // Asked of the runtime rather than taken off the last measurement, because
    // the measurement only happens when synthesis is switched on -- and the
    // backend is the one line this panel has to be able to state in a session
    // that has never enabled it. Guarded by the same API floor
    // SynthRegisterEvents checks: below that, effect_runtime's vtable is not the
    // one the vendored headers describe and get_device is a different slot.
    uint32_t dev_api = 0;
    bool     dev_api_known = false;
    if (rt != nullptr && g_reshade_api >= kSynthApiFloor)
    {
        reshade::api::device *dev = rt->get_device();
        if (dev != nullptr)
        {
            dev_api = static_cast<uint32_t>(dev->get_api());
            dev_api_known = true;
        }
    }

    // Which of the two feature blocks holds this session's numbers, and the test
    // is on the TRANSPORT rather than on the source. It used to read
    // src == SRC_SYNTH, which quietly meant "D3D11 mirror" and was true of the
    // mirror only there: the Vulkan mirror runs through Synth12Open,
    // Synth12Build and Synth12EvaluateShared and never writes a byte of
    // g_bridge, so a working mirror read the D3D11 block, found it empty, and
    // printed "No contract yet" and "D3D12 session: not opened yet" over its own
    // log saying it had built one and was delivering frames. This panel exists
    // to tell the user which situation they are in; contradicting the log is the
    // one thing it must not do.
    const bool on_s12 = dev_api_known &&
                        dev_api != static_cast<uint32_t>(reshade::api::device_api::d3d11) &&
                        (src == SRC_SYNTH || src == SRC_MIRROR);

    // Whether the driver's optical flow engine can run here at all. The function
    // list this add-on recovers from nvofapi64 is the D3D11 one, and on Vulkan the
    // synthetic path opens it on a private D3D11 device that aliases the two
    // shared textures -- see SynthOfaVkOpen -- so it runs there too, and ofa_grid
    // and ofa_perf are live controls on both. Only D3D12 is not wired.
    //
    // This read D3D11 alone, which made a Vulkan session print "the driver's
    // optical flow engine" as its motion-vector provider and then, three lines
    // later, that the engine could not run there -- while hiding the cost line
    // and both controls that were working.
    // From the struct that belongs to THIS transport. The engine has two sessions
    // -- g_ofa is the D3D11 one, g_ovk the private D3D11 device the Vulkan path
    // aliases the shared textures onto -- and this read only the first. On Vulkan
    // that made the panel print "Optical flow opens on the first synthetic frame"
    // for the rest of the session, over a g_ovk that had already tried and
    // failed, while the log said exactly what went wrong. The panel's whole job
    // is to say which situation the session is in.
    const bool on_vk_ofa = dev_api_known &&
                           dev_api == static_cast<uint32_t>(reshade::api::device_api::vulkan);
    const bool o_ready   = on_vk_ofa ? g_ovk.ready   : g_ofa.ready;
    const bool o_stopped = on_vk_ofa ? g_ovk.stopped : g_ofa.stopped;
    const bool o_tried   = on_vk_ofa ? g_ovk.tried   : g_ofa.tried;

    const bool ofa_here = dev_api_known &&
                          (dev_api == static_cast<uint32_t>(reshade::api::device_api::d3d11) ||
                           dev_api == static_cast<uint32_t>(reshade::api::device_api::vulkan));

    ui->SeparatorText("DLSS");
    if (src == SRC_MIRROR)
    {
        // g_source stays SRC_MIRROR after a stand-down and g_eval_count keeps
        // rising on the game's own evaluates, so a dead mirror must be named as
        // dead rather than reported as a live one with a growing count.
        if (VkmStoodDown())
            PanelLine("Stopped. The game's own DLSS is on screen and correct; nothing "
                      "is added any more. The log says why.");
        else
            PanelLine("Using the game's own DLSS. Full quality.");
        PanelLine("%ld frames from the game so far.", evals);
    }
    else if (src == SRC_SYNTH)
    {
        PanelLine(releases > 0
            ? "Using the built-in replacement: the game switched its own DLSS off."
            : "Using the built-in replacement: the game has no DLSS of its own.");
        PanelLine("Lower quality than a game's own DLSS. Same resolution in and out.");
    }
    else
    {
        if (releases > 0)
            PanelLine("Waiting: the game switched its DLSS off. Turning it back on "
                      "resumes at once.");
        else
            PanelLine("Waiting for the game to use DLSS.");
        // The reason, only when a replacement was asked for and is not coming.
        // With the box off the reason IS the box, and this panel used to say it
        // twice, three lines each.
        if (SynthOptedIn())
        {
            const char *why = SynthArmRefusal(&facts, false);
            const char *w = why != nullptr ? why : "refused later; the log says why";
            char brief[120];
            size_t n = 0;
            while (w[n] != 0 && w[n] != '.' && n < sizeof(brief) - 1) ++n;
            memcpy(brief, w, n);
            brief[n] = 0;
            PanelLine("No replacement yet: %s.", brief);
        }
    }

    // One switch. It used to be a three-way radio and two checkboxes that
    // described one decision three ways, and a reader could not tell what
    // happened with none of them ticked. Drawn whenever the file is writable:
    // the game's own DLSS wins from any state, so turning this on while the
    // mirror holds the session changes nothing until the game stops asking.
    if (!cfg_writable)
    {
        PanelLine("Settings cannot be saved: no dlss5-bridge.cfg beside this add-on.");
    }
    else
    {
        bool w_synth = SynthOptedIn();
        if (ui->Checkbox("Replace DLSS when the game isn't using its own", &w_synth))
            PanelSet("synth", w_synth ? 1 : 0, nullptr);
        PanelLine(w_synth
            ? "On. Lower quality than the game's own DLSS: text softens, foliage smears."
            : "Off. Only the game's own DLSS is used; nothing is added.");
        PanelReceipt("synth", c_synth);
    }

    // Never on the mirror: the game supplies its own motion. Drawn before the
    // source has latched too, because that is where a first-time user is.
    if (src != SRC_MIRROR)
    {
        ui->SeparatorText("Motion (replacement only)");

        if (src == SRC_SYNTH)
        {
            if (facts.mv_which == 0)
                PanelLine("NVIDIA optical flow. Sharp; a slight drift when almost still.");
            else if (facts.mv_which > 0 &&
                     facts.mv_which < static_cast<int>(_countof(kSynthMv)))
                PanelLine("ReShade shader %s. Fine detail smears in slow motion.",
                          kSynthMv[facts.mv_which].name);
            else
                PanelLine("No motion source found.");
        }

        // The engine as it is running, not the file: a setting written this
        // frame is in g_cfg and not yet here, and this line is what shows that.
        if (!ofa_here)
        {
            if (dev_api_known)
                PanelLine("Optical flow is not available on D3D12.");
        }
        else if (o_ready)
        {
            const UINT vx = o_grid != 0 ? (o_w + o_grid - 1) / o_grid : 0;
            const UINT vy = o_grid != 0 ? (o_h + o_grid - 1) / o_grid : 0;
            if (f_frames != 0 && f_qpf != 0)
                PanelLine("Optical flow: %ux%u vectors, %.2f ms per frame.", vx, vy,
                          1000.0 * double(f_ticks) / double(f_qpf) / double(f_frames));
            else
                PanelLine("Optical flow: %ux%u vectors.", vx, vy);
        }
        else if (c_grid == 0)
            PanelLine("Optical flow is off. Set Detail to normal to use it.");
        else if (o_stopped || o_tried)
            PanelLine("Optical flow did not start; the log says why.");
        else
            PanelLine("Optical flow starts with the first replaced frame.");

        // Radios, not sliders: every legal value is whitelisted in CfgReload and
        // a slider could select one the validator silently rejects.
        if (ofa_here && cfg_writable)
        {
            if (PanelRadioRow("Detail", &w_grid, kPanelGridNames, kPanelGridValues, 4))
                PanelSet("ofa_grid", w_grid, nullptr);
            if (PanelRadioRow("Speed ", &w_perf, kPanelPerfNames, kPanelPerfValues, 3))
                PanelSet("ofa_perf", w_perf, nullptr);
            PanelLine("The defaults suit almost everyone.");
            PanelReceipt("ofa_grid", c_grid);
            PanelReceipt("ofa_perf", c_perf);
        }

        // While the engine runs there is no control, only what it resolved to.
        if (o_ready)
            PanelLine("Direction %+d,%+d, %s.",
                      c_mvx != 0 ? c_mvx : kSynthMv[0].sign_x,
                      c_mvy != 0 ? c_mvy : kSynthMv[0].sign_y,
                      (c_mvx != 0 || c_mvy != 0) ? "set by hand" : "measured");
        else if (cfg_writable)
        {
            if (PanelRadioRow("Direction X", &w_mvx, kPanelMvxNames, kPanelMvValues, 3))
                PanelSet("mv_sign_x", w_mvx, nullptr);
            if (PanelRadioRow("Direction Y", &w_mvy, kPanelMvyNames, kPanelMvValues, 3))
                PanelSet("mv_sign_y", w_mvy, nullptr);
            PanelLine("Leave on auto. Flip an axis only if the picture smears that way.");
            PanelReceipt("mv_sign_x", c_mvx);
            PanelReceipt("mv_sign_y", c_mvy);
        }
    }

    ui->SeparatorText("Session");
    // Which block holds this session's numbers is decided by the TRANSPORT: the
    // Vulkan mirror runs through the shared D3D12 path and never writes g_bridge.
    if (!dev_api_known)
        PanelLine("Runtime unknown (add-on API %u).", g_reshade_api);
    else if (on_s12 && s_w != 0)
        PanelLine("%s. %ux%u in, %ux%u out%s.", SynthApiName(dev_api), s_w, s_h,
                  s_ow != 0 ? s_ow : s_w, s_oh != 0 ? s_oh : s_h,
                  (s_ow == s_w && s_oh == s_h) ? " (no upscale)" : "");
    else if (!on_s12 && b_frame)
        PanelLine("%s. %ux%u in, %ux%u out.", SynthApiName(dev_api), b_rw, b_rh, b_nw, b_nh);
    else
        PanelLine("%s. No DLSS yet.", SynthApiName(dev_api));
    // What is being presented and how bright the output came back: the two
    // things an HDR report needs and a screenshot can carry.
    {
        const LONG pf = InterlockedCompareExchange(&g_present_flags, 0, 0);
        const LONG ps = InterlockedCompareExchange(&g_present_space, 0, 0);
        if (pf >= 0 || ps != 0)
            PanelLine("Presentation: %s, %s%s.", ColorSpaceName(ps),
                      pf >= 0 ? ((pf & 1) ? "HDR contract" : "SDR contract") : "no contract yet",
                      g_present_expo > 0.0f ? ", exposure texture" : "");
        if (g_bright_in > 0.0f && g_bright_out > 0.0f)
            PanelLine("Brightness out/in: %.2f%s", g_bright_out / g_bright_in,
                      (g_bright_out / g_bright_in > 1.5f || g_bright_out / g_bright_in < 0.67f)
                          ? " -- the picture came back at a different brightness than it went in" : "");
    }

    {
        // Whether frames are still arriving, not only how many have.
        const ULONGLONG now_t = GetTickCount64();
        if (out != g_panel_last_out) { g_panel_last_out = out; g_panel_last_out_tick = now_t; }
        if (out == 0)
            PanelLine("No frames delivered yet.");
        else if (now_t - g_panel_last_out_tick <= 2000)
            PanelLine("%ld frames delivered, live.", out);
        else
            PanelLine("%ld frames delivered; none for %llu s.",
                      out, (now_t - g_panel_last_out_tick) / 1000);
    }

    // Silent when nothing is off its default.
    char nondefault[384];
    PanelNonDefault(nondefault, sizeof(nondefault));
    if (nondefault[0] != 0)
        PanelLine("Changed settings: %s. Picture problems may be these.", nondefault);

    // Everything a bug report needs and a running session does not.
    ui->Checkbox("Details", &g_panel_diag);
    if (g_panel_diag)
    {
        PanelLine("dlss5-bridge %s, built %s, add-on API %u.",
                  BRIDGE_VERSION, __DATE__, g_reshade_api);
        if (dev_api_known && (src == SRC_SYNTH || src == SRC_MIRROR))
            PanelLine("Transport: %s.",
                      dev_api == static_cast<uint32_t>(reshade::api::device_api::d3d11)
                          ? "D3D11 to this add-on's D3D12 device"
                          : dev_api == static_cast<uint32_t>(reshade::api::device_api::vulkan)
                              ? "Vulkan, imported D3D12 textures"
                              : "D3D12, no copy");
        if (on_s12)
            PanelLine("D3D12 session: %s, feature %s.",
                      s_ready ? (s_shared ? "open on this add-on's own device"
                                          : "open on the game's own device")
                              : s_tried ? "failed, not retried" : "not opened yet",
                      s_feature ? "created" : "none");
        else
            PanelLine("D3D12 session: %s, feature %s.",
                      b_disabled ? "failed, not retried"
                                 : b_session ? "open on this add-on's own device"
                                             : "not opened yet",
                      b_feature ? "created" : "none");
        if (!on_s12 && b_frame)
            PanelLine("Textures %ux%u in, %ux%u out.", b_w, b_h, b_ow, b_oh);
        if (flags >= 0)
        {
            char names[192];
            CreateFlagNames(names, sizeof(names), static_cast<unsigned int>(flags));
            PanelLine("Create flags %ld:%s", flags, names);
        }
        PanelLine("Log: %s", g_log_path);
        if (cfg_writable)
            PanelLine("Config: %s", cfg_path);
        if (g_panel_addons[0] != 0)
        {
            PanelLine("Add-ons in this folder:");
            ui->TextUnformatted(g_panel_addons, nullptr);
        }
        if (g_panel_dlss5[0] != 0)
        {
            PanelLine("[RenoDX.DLSS5]:");
            ui->TextUnformatted(g_panel_dlss5, nullptr);
        }
    }
}

typedef const void *(*PFN_ReShadeGetImGuiFunctionTable)(uint32_t);
typedef void (*PFN_ReShadeRegisterOverlay)(const char *,
                                           void (*)(reshade::api::effect_runtime *));

// Both exports are resolved from the same module the add-on registration came
// from, the way SynthRegisterEvents resolves ReShadeRegisterEvent. The callback
// address is what ReShade uses to work out which add-on owns the overlay, so
// this has to run after RegisterWithReShade succeeded -- which it does, from
// DllMain, beside the event registration.
//
// Nothing unregisters: ReShadeUnregisterAddon drops the overlay along with the
// events at detach.
static void PanelRegister()
{
    if (g_reshade_dll == nullptr) return;

    auto get_table = reinterpret_cast<PFN_ReShadeGetImGuiFunctionTable>(
        GetProcAddress(g_reshade_dll, "ReShadeGetImGuiFunctionTable"));
    auto reg = reinterpret_cast<PFN_ReShadeRegisterOverlay>(
        GetProcAddress(g_reshade_dll, "ReShadeRegisterOverlay"));

    // Not present, rather than not enabled: both exports live inside ReShade's
    // own GUI build switch, so a ReShade compiled without the overlay exports
    // neither and there is nothing here to fix.
    if (get_table == nullptr || reg == nullptr)
    {
        Log("[panel] this ReShade exports no %s, so it has no ImGui overlay to draw a "
            "status panel into. Everything else is unaffected.",
            get_table == nullptr ? "ReShadeGetImGuiFunctionTable" : "ReShadeRegisterOverlay");
        return;
    }

    // Refused cleanly, rather than faulted: ReShade validates the number, names
    // it in its own log, and returns nothing. Registering the overlay anyway
    // would fault on the first widget, because every call in the draw goes
    // through this pointer -- so the null check is what gates registration, and
    // not registering is the whole of the defence.
    g_panel_gui = static_cast<const PanelImGui *>(get_table(kPanelImGuiVersion));
    if (g_panel_gui == nullptr)
    {
        Log("[panel] this ReShade does not serve the ImGui %u function table, so no status "
            "panel is registered and its settings can only be changed in "
            "dlss5-bridge.cfg -- ReShade.log names the version it refused. The signed "
            "public build is the one that does this: it serves only the newest table. "
            "Everything else in this add-on is unaffected.", kPanelImGuiVersion);
        return;
    }

    // Until this panel gained controls it only read through the table. Checkbox
    // and RadioButton2 write through a bool * and an int *, so a table base that
    // is not the layout this struct describes would corrupt memory rather than
    // print nonsense. Ordinal 3 is the cheapest thing that says the base is
    // right. Not compared against a literal: the 19000 table forwards to
    // whatever ImGui ReShade was built with, so the string tracks ReShade's
    // build and not the table number -- only "it looks like a version" is
    // checkable here.
    const char *imgui_ver = g_panel_gui->GetVersion();
    if (imgui_ver == nullptr || *imgui_ver < '0' || *imgui_ver > '9')
    {
        Log("[panel] the ImGui %u function table's version entry returned %s, which is not "
            "a version string, so the table is not the layout this panel expects and no "
            "status panel is registered. Everything else in this add-on is unaffected.",
            kPanelImGuiVersion, imgui_ver != nullptr ? imgui_ver : "nothing");
        g_panel_gui = nullptr;
        return;
    }

    reg("DLSS 5 Bridge", &OnPanelDraw);
    Log("[panel] status panel registered in ReShade's overlay. Asked for the ImGui %u "
        "function table; ReShade reports its own ImGui as %s.",
        kPanelImGuiVersion, g_panel_gui->GetVersion());
}
