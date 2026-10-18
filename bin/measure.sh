#!/bin/zsh
# usage: measure.sh <label>  -- waits for gameplay, then records frame stats, per-thread CPU and a screenshot
label=$1
for i in $(seq 1 100); do
  out=$(python3 /Users/shaurya/hb2lab/bin/perf.py 3 poll 2>&1 | tail -1)
  g=$(echo "$out" | rg -o 'gpu_ms=([0-9.]+)' -r '$1')
  [ -n "$g" ] && [ "${g%.*}" -gt 12 ] && break
  sleep 3
done
echo "$(date +%T) gameplay detected"
sleep 12
pid=$(ps -axo pid,rss,comm | rg wine64-preloader | sort -k2 -nr | head -1 | awk '{print $1}')
screencapture -x -t jpg /Users/shaurya/hb2lab/out/$label.jpg
ps -M -p $pid | awk 'NR>1 {print $(NF-6)}' | sort -nr | head -12 | tr '\n' ' ' > /Users/shaurya/hb2lab/out/$label.threads
sleep 3
python3 /Users/shaurya/hb2lab/bin/perf.py 15 $label 2>&1 | tail -1
echo "top thread CPU%: $(cat /Users/shaurya/hb2lab/out/$label.threads)"
echo "total CPU%: $(ps -o %cpu= -p $pid)"
