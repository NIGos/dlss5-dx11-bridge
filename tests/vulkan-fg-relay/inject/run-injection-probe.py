"""Device-extension injection and D3D12 <-> Vulkan shared-fence probe.

host.exe creates a Vulkan device WITHOUT VK_KHR_external_semaphore_win32, three
seconds after start (later than the add-on installs its hook). The run passes
when the bridge log shows the extension was appended at vkCreateDevice and both
directions work on that device: a D3D12 Signal observed as a Vulkan timeline
value, and a D3D12 Signal satisfying vkWaitSemaphores.

Needs: an NVIDIA GPU with Vulkan and D3D12, ReShade 6.8 installed as
VK_LAYER_reshade, and the bridge build under test.

Usage: python run-injection-probe.py <new-dir> --bridge <dlss5-bridge.addon64>
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


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--bridge', type=Path, required=True)
    parser.add_argument('--host', type=Path, default=HERE / 'build' / 'host.exe')
    args = parser.parse_args()
    directory = args.directory.resolve()
    directory.mkdir(parents=True, exist_ok=False)
    files = {'probe-host.exe': args.host.resolve(), 'dlss5-bridge.addon64': args.bridge.resolve()}
    for name in ('ReShade.ini', 'TestPreset.ini', 'AdapterEvents.fx', 'dlss5-bridge.cfg'):
        files[name] = HERE / 'template' / name
    identities = {}
    for name, source in files.items():
        shutil.copy2(source, directory / name)
        identities[name] = {'source': str(source), 'sha256': digest(directory / name)}
    (directory / 'vk-present-adapter.ini').write_text(
        '[Adapter]\nEnabled=1\nSource=fg-input\nPipeline=2\nImport=1\nTrace=0\n', encoding='ascii')
    env = os.environ.copy()
    env['RESHADE_BASE_PATH_OVERRIDE'] = str(directory)
    env['VK_LOADER_LAYERS_DISABLE'] = '~implicit~'
    env.pop('DISABLE_VK_LAYER_reshade_1', None)
    started = datetime.now().astimezone().isoformat()
    process = subprocess.run([str(directory / 'probe-host.exe')], cwd=directory, env=env,
                             capture_output=True, timeout=90)
    output = process.stdout.decode('utf-8', errors='replace') + process.stderr.decode('utf-8', errors='replace')
    (directory / 'probe-stdout.txt').write_text(output, encoding='utf-8')
    (directory / 'run.json').write_text(json.dumps({
        'started_at': started, 'finished_at': datetime.now().astimezone().isoformat(),
        'exit_code': process.returncode, 'files': identities,
    }, indent=2), encoding='utf-8')
    print(output, flush=True)
    print(f'exit_code={process.returncode}')
    return process.returncode


if __name__ == '__main__':
    raise SystemExit(main())
