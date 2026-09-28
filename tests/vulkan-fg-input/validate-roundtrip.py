"""Validate copy, carrier and two two-pass NR requests in a fixed-size Vulkan host.

Run after the host exits so ReShade's buffered log is complete. Exit 0 means
the bounded transport/NR/workset checks passed, not game quality or FG support.
The evidence directory must contain result.json, ReShade.log and the Bridge log.
"""
import argparse
import json
import math
import re
from pathlib import Path


SCOPE = ('fixed-size independent host; sampled ROI and NR/SR evidence only; '
         'no game/MFG/motion-quality coverage')
REQUESTS = [(1, 12, 0), (2, 12, 2), (3, 16, 1), (4, 16, 1), (5, 0, 0)]


def validate_logs(result: dict, nr: str, bridge: str) -> dict:
    stdout = result.get('stdout', '').replace('\r\n', '\n')
    adapter = [line for line in bridge.splitlines() if '[present-adapter]' in line]
    accepted = [tuple(map(int, m)) for m in re.findall(
        r'\[present-adapter\] request (\d+) accepted: max=(\d+) frames,', bridge)]
    summaries = [dict(zip(('frames', 'writes', 'no_nr', 'token'), map(int, m)))
                 for m in re.findall(r'\[present-adapter\] request complete: frames=(\d+) '
                                     r'writes=(\d+) no_nr=(\d+)\. token=(\d+)(?=\s|$)', bridge)]
    sample_lines = [line for line in adapter if '[present-adapter] frame=' in line]
    samples, parse_errors = [], []
    for line in sample_lines:
        fields = dict(re.findall(r'\b(\w+)=([^\s,;]+)', line))
        try:
            sample = {key: int(fields[key]) for key in
                      ('frame', 'token', 'nr_entries', 'completed', 'writeback', 'reset', 'sr_entries', 'sr_copies')}
            sample.update(mode=fields['mode'], image=int(fields['image'], 16),
                          cpu_ms=float(fields['cpu_ms'].rstrip('.')),
                          input_wait_ms=float(fields['input_wait_ms'].rstrip('.')))
            samples.append(sample)
        except (KeyError, ValueError) as error:
            parse_errors.append(f'{error}: {line}')
    grouped = {token: [s for s in samples if s['token'] == token] for token in range(1, 5)}
    modes = {1: 'copy', 2: 'carrier', 3: 'NR', 4: 'NR'}
    fixture = re.findall(r'^FIXTURE version=gray-colour-stripes-v1 hdr=([01]) roi=16x16 offset=-48,-48$',
                         stdout, re.M)
    hdr = len(fixture) == 1 and fixture[0] == '1'
    scenario = re.findall(r'^SCENARIO requests_sent=(\d+) accepted=(\d+) completed=(\d+) '
                          r'pause_ms=(\d+) pause_presents=(\d+) stop_token=(\d+)$', stdout, re.M)
    scenario = list(map(int, scenario[0])) if len(scenario) == 1 else []
    sent = [tuple(map(int, m)) for m in re.findall(
        r'^request_sent token=(\d+) frames=(\d+) mode=(\d+)$', stdout, re.M)]
    created = nr.count('created inline NR resources ws')
    failures = [line for line in adapter if re.search(
        r'mismatch|refus(?:ed|ing|al)|fail(?:ed|ure)?\b|unavailable', line, re.I)]
    checks = {
        'host_exit_zero': result.get('exit_code') == 0 and not result.get('timeout', False),
        'fixture_identified': len(fixture) == 1,
        'requests_sent_in_order': sent == REQUESTS,
        'four_requests_accepted': (accepted == [(t, f) for t, f, _ in REQUESTS[:4]] and
                                   sum(' accepted:' in line for line in adapter) == 4),
        'four_requests_completed': (len(summaries) == 4 and
            sum('request complete:' in line for line in adapter) == 4 and
            [(s['token'], s['frames']) for s in summaries] == [(t, f) for t, f, _ in REQUESTS[:4]]),
        'host_observed_completion': bool(scenario and scenario[:3] == [4, 4, 4] and scenario[5] == 5),
        'pause_with_new_presents': bool(scenario and scenario[3] >= 750 and scenario[4] > 0),
        'copy_carrier_all_written': all(any(s == {'token': t, 'frames': 12, 'writes': 12, 'no_nr': 0}
                                                      for s in summaries) for t in (1, 2)),
        'both_nr_requests_written': all(any(s['token'] == t and s['frames'] == 16 and
                                            0 < s['writes'] <= 16 and s['no_nr'] == 16 - s['writes']
                                            for s in summaries) for t in (3, 4)),
        'sample_schema_complete': not parse_errors and len(samples) == 16,
        'sample_frames_complete': all([s['frame'] for s in grouped[t]] == [1, 2, 3, 12 if t < 3 else 16]
                                     for t in grouped),
        'sample_mode_completion': bool(samples) and all(
            s['mode'] == modes.get(s['token']) and s['completed'] == 1 and s['image'] != 0 and
            s['reset'] in (0, 1) and 0 <= s['nr_entries'] <= 2 and
            (s['writeback'] == 1 and s['nr_entries'] == 0 if s['token'] < 3 else
             s['writeback'] == int(s['nr_entries'] > 0)) for s in samples),
        'nr_private_sr_identity_once': all(grouped[t] and all(s['sr_entries'] == s['sr_copies'] == 1
                                                              for s in grouped[t]) for t in (3, 4)),
        'nr_reset_first_only': all(grouped[t] and all(s['reset'] == int(s['frame'] == 1)
                                                     for s in grouped[t]) for t in (3, 4)),
        'nr_hot_last_frame_two_pass': all(any(s['frame'] == 16 and s['nr_entries'] == 2 and
                                               s['sr_entries'] == s['sr_copies'] == 1 and
                                               s['completed'] == s['writeback'] == 1
                                               for s in grouped[t]) for t in (3, 4)),
        'sample_timings_valid': bool(samples) and all(math.isfinite(s[k]) and s[k] >= 0
                                                      for s in samples for k in ('cpu_ms', 'input_wait_ms')),
        'author_two_pass_success': bool(re.search(r'inline feature 18 evaluation succeeded[^\r\n]*2 stack pass\(es\)', nr)),
        'fixed_input_workset_stable': created == 1,
        'no_adapter_processing_failure': not failures,
    }
    pixel_reports = {}
    pixel_pattern = (r'^PIXEL_SAMPLE token=(\d+) bridge_frame=(\d+) host_frame=(\d+) image=(0x[0-9A-Fa-f]+) '
                     r'max_error=(\d+),(\d+),(\d+),(\d+) tolerance=(\d+)$')
    pixel_samples = [dict(token=int(m[0]), frame=int(m[1]), host_frame=int(m[2]), image=int(m[3], 16),
                          maximum=list(map(int, m[4:8])), tolerance=int(m[8]))
                     for m in re.findall(pixel_pattern, stdout, re.M)]
    checks['pixel_sample_schema'] = (len(pixel_samples) == stdout.count('PIXEL_SAMPLE ') and
                                     all(p['token'] in (1, 2) for p in pixel_samples))
    for token, label in ((1, 'COPY'), (2, 'CARRIER')):
        rows = re.findall(rf'^{label}_PIXELS checked=(\d+) mismatches=(\d+) '
                          r'max_error=(\d+),(\d+),(\d+),(\d+) tolerance=(\d+)$', stdout, re.M)
        p = [s for s in pixel_samples if s['token'] == token]
        tolerance = 0 if token == 1 else (2 if hdr else 1)
        checked, mismatches, *rest = map(int, rows[0]) if len(rows) == 1 else [0, 0, 0, 0, 0, 0, -1]
        maximum = rest[:4]
        measured = [max(s['maximum'][c] for s in p) for c in range(4)] if p else None
        bad = sum(any(v > tolerance for v in s['maximum'][:3]) or s['maximum'][3] != 0 for s in p)
        evidence = all(any(s['frame'] == v['frame'] and s['image'] == v['image'] and
                           s['completed'] == s['writeback'] == 1 and s['nr_entries'] == 0
                           for s in grouped[token]) for v in p)
        checks[f'{label.lower()}_pixels_checked'] = (
            len(rows) == 1 and checked == len(p) > 0 and mismatches == bad == 0 and
            len({s['frame'] for s in p}) == len(p) and len({s['host_frame'] for s in p}) == len(p) and
            maximum == measured and rest[4] == tolerance and
            all(s['tolerance'] == tolerance for s in p) and evidence)
        pixel_reports[label.lower()] = dict(checked=checked, mismatches=mismatches, max_error=maximum,
                                            tolerance=tolerance, samples=p)
    timings = {}
    for token, group in grouped.items():
        timings[token] = {}
        for key in ('cpu_ms', 'input_wait_ms'):
            values = [s[key] for s in group if math.isfinite(s[key])]
            if values:
                timings[token][key] = dict(count=len(values), minimum=min(values), maximum=max(values),
                                          mean=sum(values) / len(values))
    return {'checks': checks, 'created_worksets': created, 'accepted': accepted, 'summaries': summaries,
            'samples': samples, 'pixels': pixel_reports, 'sample_timings': timings,
            'pause_ms': scenario[3] if scenario else None, 'pause_presents': scenario[4] if scenario else None,
            'parse_errors': parse_errors, 'adapter_failures': failures, 'passed': all(checks.values()), 'scope': SCOPE}


def validate(directory: Path) -> dict:
    try:
        result = json.loads((directory / 'result.json').read_text(encoding='utf-8-sig'))
        nr = (directory / 'ReShade.log').read_text(encoding='utf-8-sig', errors='replace')
        bridge = (directory / 'dlss5-bridge.log').read_text(encoding='utf-8-sig', errors='replace')
    except (OSError, ValueError) as error:
        return {'checks': {'evidence_readable': False}, 'error': str(error), 'passed': False, 'scope': SCOPE}
    return validate_logs(result, nr, bridge)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    report = validate(args.directory)
    print(json.dumps(report, indent=2))
    raise SystemExit(0 if report['passed'] else 1)
