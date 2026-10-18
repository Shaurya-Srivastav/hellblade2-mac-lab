#!/usr/bin/env python3
"""A/B/A render-cost ablations on the running game.

Each experiment writes console-variable values straight into the game's memory
(cvarpeek --write), waits for them to settle, measures the per-drawable trace and
restores the baseline, which is measured again between experiments.
"""
import json, os, re, shutil, subprocess, sys, tempfile, time

WINRUN = os.path.expanduser('~/hb2lab/winrun.sh')
PROBE = os.path.expanduser('~/hb2lab/probe')
RUNNER = 'Game-Porting-Toolkit-3.0-3-fg'
GAME = 'Hellblade2-Win64-Shipping.exe'

CVARS = {  # name: value type
    'r.ScreenPercentage': 'f32',
    'r.Lumen.DiffuseIndirect.Allow': 'i32',
    'r.Lumen.Reflections.Allow': 'i32',
    'r.Lumen.ScreenProbeGather.DownsampleFactor': 'i32',
    'r.Shadow.Virtual.ResolutionLodBiasDirectional': 'f32',
    'r.VolumetricFog': 'i32',
    'r.Nanite.MaxPixelsPerEdge': 'f32',
    'r.DepthOfFieldQuality': 'i32',
    'r.MotionBlurQuality': 'i32',
    'r.Lumen.TranslucencyVolume.Enable': 'i32',
}

EXPERIMENTS = {
    'sp58-balanced': {'r.ScreenPercentage': 58},
    'sp50-performance': {'r.ScreenPercentage': 50},
    'sp33-ultraperf': {'r.ScreenPercentage': 33.3},
    'lumen-gi-off': {'r.Lumen.DiffuseIndirect.Allow': 0},
    'lumen-refl-off': {'r.Lumen.Reflections.Allow': 0},
    'probe-downsample-32': {'r.Lumen.ScreenProbeGather.DownsampleFactor': 32},
    'vsm-bias-0.5': {'r.Shadow.Virtual.ResolutionLodBiasDirectional': 0.5},
    'volumetric-fog-off': {'r.VolumetricFog': 0},
    'nanite-ppe-2': {'r.Nanite.MaxPixelsPerEdge': 2},
    'dof-off': {'r.DepthOfFieldQuality': 0},
    'motion-blur-off': {'r.MotionBlurQuality': 0},
    'lumen-translucency-off': {'r.Lumen.TranslucencyVolume.Enable': 0},
}

LINE = re.compile(r'CVAR (\S+) object=([0-9a-fA-F]+) setby=\S+(?: ref\+48=([0-9a-fA-F]+)\(int=(-?\d+) float=(\S+)\))?'
                  r'.*? shadow48=(-?\d+)/(\S+)')

targets = {}  # name: (addresses, type, baseline value)


def resolve():
    out = subprocess.run([WINRUN, RUNNER, './cvarpeek.exe', GAME, '--brief', *CVARS], cwd=PROBE,
                         capture_output=True, text=True, timeout=180).stdout
    for m in LINE.finditer(out):
        name, obj, ref, ref_int, ref_float, sh_int, sh_float = m.groups()
        if name in targets:
            continue
        kind = CVARS[name]
        if ref:  # FAutoConsoleVariableRef: the engine reads the global it points at
            value = int(ref_int) if kind == 'i32' else float(ref_float)
            targets[name] = ([int(ref, 16)], kind, value)
        else:  # TAutoConsoleVariable: game- and render-thread shadow copies
            value = int(sh_int) if kind == 'i32' else float(sh_float)
            targets[name] = ([int(obj, 16) + 0x48, int(obj, 16) + 0x4c], kind, value)
    missing = set(CVARS) - set(targets)
    if missing:
        print(f'not in this build, skipping their experiments: {sorted(missing)}', flush=True)
    if 'r.ScreenPercentage' not in targets:
        raise SystemExit(f'could not resolve r.ScreenPercentage:\n{out}')
    for name, (addrs, kind, value) in targets.items():
        print(f'resolved {name} = {value} at {[hex(a) for a in addrs]}', flush=True)


def values(settings):
    return [(addr, targets[name][1], value) for name, value in settings.items() for addr in targets[name][0]]


def base():
    return values({name: t[2] for name, t in targets.items()})


def write(values):
    args = [WINRUN, RUNNER, './cvarpeek.exe', GAME, '--write']
    for addr, kind, value in values:
        args += [f'{addr:x}', kind, str(value)]
    out = subprocess.run(args, cwd=PROBE, capture_output=True, text=True, timeout=60).stdout
    if 'failed' in out or 'refused' in out or out.count('WRITE') != len(values):
        raise SystemExit(f'write failed:\n{out}')


