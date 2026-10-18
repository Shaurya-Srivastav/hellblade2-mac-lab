#!/bin/zsh
# usage: menu-hud.sh <label> <run-gptk3.sh args...>  -- restarts the game, screenshots the Metal HUD at the main menu
label=$1; shift
P="/Users/shaurya/Games/Heroic/Prefixes/default/Senuas saga hellblade II"
WINEPREFIX="$P" "/Users/shaurya/Library/Application Support/heroic/tools/game-porting-toolkit/Game-Porting-Toolkit-3.0-3/Contents/Resources/wine/bin/wineserver" -k
sleep 4
nohup /tmp/hb2exp/run-gptk3.sh "$@" > /tmp/hb2exp/run-$label.log 2>&1 &
sleep 20
for i in $(seq 1 40); do
  out=$(python3 /tmp/hb2exp/perf.py 3 poll 2>&1 | tail -1)
  t=$(echo "$out" | rg -o 'metalfx_temporal=([0-9]+)' -r '$1')
  [ -n "$t" ] && [ "$t" -gt 20 ] && break
  sleep 3
done
sleep 4
screencapture -x -t jpg /tmp/hb2exp/$label.jpg
sips -c 1440 1700 --cropOffset 0 3420 /tmp/hb2exp/$label.jpg --out /tmp/hb2exp/$label-hud.jpg >/dev/null
sips -Z 700 /tmp/hb2exp/$label-hud.jpg >/dev/null
python3 /tmp/hb2exp/perf.py 5 $label 2>&1 | tail -1
