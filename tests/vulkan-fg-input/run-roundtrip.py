"""Run the Vulkan adapter's Source=present path in an isolated, off-screen host.

Requires an assets directory holding the third-party files this repository does
not ship: renodx-dlss5.addon64 (Generic 8.5.0-rc10), nvngx_dlss.dll,
nvngx_dlssnr.dll, optionally nvngx_dlssd.dll (for --with-rr-module), plus a
ReShade.ini with a [RenoDX.DLSS5] section (EnableHooks=1, NRHookPoint=0,
NRSourceEncoding=0) and a TestPreset.ini. ReShade 6.8 must be installed as the
VK_LAYER_reshade layer. Exit 0 means the bounded transport/NR/workset checks in
validate-roundtrip.py passed; it says nothing about game quality or FG coverage.
"""
import argparse
import csv
import hashlib
import json
import os
import runpy
import shutil
import subprocess
from datetime import datetime
from pathlib import Path

validate = runpy.run_path(str(Path(__file__).with_name('validate-roundtrip.py')))['validate']

GENERIC_SHA256 = 'dcd93881e976ad033d83c2bb01f4bc3e4ddc59c15fe0dd4ca165bc5fc7d1ac68'


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('directory', type=Path, help='new evidence directory (must not exist)')
    parser.add_argument('--assets', type=Path, required=True, help='directory with the third-party files listed above')
    parser.add_argument('--bridge', type=Path, default=Path(__file__).resolve().parents[2] / 'src/dlss5-bridge.addon64')
    parser.add_argument('--hdr', action='store_true')
    parser.add_argument('--full-resolution', action='store_true', help='use 5120x2160; exit the game first')
    parser.add_argument('--with-rr-module', action='store_true', help='also load nvngx_dlssd.dll as a game process does')
    parser.add_argument('--source-encoding', type=int, choices=(0, 1), default=0)
    parser.add_argument('--hook-point', type=int, choices=(0, 1, 2), default=0)
    args = parser.parse_args()
    if args.full_resolution:
        processes = subprocess.run(['tasklist.exe', '/FI', 'IMAGENAME eq Endfield.exe', '/FO', 'CSV', '/NH'],
                                   capture_output=True, check=True)
        rows = csv.reader(processes.stdout.decode('utf-8', errors='replace').splitlines())
        if any(row and row[0].casefold() == 'endfield.exe' for row in rows):
            parser.error('Exit the game before the 5120x2160 test to avoid competing NR allocations.')
    source = Path(__file__).resolve().parents[2] / 'src'
    assets = args.assets.resolve()
    directory = args.directory.resolve()
    directory.mkdir(parents=True, exist_ok=False)
    files = {
        'roundtrip-host.exe': Path(__file__).parent / 'build/roundtrip-host.exe',
        'dlss5-bridge.addon64': args.bridge,
        'renodx-dlss5.addon64': assets / 'renodx-dlss5.addon64',
        'nvngx_dlss.dll': assets / 'nvngx_dlss.dll',
        'nvngx_dlssnr.dll': assets / 'nvngx_dlssnr.dll',
        'AdapterEvents.fx': source / 'AdapterEvents.fx',
        'TestPreset.ini': assets / 'TestPreset.ini',
    }
    if args.with_rr_module:
        files['nvngx_dlssd.dll'] = assets / 'nvngx_dlssd.dll'
    identities = {}
    for name, original in files.items():
        target = directory / name
        shutil.copy2(original, target)
        expected = digest(original)
        if digest(target) != expected:
            raise RuntimeError(f'Copy verification failed: {name}')
        identities[name] = {'source': str(original), 'sha256': expected}
    if identities['renodx-dlss5.addon64']['sha256'] != GENERIC_SHA256:
        raise RuntimeError('The isolated test requires the unmodified Generic 8.5.0-rc10 binary')
    ini = (assets / 'ReShade.ini').read_text(encoding='utf-8-sig')
    for key, value in (('NRSourceEncoding', args.source_encoding), ('NRHookPoint', args.hook_point)):
        if ini.count(f'{key}=0') != 1:
            raise RuntimeError(f'Expected exactly one baseline {key}=0 setting in the assets ReShade.ini')
        ini = ini.replace(f'{key}=0', f'{key}={value}')
    (directory / 'ReShade.ini').write_text(ini, encoding='utf-8')
    (directory / 'dlss5-bridge.cfg').write_text('# dlss5-bridge keep\nmode=2\nstage=3\nunwrap=1\nvk_mirror=1\nsynth=0\n', encoding='ascii')
    (directory / 'vk-present-adapter.ini').write_text('[Adapter]\nEnabled=1\n', encoding='ascii')
    (directory / 'vk-present-adapter.request').write_text('0 0 0\n', encoding='ascii')
    env = os.environ.copy()
    env['RESHADE_BASE_PATH_OVERRIDE'] = str(directory)
    env['VK_LOADER_LAYERS_DISABLE'] = '~implicit~'
    env.pop('DISABLE_VK_LAYER_reshade_1', None)
    command = [str(directory / 'roundtrip-host.exe')]
    if args.hdr:
        command.append('hdr')
    if args.full_resolution:
        command.append('full')
    if args.with_rr_module:
        command.append('load-rr')
    scenario = {
        'name': 'copy12-carrier12-nr16-present-pause-nr16',
        'requests': [dict(token=1, mode=0, frames=12), dict(token=2, mode=2, frames=12),
                     dict(token=3, mode=1, frames=16),
                     dict(token=4, mode=1, frames=16, minimum_presenting_pause_ms=750),
                     dict(token=5, mode=0, frames=0)],
    }
    parameters = {
        'hdr': args.hdr, 'source_encoding': args.source_encoding, 'rr_module_loaded': args.with_rr_module,
        'nr_hook_point': args.hook_point,
        'requested_extent': [5120, 2160] if args.full_resolution else [800, 600],
        'fixture': 'gray-colour-stripes-v1',
        'roi_extent': [16, 16], 'roi_offset_from_bottom_right': [-48, -48],
        'copy_rgb_tolerance': 0, 'carrier_rgb_tolerance': 2 if args.hdr else 1,
        'alpha_tolerance': 0, 'nr_passes': 2, 'host_frame_limit': 600,
        'host_time_limit_ms': 45000, 'process_timeout_s': 60,
        'reshade_ini_sha256': digest(directory / 'ReShade.ini'),
    }
    report = {'started_at': datetime.now().astimezone().isoformat(), 'command': command,
              'scenario': scenario, 'parameters': parameters, 'files': identities, 'timeout': False}
    print(json.dumps({'directory': str(directory), 'command': command, 'parameters': parameters}, indent=2), flush=True)
    try:
        process = subprocess.run(command, cwd=directory, env=env, capture_output=True,
                                 timeout=parameters['process_timeout_s'])
        report.update(exit_code=process.returncode,
                      stdout=process.stdout.decode('utf-8', errors='replace'),
                      stderr=process.stderr.decode('utf-8', errors='replace'))
    except subprocess.TimeoutExpired as error:
        report.update(exit_code=None, timeout=True,
                      stdout=(error.stdout or b'').decode('utf-8', errors='replace'),
                      stderr=(error.stderr or b'').decode('utf-8', errors='replace'))
    report['finished_at'] = datetime.now().astimezone().isoformat()
    (directory / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    validation = validate(directory)
    (directory / 'post-exit-validation.json').write_text(json.dumps(validation, indent=2), encoding='utf-8')
    print(json.dumps({'directory': str(directory), **validation}, indent=2))
    return 0 if validation['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