def measure(seconds):
    end = int(time.time())
    out = tempfile.mkdtemp(prefix='hb2abl')
    try:
        subprocess.run(['metalperftrace', 'collect', '--start', f'@{end - seconds}', '--end', f'@{end}', out],
                       check=True, capture_output=True)
        trace = next(os.path.join(out, f) for f in os.listdir(out) if f.endswith('.atrc'))
        data = json.loads(subprocess.run(['metalperftrace', 'overview', '--json', '--json-include-timeline', trace],
                                         check=True, capture_output=True, text=True).stdout)
    finally:
        shutil.rmtree(out, ignore_errors=True)
    best = None
    for proc in data if isinstance(data, list) else []:
        if not isinstance(proc, dict) or not proc.get('Process', '').startswith('wine'):
            continue
        for layer in proc.get('Layers', []):
            rows = layer.get('On-Glass Drawable Timeline') or []
            if not best or len(rows) > len(best):
                best = rows
    if not best or len(best) < 10:
        return None
    gaps = [b['Start MCT Seconds'] - a['End MCT Seconds'] for a, b in zip(best, best[1:])]
    span = best[-1]['End MCT Seconds'] - best[0]['Start MCT Seconds']
    if max(gaps, default=0) > 0.5 or span < seconds * 0.8:
        return None
    info = [r['In-Flight Drawable Info'] for r in best]
    real = [i for i in info if i.get('MetalFX Enabled')]
    gpu = sum(i.get('On-GPU Time (seconds)', 0) for i in info) * 1000
    widths = sorted({(i.get('MetalFX Input Width'), i.get('MetalFX Input Height')) for i in real})
    return {
        'shown_fps': len(info) / span if span else 0,
        'real_fps': len(real) / span if span else 0,
        'gpu_ms_per_real': gpu / len(real) if real else 0,
        'gpu_busy': gpu / 1000 / span if span else 0,
        'interp': sum(1 for i in info if i.get('MetalFX Frame Interpolator Enabled')),
        'input': widths,
        'frames': len(info),
    }


def show(label, m):
    if not m:
        print(f'{label:22s} no frames (game not rendering)', flush=True)
        return
    print(f"{label:22s} real_fps={m['real_fps']:5.1f} shown_fps={m['shown_fps']:5.1f} "
          f"gpu_ms/real={m['gpu_ms_per_real']:5.1f} busy={m['gpu_busy']:.2f} input={m['input']}", flush=True)


def wait_for_rendering(gameplay=False):
    """gameplay: also wait for interpolated frames, which the game only generates in gameplay"""
    announced = False
    while True:
        pids = subprocess.run(['pgrep', '-f', GAME], capture_output=True, text=True).stdout.split()
        for pid in pids:
            subprocess.run(['metalperftrace', 'setup', '--enable', 'per-frame-metrics', '--pid', pid], capture_output=True)
        m = measure(3)
        if m and (not gameplay or m['interp']):
            return
        if not announced:
            print('waiting for gameplay with frame generation' if gameplay else 'waiting for the game to render', flush=True)
            announced = True
        time.sleep(3)


def main():
    names = sys.argv[1:] or list(EXPERIMENTS)
    settle, window = 3, 5
    results = {}
    while not subprocess.run(['pgrep', '-f', GAME], capture_output=True).stdout:
        time.sleep(5)
    pid = subprocess.run(['pgrep', '-f', GAME], capture_output=True, text=True).stdout.split()[0]
    subprocess.run(['metalperftrace', 'setup', '--enable', 'per-frame-metrics', '--pid', pid], capture_output=True)
    wait_for_rendering(gameplay=True)
    resolve()
    try:
        for name in names:
            if not set(EXPERIMENTS[name]) <= set(targets):
                continue
            while True:
                wait_for_rendering()
                time.sleep(window)
                baseline = measure(window)
                write(values(EXPERIMENTS[name]))
                time.sleep(settle + window)
                m = measure(window)
                write(base())
                time.sleep(settle)
                if baseline and m:
                    break
                print(f'{name}: rendering stopped during the measurement, repeating', flush=True)
            show('  baseline', baseline)
            show(name, m)
            results[name] = (baseline, m)
            json.dump(results, open(os.path.expanduser('~/hb2lab/out/ablate.json'), 'w'), indent=1, default=str)
        print('DONE', flush=True)
    finally:
        if targets:
            write(base())


if __name__ == '__main__':
    main()
