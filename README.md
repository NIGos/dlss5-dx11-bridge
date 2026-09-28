# DLSS 5 Bridge

> [!WARNING]
> **Unofficial download site:** `dlss5bridge.com` is not operated or endorsed by this project.
> Get DLSS 5 Bridge from [this repository's GitHub releases](https://github.com/NIGos/dlss5-bridge/releases).
> Its ZIP contains an unofficial `.exe` installer. [Hybrid Analysis](https://hybrid-analysis.com/sample/5c3cc8dec5827d57f4cfe0968415eeb44ed3f865253bccbca95689989e462837/6aa123764f15cef3a90c2e95)
> classifies it as **Malicious** and records attempts to add Microsoft Defender exclusions for other executables.
> **Do not run installers from that site.** A user's reported compromise has not been independently verified;
> see [#30](https://github.com/NIGos/dlss5-bridge/issues/30).
> [Verify your download before loading it](VERIFYING-DOWNLOADS.md): official SHA-256 values and an optional PowerShell checker.

**A bridge for DLSS 5 Neural Rendering add-ons in DirectX 11 and Vulkan games,
with an optional optical-flow path for games without DLSS.**

A ReShade add-on that mirrors a DirectX 11 or Vulkan game's DLSS onto a private
DirectX 12 session, where a compatible neural rendering add-on can process it.
For games without DLSS, it can build substitute inputs from ReShade's depth
and NVIDIA Optical Flow, or a ReShade motion-vector shader. The game's files
are not patched.

This bridge does not do neural rendering itself. It needs a separate compatible
**DLSS 5 Neural Rendering add-on**, such as RenoDX's `renodx-dlss5.addon64`, distributed in
[its Discord channel](https://discord.com/channels/1408098019194310818/1542647972695904317),
together with its `nvngx_dlssnr.dll`. The bridge only gives that add-on a
place to work.

If it is useful to you, you can help cover the AI tooling used in its
development:

[![Support on Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/nigos)

Releases and their notes: [github.com/NIGos/dlss5-bridge/releases](https://github.com/NIGos/dlss5-bridge/releases).

**Release status (11 September 2026).** [v1.4.12](https://github.com/NIGos/dlss5-bridge/releases/tag/v1.4.12)
remains the stable release. `main` contains the cumulative 1.4.13 prerelease
changes and the [D3D11 depth/MV conversion fix](https://github.com/NIGos/dlss5-bridge/pull/34).
The latest test build, [v1.4.13-pre7](https://github.com/NIGos/dlss5-bridge/releases/tag/v1.4.13-pre7),
also fixes a Vulkan Frame Generation startup failure
reported in [Endfield](https://github.com/NIGos/dlss5-bridge/issues/27#issuecomment-5606803978):
it supplies the command buffer's device when NGX cannot infer one alongside
the Bridge's private device, and clears obsolete hooks when an NGX module unloads.
Pre7 includes all pre6 changes and has verified GitHub Actions build provenance.
It is still unsigned; follow the
[download verification instructions](VERIFYING-DOWNLOADS.md) before loading it.
Pre6 separated D3D11 temporal histories and the BG3 reporter confirmed that
shimmering was fixed. Pre7 additionally avoids rebuilding shared textures when
split-screen views have different heights. The official build passes the
unequal-height Gym test with neural rendering active; confirmation of the
stutter/shutdown fix in BG3 is still pending. Neural rendering reaching only one
BG3 viewport remains open ([#12](https://github.com/NIGos/dlss5-bridge/issues/12)).
The [BG3 FG reporter](https://github.com/NIGos/dlss5-bridge/issues/28#issuecomment-5609017168)
confirmed neural output, but also tested local hook changes and newer Streamline
files; flickering/ghosting and general hook coexistence remain under investigation.
The [Endfield reporter](https://github.com/NIGos/dlss5-bridge/issues/27#issuecomment-5609207523)
confirmed DLAA + MFG 4x + neural rendering working together on pre5.
The Endfield mirror slowdown
remains unresolved; the FG startup fix does not establish an FPS improvement.

Some neural add-ons support additional graphics APIs directly. Whether you need
this bridge depends on the add-on build and the game. See the compatibility
notes below; support for NGX D3D12 calls alone does not guarantee compatibility.

## What it does

The DLSS 5 add-on is not modified. It receives genuine NGX D3D12 calls on a
private D3D12 device, and its result is copied back into the game's output.
Three routes; the substitute is off by default:

| Route | Game | Contract |
| --- | --- | --- |
| **D3D11 bridge** | DirectX 11 with DLSS | The game's Color, Depth and MotionVectors are copied into shared textures, evaluated on D3D12 and copied back. The bridge follows the supplied dimensions, regions and parameters, with defaults for missing values. |
| **Vulkan mirror** | Vulkan with DLSS | The game's own, mirrored the same way through imported D3D12 textures. `vk_mirror=1`, the default. |
| **Vulkan final-frame feed** | Vulkan with DLSS Frame Generation | The neural add-on's "Present" hook point, which it does not serve on Vulkan itself: the real frame is processed once at the game's Frame Generation input (HUD-less in, UI preserved), so FG interpolates the enhanced image and the presented multiplier stays. Opt-in through `vk-present-adapter.ini`; see [VULKAN-FG-INPUT-NR.md](VULKAN-FG-INPUT-NR.md). |
| **Substitute contract** | Games without DLSS that expose usable depth and motion inputs through ReShade | DLAA at back-buffer size, using NVIDIA Optical Flow or a ReShade motion-vector shader. Requires `synth=1`; availability depends on the API, driver and inputs. |

With `source=auto`, the game's own DLSS takes priority. Prefer that route when
the game has DLSS. The substitute can also run while the game's DLSS is off,
but switching between the two has limitations described below.

The substitute is a real DLSS feature fed approximated inputs, and it shows:
text softens and dense foliage smears. It is an option, not a default.

## Requirements

An NVIDIA GPU and driver that support DLSS, plus a neural add-on and model
compatible with that GPU. D3D12 support alone is not sufficient. Required files:

| File | From |
| --- | --- |
| ReShade 6.0 or newer **with full add-on support** | [reshade.me](https://reshade.me/). Install for the game's rendering API: D3D11 uses a local proxy DLL; Vulkan uses ReShade's Vulkan layer. A local `dxgi.dll` is not the Vulkan installation method. |
| A compatible neural rendering add-on, such as `renodx-dlss5.addon64` | [its Discord channel](https://discord.com/channels/1408098019194310818/1542647972695904317). Use one neural consumer at a time; see compatibility notes below. |
| `nvngx_dlssnr.dll` | shipped with that add-on |
| `dlss5-bridge.addon64` | this project |
| `nvngx_dlss.dll` | Use the game's existing DLSS for the native mirror. For the substitute path, supply version **3.1.13 or newer** beside the executable, even in a game without DLSS. The driver alone does not supply the required SR snippet there. |

The neural add-on's own toggle has to be on, in its panel or in `ReShade.ini`.

**The neural add-on's build matters, and its version number does not identify
one.** Two builds of 2026-08-28 declare the same version; the newer one needs to
see this add-on's D3D12 device through ReShade's proxy, and underneath it
reports active and writes nothing. The log prints a SHA-256 for every add-on
beside this one, says whether that build has been measured here, and keeps the
proxy for a build that needs it.

## Install

**Download `dlss5-bridge.addon64` only from [our GitHub releases](https://github.com/NIGos/dlss5-bridge/releases).**
Bridge has no installer and never asks you to disable antivirus protection or
add exclusions. ReShade has its own legitimate installer, available separately
from [reshade.me](https://reshade.me/). [Verify the Bridge file](VERIFYING-DOWNLOADS.md)
before putting it in the game folder.

Install ReShade for the correct API, then copy `dlss5-bridge.addon64` and the
neural add-on's files into its add-on search location, normally beside the
game executable. On first run the Bridge writes `dlss5-bridge.cfg` with defaults
for games using their own DLSS. Games without DLSS need the opt-in below.
Keep only one Bridge DLL installed; back up old DLLs outside the game folder.
To remove the Bridge, delete its `.addon64` file.

The settings file's first line is the version that wrote it. A different version
replaces the file with its own defaults on first run and says so in the log;
the same version never touches it. `# dlss5-bridge keep` as the first line
keeps a file across versions.

**Games without DLSS.** The substitute contract needs three files beside the
game's executable that such a game does not bring: the DLSS 5 add-on and its
`nvngx_dlssnr.dll`, and a **`nvngx_dlss.dll` of version 3.1.13 or newer**,
copied by hand from any game that has DLSS. The NVIDIA driver does not supply
that file in the game's folder. Open ReShade's overlay and, in **DLSS 5 Bridge**,
enable **Replace DLSS when the game isn't using its own** (or set `synth=1`).
Enable neural rendering in the separate neural add-on's panel too. Optical flow
is selected by default; the Bridge panel shows whether usable depth and motion
inputs are available. If the DLSS DLL is missing, the panel and log name it.

**Upgrading from 1.1.0 or earlier:** the files were called
`dlss5-dx11-bridge.addon64` and `dlss5-dx11-bridge.cfg`. Delete the old
`.addon64` — ReShade loads both, and the older one ends up on screen. The old
`.cfg` is read where no current one exists.

The Bridge hooks supported NGX entry points in memory. Hook selection has guards
for unsupported exports; it does not patch every export indiscriminately.
The prerelease additionally leaves the Frame Generation DLL untouched.

## Configuration

`dlss5-bridge.cfg` is re-read about once a second, so most keys take effect
without a restart. While a Vulkan mirror session holds the session the file is
not re-read until the game's DLSS goes quiet.

| Key | Default | Meaning |
| --- | --- | --- |
| `vk_mirror` | 1 | Hook the Vulkan NGX entry points and mirror a Vulkan game's own DLSS. Read once at launch. Depth aspects carried: `D32_SFLOAT`, `D32_SFLOAT_S8_UINT`, `D24_UNORM_S8_UINT`; a 16-bit aspect is refused by name. |
| `synth` | 0 | Allow the substitute contract, for a game with no DLSS and for a game whose DLSS is switched off. One switch in the panel. |
| `synth_after` | 0 | With `synth=1`, seconds of silence from the game before a substitute is built for a game that has never asked. `0` is the default 10 s. A delay, not an opt-in. |
| `source` | `auto` | `auto` prioritizes the game's DLSS and permits the substitute when `synth=1`; `mirror` and `synth` pin one; `off` disables both processing paths. |
| `ofa_grid` | 2 | Grid of the driver's optical flow engine on the substitute path: `1`, `2`, `4`, or `0` to use a ReShade motion-vector shader instead. |
| `ofa_perf` | 20 | Optical flow effort, NVIDIA's own values: `5` slow, `10` medium, `20` fast. |
| `mv_sign_x`, `mv_sign_y` | 0 | Force the motion-vector sign (`1`, `-1`); `0` uses the provider's convention or the engine's measurement. Diagnostic. |
| `vk_present` | 0 | How the substitute's result reaches a Vulkan back buffer: `0` copies where the image allows it and draws it otherwise, `1` copies only, `2` draws always. |
| `vk_sync` | 0 | How the substitute's Vulkan transport orders itself against the private D3D12 device: `0` parks the game's queue on a Vulkan event while a worker thread runs the optical flow and the evaluate, `1` waits the queue idle on the CPU instead, `2` pipelines. `1` costs a full CPU-GPU serialisation every frame and is the fallback if the park misbehaves on a driver. `2` runs one frame's evaluate while the game renders the next, so the wait the game's queue reaches is on work that started a frame earlier rather than on work that starts when it gets there -- at the price of showing the result one present later, which is about 17 ms at 60 fps and 33 at 30, and one more Output texture. That texture is the back buffer's own size and format, so it is width x height x bytes per texel: 8.3 MB at 1920x1080 and 24.6 MB at 3840x1600 for a 4-byte format, double each on an RGBA16F back buffer. |
| `stage` | 3 | Processing level: `0` disables processing but leaves hooks installed. D3D11: `1` input copies, `2` plus depth conversion, `3` full processing. The substitute needs `3`; the Vulkan mirror records from `2` and copies results back at `3` with `mode=2`. |
| `mode` | 2 | `0` never writes to the game, `1` transport only, `2` the full path. |
| `skip_game` | 1 | D3D11 only: skip the game's DLSS evaluate when the bridge can replace its full output. Vulkan always forwards the game's evaluate. |
| `flags` | -1 | `DLSS.Feature.Create.Flags`. `-1` copies the game's value. `107` is treated as unset (an old default); use `108` to force that pattern. |
| `subrects` | 1 | Fallback for `DLSS.Enable.Output.Subrects` when the game sets none. D3D11 bridge only. |
| `reset_every` | 0 | `1` sets the NGX Reset flag every frame. Diagnostic. |
| `pixels` | 0 | `1` reads pixels back to the CPU on frames 2 to 4. Diagnostic; stalls the GPU. |
| `dred` | 1 | Ask D3D12 to record what the GPU was executing, so a device reset can be explained. Read when the session opens. |
| `skip_exe` | 1 | `1` hooks the executable's own NGX exports only if no library exports them within a minute, so a game's image is not patched at startup. `0` hooks at once, `2` never. |
| `unwrap` | 1 | Hand NGX the D3D12 device underneath ReShade's proxy. `0` keeps the proxy. A neural add-on build measured to need the proxy, and ReShade loaded as `d3d12.dll`, both override `1` automatically. `2` forces unwrapping despite those checks; use only for a targeted diagnostic. See the Linux reports below for cases needing `0`. |
| `ngx_loader` | 0 | What to do about an NVIDIA driver whose NGX loader drives neural rendering into a snippet that faults (32.0.16.1664 and 1686 with `nvngx_dlssnr.dll` 310.8.0.0): `0` closes that route in memory at attach, so the DLSS 5 add-on drives the feature itself as it did on 32.0.16.1656; `1` leaves the driver alone; `2` loads the previous driver's loader instead, if the driver store still holds one. Applies only when a snippet measured to fault is beside the game. |
| `shape` | 0 | `1` builds the mirrored feature at the render region the game declares on each evaluate (`DLSS.Render.Subrect.Dimensions`) and rebuilds when that region changes. Confirmed by the PSO2 reporter to fix `0xBAD00005` in [#8](https://github.com/NIGos/dlss5-bridge/issues/8#issuecomment-5552628561). Leave at `0` unless needed. |
| `stall_test` | 0 | Holds the private D3D12 queue for this many milliseconds, once, at the 60th submission, to exercise the stall path. Diagnostic. |
| `unwrap_list` | 0 | `1` hands NGX the command list underneath ReShade's proxy. Diagnostic. |
| `probe` | 0 | `1` runs a standalone NGX D3D12 probe at attach and logs the result. Diagnostic. |
| `hash_out` | 1 | Once per feature build, 60 frames in, read the input and the output back and log a hash of the output, the mean of each channel of both, and the brightness ratio out/in. One readback per build; `0` disables. D3D11 bridge only. |

A second, optional file beside the add-on, `vk-present-adapter.ini`, enables the
Vulkan final-frame feed (`Enabled=1`, `Source=fg-input`, `Follow=1`); it is read
once at start-up and documented in `src/vk-present-adapter.ini.example`.

## Status panel

ReShade's overlay has a **DLSS 5 Bridge** window. It shows which route holds
the session and why, the contract in use, the motion-vector source on the
substitute path, and whether frames are still arriving. One switch, **Replace
DLSS when the game isn't using its own**, writes `synth`; Detail, Speed and
Direction write the optical flow and sign keys. A **Details** checkbox adds
what a bug report needs: build, transport, create flags, file paths, every
add-on in the folder and the neural add-on's `ReShade.ini` section.

The panel writes single lines into `dlss5-bridge.cfg` and reads the file back
like any other edit. There is no save, reload or reset: deleting the file
restores the defaults on the next launch.

The panel has compatibility handling for ReShade 6.0.0 through 6.8.0. A ReShade built without its overlay,
or the "Release Signed" build, has no panel; the log says so and nothing else
is affected.

## Log

`dlss5-bridge.log` beside the add-on records the environment, every add-on and
NVIDIA model file in the folder with its SHA-256, the contract read from the
game, the presentation (output format, colour space, HDR or SDR contract, the
exposure texture's value), every NGX result, a cumulative delivered-frame count
every 600 frames, on the D3D11 bridge the brightness the output came back at
relative to the input, and a timing line:

```
[bridge] 600 frames: bridge CPU 0.84 ms/frame | frame interval 16.00 ms (62.5 fps) | spread 5.74-29.93 ms | bridge is 5% of the frame | d3d12 43200/43202 (2 behind)
```

*bridge CPU* is time inside the add-on, mostly waiting on the GPU. *spread* is
the widest and narrowest frame interval in the window. *d3d12 N/M* is how far
the D3D12 side runs behind; a few is ordinary pipelining.

The bridge adds texture copies, synchronization and, on the substitute path,
motion estimation, as well as the neural pass. Their cost depends on the API,
scene, resolution, GPU and settings. This CPU timing includes waits and does
not establish whether a game is CPU- or GPU-limited. Compare the same scene
and settings in separate runs; Vulkan configuration changes may require a restart.

## Reporting a problem

A screenshot of the panel with Details ticked answers the first questions.
Past that, attach fresh `dlss5-bridge.log` and `ReShade.log` files. Please name
the game, API, Bridge version, GPU/driver, neural add-on/model build, and whether
DLSS, FG/MFG and HDR were enabled. Reports from titles not listed below are useful even when
everything works.

## Compatibility

Developed and verified on one GPU, on Baldur's Gate 3 (D3D11 and Vulkan), Red
Dead Redemption 2 (Vulkan) and Skyrim Special Edition (D3D11). Every release is
run through [ngxGym](https://github.com/NIGos/ngxGym), a synthetic DLSS host
that exercises both backends, mode changes, contract faults and the substitute
contract against real NGX, without a game.

NVIDIA drivers 32.0.16.1664 and 1686 route neural rendering (NGX feature 18) into
`nvngx_dlssnr.dll` itself. The 310.8.0.0 build tested with those drivers
faulted inside D3D12 on that route: with the DLSS 5 add-on present the
game terminated or stopped presenting. The add-on closes that route in the
loaded `_nvngx.dll` at attach -- one pointer, in memory, nothing on disk -- and
the DLSS 5 add-on drives the feature as it did on 32.0.16.1656. See `ngx_loader`.
Other model builds exist; this is a workaround for the identified combination,
not a claim that all current models need it.

Additional reports, scoped to the tested setups:

- [Endfield Vulkan, v1.4.12](https://github.com/NIGos/dlss5-bridge/issues/17#issuecomment-5553000888):
  the reporter confirmed correct neural rendering. In
  [#27](https://github.com/NIGos/dlss5-bridge/issues/27), pre2 restored FG in the
  reported setup; the mirror slowdown remains open, with no FPS improvement
  reported from pre3.
- [BG3 D3D11 with neural-upstream v0.3.0](https://github.com/NIGos/dlss5-bridge/issues/26):
  reported working on RTX 4090 / driver 616.64. Not locally verified.
- [Linux/Proton, D3D11 substitute](https://github.com/NIGos/dlss5-bridge/issues/22):
  users report success with NapXDD's addon-dlssnr-linux and `unwrap=0` on RTX
  4080 and 5070. Hardware optical flow was unavailable in those reports; they
  used ReShade motion-vector shaders. This does not establish compatibility
  for other neural add-ons or the Vulkan mirror. ReShade's descriptor-handle
  fix is tracked in [PR #435](https://github.com/crosire/reshade/pull/435).

Other titles reported working by users (not a guarantee for every build or mode):

| Title | Engine | DLSS from |
| --- | --- | --- |
| Baldur's Gate 3 | Divinity 4.0 | the game |
| Final Fantasy XIV Online | in-house | the game |
| The Legend of Heroes: Trails beyond the Horizon | Falcom | the game |
| Tainted Grail: Fall of Avalon | Unity | the game |
| 7 Days to Die | Unity | the game |
| Skyrim Special Edition | Creation | a DLSS injector mod |
| Fallout 4 | Creation | a DLSS injector mod |
| S.T.A.L.K.E.R. Anomaly | X-Ray | an upscaler injector mod (SSS24) |
| Assetto Corsa | kunOS | Custom Shaders Patch (Preview 338 or later) |

Tainted Grail also has an open, setup-specific device-creation report in
[#24](https://github.com/NIGos/dlss5-bridge/issues/24).

The bridge follows the game's NGX inputs rather than requiring a game-specific
integration. DLSS supplied by a mod can be picked up like built-in DLSS;
compatibility still depends on the supplied inputs and the other add-ons.

## Known limits

The Bridge attempts to stand down on detected failures, but failures in a
driver or another add-on can still crash the game or produce an incorrect image.

- **Two NGX sessions.** Vulkan runs the game's DLSS and the mirrored DLSS.
  D3D11 can skip the original with `skip_game=1` when replacing the full output.
  The Bridge serializes its intercepted NGX calls with its private work, which
  can introduce waits.
- **Vulkan exposure texture.** v1.4.12 carries both `R16_SFLOAT` and
  `R32_SFLOAT`, and supports different input/output color formats. Unsupported
  exposure contracts are logged and left to the game's own DLSS; changing
  exposure flags is not an equivalent fix.
- **HDR on the substitute path.** The input may already include the game's
  tone mapping and UI. With neural rendering this can alter colors across the
  whole view. A general solution that preserves the game's presentation is
  still under investigation; no universal HDR fix is included.
- **FG and split screen.** The 1.4.13 prereleases fix reproduced Vulkan FG
  initialization and D3D11 partial-output defects. BG3's complete split-screen
  neural output remains unresolved ([#12](https://github.com/NIGos/dlss5-bridge/issues/12)).
  A separate hook conflict with BG3's Vulkan FG mod also remains open
  ([#28](https://github.com/NIGos/dlss5-bridge/issues/28)).
- **Substitute before the game's DLSS.** This order has caused faults in tested
  neural add-on builds when native DLSS first appears. Prefer the native DLSS
  route and leave the substitute off in games that provide it.
- **Substitute depth must be back-buffer sized.** A smaller or otherwise
  mismatched depth buffer does not meet the substitute's requirements.
  The Vulkan substitute reads depth with a compute pass that
  needs `VK_KHR_push_descriptor`; a driver without it is refused by name.
- **Optical flow on Vulkan** runs on a private D3D11 device with two textures of
  its own, because the driver refuses to open the transport's D3D12 textures as
  D3D11 aliases. On D3D12 runtimes the engine is not wired; motion vectors come
  from a ReShade shader there.
- **Vulkan mirror** runs one frame in flight, one pipeline bubble per frame, and
  parks the game's command buffer on a `VkEvent` pair. That park raises two
  Khronos validation messages per frame by construction
  (`VUID-vkSetEvent-event-09543`, `VUID-vkCmdWaitEvents-srcStageMask-01158`);
  every NVIDIA driver measured accepts it.
- Resolution, preset and display-mode changes rebuild, and so does the game
  creating its DLSS feature again, whatever the shape: the create flags can
  change on their own, as IsHDR does when a game switches HDR on. On Vulkan a
  display change also recreates ReShade's runtime; the mirror and the
  substitute both follow it.
- **Partial D3D11 output.** Stable v1.4.12 copies a partial result at 0,0;
  this can overwrite the wrong region when the game supplies a nonzero output
  origin. The 1.4.13 prereleases preserve the declared origin. The original evaluate
  is retained on partial-output frames to preserve the surrounding texture.
- Verbose logging is always on.

## Related

[dlss5-d3d12-fix](https://github.com/NIGos/dlss5-d3d12-fix) addresses a
different failure of the same neural add-on: a DirectX 12 game whose DLSS output
carries a mip chain. Check that project's diagnosis before using it;
STANDBY/FAILED alone does not identify that problem.

## Building

Windows SDK and MSVC; no external dependencies. `src\build.cmd` sets up the
MSVC environment and runs the two lines:

```
rc /nologo version.rc
cl /nologo /W4 /O2 /MT /EHsc /std:c++17 /Ireshade /LD dlss5-bridge.cpp version.res ^
   /Fe:dlss5-bridge.addon64 /link /DLL user32.lib advapi32.lib bcrypt.lib
```

`bridge.h`, `bridge.inc`, `synth.inc`, `vkmirror.inc` and `depth_convert_spv.h`
are included by the `.cpp`. The version is in two places that stay in step:
`BRIDGE_VERSION` in the `.cpp` and the numbers in `version.rc`.

## Third-party code

- The ReShade add-on API headers under `src/reshade/` are Copyright 2014 Patrick
  Mours, BSD 3-clause; their licence is in
  [src/reshade/LICENSE.md](src/reshade/LICENSE.md) and travels with every binary.
- `depth_convert.comp` and `depth_convert_spv.h` are from
  [AlanBacker/dlss5-vk-bridge](https://github.com/AlanBacker/dlss5-vk-bridge),
  MIT, verbatim, with their dual copyright in the array's header. Two ideas in
  `vkmirror.inc` come from the same project: creating the shared textures on
  D3D12 and importing them into the game's `VkDevice`, and parking the game's
  command buffer on a `VkEvent` pair.

Everything else is this project's own, under MIT.

### Hook lifecycle (next release)

MinHook is compiled into the bridge; no additional DLL is needed. Once NGX hooks
are installed, the bridge stays loaded until the game exits. Restart the game
to replace or unload it; live configuration changes still work. See
[implementation and tests](MINHOOK-INTEGRATION.md) and
[third-party notices](THIRD-PARTY-NOTICES.txt).
