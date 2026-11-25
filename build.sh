#!/usr/bin/env bash
# 构建 Luma（需要 msys64/ucrt64 的 g++ 与 windres）
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="$ROOT/src"
DIST="$ROOT/dist"
TOOLS="$ROOT/tools"

mkdir -p "$DIST"

echo "[1/4] 编译图标生成器"
g++ -O2 -o "$TOOLS/mkicon.exe" "$TOOLS/mkicon.cpp" -lgdiplus -lgdi32 -luser32

echo "[2/4] 生成 app.ico"
( cd "$SRC" && "$TOOLS/mkicon.exe" )

echo "[3/4] 编译资源"
( cd "$SRC" && windres app.rc -O coff -o app.res )

echo "[4/4] 编译主程序"
g++ -std=c++17 -O2 -s -municode -mwindows \
    "$SRC/main.cpp" "$SRC/brightness.cpp" "$SRC/app.res" \
    -o "$DIST/Luma.exe" \
    -ldwmapi -ld2d1 -ldwrite -ld3d11 -ldxgi -ldcomp -ldxva2 \
    -lshell32 -lole32 -loleaut32 -lwbemuuid -ladvapi32 -lgdi32 -luser32 \
    -static

ls -la "$DIST/Luma.exe"
