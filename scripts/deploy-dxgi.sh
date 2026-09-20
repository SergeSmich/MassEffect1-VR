#!/bin/sh
# Deploy: the mod is a single dxgi.dll in the game's Win64 folder. NOT an .asi.
# The build already outputs dxgi.dll (TargetName=dxgi). Copy it next to MassEffect1.exe.
#
# Usage: deploy-dxgi.sh "<path to ...\Game\ME1\Binaries\Win64>"
#    or: MELEVR_WIN64="<path>" deploy-dxgi.sh
WIN64="${1:-$MELEVR_WIN64}"
[ -n "$WIN64" ] || { echo "usage: $0 <path to Game/ME1/Binaries/Win64>  (or set MELEVR_WIN64)"; exit 1; }
[ -d "$WIN64" ] || { echo "not a directory: $WIN64"; exit 1; }
SRC="$(dirname "$0")/../builds/dxgi.dll"
[ -f "$SRC" ] || { echo "build first (Release|x64) -> $SRC missing"; exit 1; }
cp "$SRC" "$WIN64/dxgi.dll" && echo "deployed dxgi.dll: $(sha256sum "$WIN64/dxgi.dll" | cut -c1-16)"
# Never deploy as MELEVR.asi - that causes a two-copy load conflict.
