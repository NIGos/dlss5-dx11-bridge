# Vulkan: neural rendering of the final frame at the Frame Generation input

Status: verified in one title (Arknights: Endfield, Vulkan, 5120x2160, RTX 5090,
Generic 8.5.0-rc10 with two NR passes, native DLSS-G fixed 6x). Not yet tested
with other Vulkan games, HDR swapchains, or without native Frame Generation.

## Problem

RenoDX's DLSS 5 Generic offers three hook points: Upscaled, Render and Present.
On Vulkan the add-on itself falls back: `NRHookPoint=Present is not served on
Vulkan; NR runs at the Upscaled hook point`. The Present point is what users
want when they need the neural pass to see the finished frame (post-processing,
colour grading, HUD) rather than the DLSS output.

Two facts constrain any Vulkan implementation of that point:

1. **Where the frame is read decides how many times NR runs.** A game with
   native Frame Generation presents 6 frames per real frame at 6x. Anything that
   processes the present stream (ReShade `begin_effects`, a swapchain hook)
   pays one full NR evaluate per presented frame. At 5120x2160 with two NR
   passes one evaluate costs ~20-40 ms, so a post-FG feed collapses to ~29
   presents/s and drags the game's simulation rate down with it. This was
   measured with PresentMon before this design was adopted (see "Measurements").
2. **The private D3D12 session is only usable from a thread that may block.**
   Generic's NR runs inside `NVSDK_NGX_D3D12_EvaluateFeature` on the bridge's
   private device; the Vulkan side has to wait for that work before it can copy
   the result back. Doing that inside the FG evaluate (Streamline's present
   thread) with a device-wide wait deadlocked the game once during development,
   because the render thread's queue was waiting on present progress.

## Design

The feed hooks the game's own Frame Generation evaluate (NGX feature 11, which
the bridge already forwards) and processes **only the real frame, once**, before
the evaluate is forwarded:

```
FG evaluate (feature 11, MultiFrameIndex == 1)
  copy DLSSG.HUDLess   -> private COLOR   (shared D3D12 texture, imported VkImage)
  copy DLSSG.Backbuffer-> private OUT_B
  copy DLSSG.Depth     -> private DEPTH   (R32_SFLOAT, output resolution)
  copy DLSSG.MVecs     -> private MV      (R16G16_SFLOAT, DLSSG.MvecScaleX/Y)
  vkCmdSetEvent(in) ; vkCmdWaitEvents(out)         <- the mirror's existing park
  copy private OUTPUT  -> DLSSG.HUDLess
  copy private OUT_B   -> DLSSG.Backbuffer
forward the FG evaluate
```

The worker thread, woken by the `in` event:

```
sRGB decode COLOR -> linear FP16 working textures
NVSDK_NGX_D3D12_EvaluateFeature on the bridge's own 1:1 SR feature
   |- the SR runtime's public export is hooked for this thread only and answers
   |  with a same-size copy instead of running DLAA (identity carrier)
   |- Generic's after-upscale detour runs its NR passes on that call as usual
sRGB encode -> OUTPUT
composite: OUT_B = Backbuffer + (NR - HUDLess) * w,  w = saturate(1 - |Backbuffer - HUDLess| / 0.25)
set the out event
```

Consequences:

* NR cost is paid per **real** frame (22 evaluates/s at 5120x2160 for a 22 fps
  game), and FG still generates its frames from the processed image. Presented
  rate stayed at ~109/s (4.9x) in the game test versus 29/s for the post-FG feed.
* The UI is preserved: FG receives both the processed HUD-less image and a
  back buffer whose UI pixels are untouched, so real and interpolated frames
  agree. Translucent panels keep a faded part of the enhancement instead of a
  hard cut-out.
* Generic is used **unmodified** (8.5.0-rc10, SHA-256
  `DCD93881E976AD033D83C2BB01F4BC3E4DDC59C15FE0DD4CA165BC5FC7D1AC68`). Nothing
  reads its private layout; the identity-carrier hook is on the SR runtime's
  public `NVSDK_NGX_D3D12_EvaluateFeature`, scoped by thread and by resource
  identity, and tail-forwards every other caller unchanged.
* Depth and motion vectors at output resolution come from the FG parameter
  block, so the neural pass has real guides rather than the neutral zeros the
  present-stream feed had to use.

## Threading rules (the hard-won part)

* The FG evaluate runs on Streamline's present thread, not the render thread.
  **Never** call `vkDeviceWaitIdle`, `vkQueueWaitIdle` or any unbounded wait
  from `FgInputFrame`. The first build is allowed only when nothing has been
  imported yet; a shape change is refused (or, with `Follow=1`, deferred to the
  render thread).
* `FgInputFrame` takes `g_bridge_cs` with `TryEnterCriticalSection`; a missed
  frame keeps the game's own image, a blocked present thread can deadlock.
* Session hand-over between the native mirror and this feed (`Follow=1`) happens
  only inside the game's SR evaluate on the render thread, where the mirror
  already does its own device-idle rebuilds.
* A frame whose NR entry was not observed (Generic switched off, warming up) is
  **not** a failure: the carriers hold the game's own image and the recorded
  copy back is a no-op. Counting it as a failure retired the mirror after five
  frames and made Generic's toggle key unusable until restart.

## Colour

