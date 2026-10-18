#!/bin/zsh
# usage: try-w11.sh <label> [KEY=VALUE ...] : launches the Wine 11 test, kills it on crash, reports how far it got
label=$1; shift
C="/Users/shaurya/Games/Heroic/Prefixes/default/Hellblade II (Wine 11 test)/drive_c/users/crossover/AppData/Local/Hellblade2/Saved/Crashes"
W="/Users/shaurya/Library/Application Support/heroic/tools/game-porting-toolkit/.importing/W11-D3DM4/Contents/Resources/wine/bin"
export WINEPREFIX="/Users/shaurya/Games/Heroic/Prefixes/default/Hellblade II (Wine 11 test)"
mkdir -p /tmp/hb2exp/crashes "$C"
find "$C" -mindepth 1 -maxdepth 1 -exec mv {} /tmp/hb2exp/crashes/ \;
WINEDEBUG=${WINEDEBUG:-err+all,+seh} nohup /tmp/hb2exp/run-w11.sh "$@" > /tmp/hb2exp/w11-$label.log 2>&1 &
result="still running after 60s"
for i in $(seq 1 30); do
  sleep 2
  if [ -n "$(ls -A "$C")" ]; then result="crashed after $((i*2))s"; break; fi
  pgrep -f Hellblade2-Win64 >/dev/null || { result="exited after $((i*2))s"; break; }
done
echo "$label: $result"
if [ "$result" != "still running after 60s" ] || [ -n "$KILL" ]; then sleep 1; "$W/wineserver" -k; fi
rg 'c0000005' -m3 /tmp/hb2exp/w11-$label.log | cut -c1-160
