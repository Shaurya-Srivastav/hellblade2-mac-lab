#!/usr/bin/env python3
"""usage: perf.py <seconds back> [label]  -- summarizes the game's presented frames over that window"""
import json, os, shutil, subprocess, sys, tempfile, time

seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 20
label = sys.argv[2] if len(sys.argv) > 2 else ''
end = int(time.time())
start = end - seconds
out = tempfile.mkdtemp(prefix='hb2perf')
subprocess.run(['metalperftrace', 'collect', '--start', f'@{start}', '--end', f'@{end}', out],
               check=True, capture_output=True)
trace = next(os.path.join(out, f) for f in os.listdir(out) if f.endswith('.atrc'))
res = subprocess.run(['metalperftrace', 'overview', '--json', '--json-include-timeline', trace],
                     check=True, capture_output=True, text=True)
data = json.loads(res.stdout)
shutil.rmtree(out, ignore_errors=True)

procs = [p for p in data if p.get('Process', '').startswith('wine')]
best = None
for proc in procs:
    for layer in proc.get('Layers', []):
        s = layer['Total Session Stats']['Presented Frame Stats']
        if not best or s['Frame Count'] > best[1]['Frame Count']:
            best = (layer, s)
if not best:
    print(label, 'no wine frames in window'); sys.exit()
layer, s = best
tl = layer['Stats Timeline']
active = [iv['Presented Frame Stats'] for iv in tl if iv['Presented Frame Stats']['Frame Count']]
frames = sum(p['Frame Count'] for p in active)
secs = sum(iv['Time Active (sec)'] for iv in tl if iv['Presented Frame Stats']['Frame Count'])
gpu = [p['On-GPU Walltime Stats']['Average (ms)'] for p in active]
interp = sum(p.get('MetalFX Frame Interpolator Enabled Frames', 0) for p in active)
temporal = sum(p.get('Metal FX Temporal Scaling Frames', 0) for p in active)
ratio = [p['Metal FX Scaling Ratio Stats'].get('Average', 0) for p in active if isinstance(p.get('Metal FX Scaling Ratio Stats'), dict)]
g = [p.get('Game Porting Toolkit', {}) for p in active]
def avg(key):
    vals = [x[key]['Average'] for x in g if key in x and x[key].get('Count')]
    return sum(vals) / len(vals) if vals else 0
print(f"{label} window={seconds}s active={secs:.0f}s fps={frames / secs if secs else 0:.1f} "
      f"gpu_ms={sum(gpu) / len(gpu) if gpu else 0:.1f} (min {min(gpu) if gpu else 0:.1f} max {max(gpu) if gpu else 0:.1f}) "
      f"interpolated={interp}/{frames} metalfx_temporal={temporal} scale_area={sum(ratio) / len(ratio) if ratio else 0:.2f} "
      f"draws={avg('GPTk Fragment Draw Count'):.0f} execIndirect={avg('GPTk ExecuteIndirect Count'):.0f} "
      f"dispatch={avg('GPTk Dispatch Count'):.0f} cmdbufs={avg('GPTk Command Buffer Count'):.0f}")
