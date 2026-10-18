#!/bin/zsh
# usage: winrun.sh <runner folder name> <windows exe> [args...]  -- runs a program in Hellblade's prefix like the game is run
R="/Users/shaurya/Library/Application Support/heroic/tools/game-porting-toolkit/$1/Contents/Resources/wine"
shift
export WINEPREFIX="/Users/shaurya/Games/Heroic/Prefixes/default/Senuas saga hellblade II"
export D3DM_ENABLE_METALFX=1 WINEMSYNC=1 ROSETTA_ADVERTISE_AVX=1 WINEDEBUG=${WINEDEBUG:--all}
exec "$R/bin/wine64" "$@"
