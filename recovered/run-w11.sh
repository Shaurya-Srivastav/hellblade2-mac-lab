#!/bin/zsh
# Direct launch of Hellblade II on the Wine 11 + D3DMetal 4 test runner. Extra env vars can be passed as arguments (KEY=VALUE)
W="/Users/shaurya/Library/Application Support/heroic/tools/game-porting-toolkit/.importing/W11-D3DM4/Contents/Resources/wine/bin"
G="/Users/shaurya/Documents/Games/Senuas.Saga.Hellblade.II.v1.0.0.0.162837/Senuas.Saga.Hellblade.II.v1.0.0.0.162837/Hellblade2/Binaries/Win64"
export WINEPREFIX="/Users/shaurya/Games/Heroic/Prefixes/default/Hellblade II (Wine 11 test)"
export WINEDEBUG=${WINEDEBUG:--all,err+module}
export D3DM_ENABLE_METALFX=1 ROSETTA_ADVERTISE_AVX=1 MTL_HUD_ENABLED=1
for kv in "$@"; do export "$kv"; done
cd "$G" && exec "$W/wine" Hellblade2-Win64-Shipping.exe
