# Offline tests for the GPU relay build

All commands run from `cmd` on Windows with MSVC installed
(`tests/vulkan-fg-input/vcvars.cmd` finds it). Every runner writes into a NEW
directory and refuses an existing one. None of these tests starts a game, and
none of them covers the relay's arm/copy/park/release chain, which needs a real
DLSS-G evaluate.

| Test | Needs | Command | Pass |
| --- | --- | --- | --- |
| CPU contracts of the observers and hooks (whole add-on source compiled in) | MSVC | `test-fg-trace.cmd <new-dir>` | `PASS: checks=7382 failures=0` |
| `ngx-module-scan.h` against the real Windows loader | MSVC | `scan\build-scan-test.cmd <new-dir>` then `<new-dir>\scan-test.exe` | `PASS scan-test: 20705 assertions` |
| Add-on unload/reload with the real ReShade and Generic | MSVC, `VULKAN_SDK`, ReShade 6.8 at `C:\ProgramData\ReShade\ReShade64.dll`, `renodx-dlss5.addon64` (8.5.0-rc10) | `lifetime\build.cmd`, then `python lifetime\run-lifetime.py <new-dir> --mode stable --bridge <addon> --generic <renodx-dlss5.addon64>` (also `control`, `late-ref`; `race` with an older bridge) | last line `PASS` |
| Device-extension injection and both shared-fence directions | MSVC, `VULKAN_SDK`, NVIDIA GPU, ReShade 6.8 as `VK_LAYER_reshade` | `inject\build.cmd`, then `python inject\run-injection-probe.py <new-dir> --bridge <addon>` | `exit_code=0` |

`inject\template\` holds the small ReShade/bridge configuration files the probe
runs with; its `dlss5-bridge.cfg` starts with `# dlss5-bridge keep` so the build
under test does not replace it.
