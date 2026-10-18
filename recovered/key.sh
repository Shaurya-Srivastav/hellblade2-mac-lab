#!/bin/zsh
# usage: key.sh <key code> [count] [delay]   (36 = Return, 53 = Escape, 49 = Space, 125 = Down, 126 = Up)
pid=$(pgrep -f 'Hellblade2-Win64-Shipping.exe' | head -1)
[ -z "$pid" ] && { echo "game not running"; exit 1; }
count=${2:-1}
delay=${3:-0.6}
osascript <<EOF
tell application "System Events"
  set gameProcs to (every process whose unix id is $pid)
  if (count of gameProcs) > 0 then set frontmost of (item 1 of gameProcs) to true
  delay 0.4
  repeat $count times
    key code $1
    delay $delay
  end repeat
  return name of first process whose frontmost is true
end tell
EOF