* UNORM8 swapchains (sRGB, the only ones handled by this build): decode to
  normalized linear FP16 for the carrier, encode back after NR. Generic then
  sees `encoding=linear units=relative`. `NRSourceEncoding`, `NRSourcePrimaries`
  and `NRLinearUnitNits` must stay at Auto (0); the adapter refuses otherwise,
  because a forced "SDR encoded" declaration over an already-linear input made
  Generic rebuild its workset every frame.
* HDR10 (PQ) swapchains use the existing PQ decode/encode pass with FP32
  intermediates and exact BT.2020/BT.709 matrices; the previous FP16/rounded
  constants lost dark channels (measured 54/255 error on a colour-block
  fixture, 0 after the change). HDR is verified only in the offline host.

## Files

| File | Role |
| --- | --- |
| `src/fg-input.inc` | FG-evaluate hook: parameter validation, copies, park, worker half, session hand-over. |
| `src/fg-composite-shader.h` | UI-preserving composite (cs_5_0), shared with `tests/vulkan-fg-input/test-composite.cpp`. |
| `src/present-adapter.inc` | Identity-carrier hook on the SR runtime, NR entry witness, configuration checks, the reference present-stream feed. |
| `src/present-adapter-config.h` | `vk-present-adapter.ini` keys: `Enabled`, `Source`, `Follow`, `AutoRun`; session ownership. |
| `src/present-nr-witness.asm` | Tail-forwarding thunks that preserve the four arguments and the caller's return address. |
| `src/vkmirror.inc` | Worker dispatch (`fg_arm`), FG handle tracking, mirror/feed arbitration in the evaluate detour. |
| `src/synth.inc`, `src/bridge.inc`, `src/bridge.h` | sRGB path in the colour pass, fence-completion proofs, resource retention when completion is unproven, swapchain contract capture. |
| `src/AdapterEvents.fx` | Empty technique so ReShade 6.8 emits `begin_effects` (present-stream feed only). |
| `src/vk-present-adapter.ini.example` | Documented configuration. |
| `tests/vulkan-fg-input/` | Offline tests (below). |

## Tests

All require MSVC (`tests/vulkan-fg-input/vcvars.cmd` finds it) and, for the
round-trip host, `VULKAN_SDK` plus ReShade 6.8 installed as `VK_LAYER_reshade`.

| Command | Checks |
| --- | --- |
| `test-witness.cmd` | The asm thunks preserve arguments, return address and result; the identity gate is thread-local. |
| `test-composite.cmd` | Composite on a D3D12 device: 2868 scene pixels take the NR result, 192 opaque UI pixels are untouched, 12 translucent pixels blend. |
| `build-roundtrip.cmd` + `run-roundtrip.py <dir> --assets <dir> [--hdr] [--with-rr-module] [--hook-point 2]` | Off-screen Vulkan host through the present-stream feed: copy, colour round-trip and two NR requests with a pause; validates zero ROI error, single Generic workset, history reset and two NR passes after the host exits. Needs the third-party binaries in `--assets` (not shipped). |

The fg-input path has no offline host because it needs a real DLSS-G evaluate;
it was validated in the game with the request file (copy -> colour round-trip
-> NR) before being switched on continuously.

## Measurements (Arknights: Endfield, 5120x2160, two NR passes, fixed 6x)

PresentMon 2.5.1, same scene, same session:

| | idle (no NR) | present-stream feed | **fg-input feed** |
| --- | --- | --- | --- |
| presents / s | 299-320 | 29 | **109** |
| simulation marks / s | 49-53 | 14.5 | **22** |
| presents per simulation | 6.1 | 2.0 | **4.9** |
| MsBetweenPresents p50 / p95 | 3.3 / 4.0 | 32 / 44 | **7.3 / 28** |

The remaining gap to 6x is the ~28 ms the real frame spends parked while NR
runs; generated frames cannot present during that window, so the cadence is
bimodal. Pipelining (interpolate frame N-1 while NR processes frame N) would
remove it at the cost of one frame of latency and is not part of this change.

## Known limits

* UNORM8 sRGB FG inputs only; other formats and non-full-image subrects are
  refused with a log line and the game keeps its own image.
* Requires the game to supply `DLSSG.HUDLess` (Endfield does).
* One effect runtime / one swapchain; a resolution or format change while the
  feed owns the session is refused unless `Follow=1` can hand it over on the
  render thread.
* Generic's layer count and per-layer parameters are global, so the two
  sources (mirror at Upscaled, feed at Present) cannot run different layer
  configurations; only the source differs.

## Minimal integration for maintainers

If you prefer not to take the whole change, the essential pieces are, in order:

1. `fg-input.inc` + the two-line hook in `ForwardVkEvaluate` for handles noted
   as feature 11, and the `fg_arm` branch in `VkmWorkerProc`.
2. The identity-carrier hook (`PresentAdapterCarrier`, `PresentAdapterSrEnter`,
   `present-nr-witness.asm`) so Generic's after-upscale NR can be driven without
   an extra DLAA pass.
3. `Synth12Build(..., srgb=true)` and the sRGB branch in the colour pass.
4. The "no NR entry is not a failure" rule and the no-device-wait rule on the
   FG thread; both were found in the game, not offline.

The present-stream feed (`Source=present`) can be dropped entirely; it is kept
because its offline host is the only way to regression-test the colour path
without a game.
