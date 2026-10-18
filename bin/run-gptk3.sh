#!/bin/zsh
# usage: run-gptk3.sh <runner folder name> [KEY=VALUE ...] [-- game args ...]
R="/Users/shaurya/Library/Application Support/heroic/tools/game-porting-toolkit/$1/Contents/Resources/wine"
shift
export WINEPREFIX="/Users/shaurya/Games/Heroic/Prefixes/default/Senuas saga hellblade II"
export D3DM_ENABLE_METALFX=1 WINEMSYNC=1 ROSETTA_ADVERTISE_AVX=1 MTL_HUD_ENABLED=1 WINEDEBUG=-all
while [ $# -gt 0 ] && [ "$1" != "--" ]; do export "$1"; shift; done
[ "$1" = "--" ] && shift
cd "/Users/shaurya/Documents/Games/Senuas.Saga.Hellblade.II.v1.0.0.0.162837/Senuas.Saga.Hellblade.II.v1.0.0.0.162837"
exec "$R/bin/wine64" Hellblade2.exe "$@"
