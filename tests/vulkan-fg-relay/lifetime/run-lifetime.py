"""Isolated ReShade add-on unload/reload regression (no device, no rendering).

host.exe loads the real ReShade 6.8 from C:\\ProgramData\\ReShade\\ReShade64.dll
(the path its installer uses), creates and destroys two Vulkan instances, and
checks that RenoDX Generic (renodx-dlss5.addon64) is registered, unloaded and
re-registered normally while dlss5-bridge.addon64 scans loaded modules.

Modes (the run passes when the mode's expected exit code comes back):
  race     pauses the bridge's module scan while it holds its module references,
           destroys the first instance and creates the second. Exit 0 means the
           OLD bug was REPRODUCED (Generic unregistered while the final instance
           lives); use it with a bridge build that predates ngx-module-scan.h.
  stable   the same gated sequence; exit 0 means Generic registered twice, the
           bridge took zero references to it, and everything unloaded cleanly.
  control  no bridge at all; same checks as stable.
  late-ref injects one extra reference after the notification; the host must
           detect it and exit 12 (negative control for the stable checks).

Usage:
  python run-lifetime.py <new-dir> --mode race --bridge <dlss5-bridge.addon64>
         --generic <renodx-dlss5.addon64>
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
RESHADE = Path('C:/ProgramData/ReShade/ReShade64.dll')


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--mode', choices=('race', 'stable', 'control', 'late-ref'), required=True)
    parser.add_argument('--bridge', type=Path)
    parser.add_argument('--generic', type=Path, required=True)
    parser.add_argument('--host', type=Path, default=HERE / 'build' / 'host.exe')
    args = parser.parse_args()
    if args.mode != 'control' and args.bridge is None:
        parser.error('--bridge is required unless --mode control')
    if not RESHADE.exists():
        parser.error(f'ReShade is not installed at {RESHADE}')
    directory = args.directory.resolve()
    directory.mkdir(parents=True, exist_ok=False)
    files = {'host.exe': args.host.resolve(), 'renodx-dlss5.addon64': args.generic.resolve()}
    if args.mode != 'control':
        files['dlss5-bridge.addon64'] = args.bridge.resolve()
    identities = {}
    for name, source in files.items():
        shutil.copy2(source, directory / name)
        identities[name] = {'source': str(source), 'sha256': digest(directory / name)}
    identities['ReShade64.dll (loaded in place)'] = {'source': str(RESHADE), 'sha256': digest(RESHADE)}
    (directory / 'ReShade.ini').write_text(
        '[ADDON]\nAddonPath=.\n[GENERAL]\nNoReloadOnInit=1\n'
        '[RenoDX.DLSS5]\nEnableHooks=1\nNRHookPoint=2\nNeuralUplift=1\n', encoding='ascii')
    (directory / 'vk-present-adapter.ini').write_text(
        '[Adapter]\nEnabled=1\nSource=fg-input\nFollow=1\nPipeline=2\nImport=1\n', encoding='ascii')
    env = os.environ.copy()
    env['RESHADE_BASE_PATH_OVERRIDE'] = str(directory)
    env['VK_LOADER_LAYERS_DISABLE'] = '~implicit~'
    env.pop('VK_LAYER_PATH', None)
    env.pop('DISABLE_VK_LAYER_reshade_1', None)
    started = datetime.now().astimezone().isoformat()
    command = [str(directory / 'host.exe'), args.mode]
    try:
        process = subprocess.run(command, cwd=directory, env=env, capture_output=True, timeout=40)
        output, code = process.stdout + process.stderr, process.returncode
    except subprocess.TimeoutExpired as error:
        output, code = (error.stdout or b'') + (error.stderr or b'') + b'\nFAIL host timeout\n', 124
    (directory / 'stdout.txt').write_bytes(output)
    expected = 12 if args.mode == 'late-ref' else 0
    (directory / 'run.json').write_text(json.dumps({
        'started_at': started, 'finished_at': datetime.now().astimezone().isoformat(),
        'command': command, 'exit_code': code, 'expected_exit_code': expected, 'files': identities,
        'scope': 'Two Vulkan instances only; no device, swapchain, GPU workload or game.',
    }, indent=2), encoding='utf-8')
    print(output.decode('utf-8', errors='replace'))
    print(f'exit_code={code} expected={expected} -> {"PASS" if code == expected else "FAIL"}')
    return 0 if code == expected else 1


if __name__ == '__main__':
    raise SystemExit(main())
