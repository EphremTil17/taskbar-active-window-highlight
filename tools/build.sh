#!/usr/bin/env bash
# Compile a Windhawk mod with Windhawk's bundled toolchain, mirroring the flags
# the Windhawk editor uses, to catch compile/link errors before installing.
# Usage: tools/build.sh path/to/mod.wh.cpp [out-dir]
set -euo pipefail

src="$1"
out_dir="${2:-$(dirname "$src")/build}"
wh="/c/Program Files/Windhawk"
# windhawk.ini is UTF-16LE.
engine_ver="$(iconv -f UTF-16LE -t UTF-8 "$wh/windhawk.ini" | tr -d '\r\357\273\277' | sed -n 's/^EnginePath=Engine\\//p')"
clang="$wh/Compiler/bin/clang++.exe"
lib="$wh/Engine/$engine_ver/64/windhawk.lib"

mod_id="$(sed -n 's|^// @id[[:space:]]*||p' "$src" | head -1 | tr -d '\r')"
mod_ver="$(sed -n 's|^// @version[[:space:]]*||p' "$src" | head -1 | tr -d '\r')"
opts="$(sed -n 's|^// @compilerOptions[[:space:]]*||p' "$src" | head -1 | tr -d '\r')"

mkdir -p "$out_dir"
out="$out_dir/${mod_id}_${mod_ver}.dll"

# shellcheck disable=SC2086
"$clang" -std=c++23 -O2 -shared -target x86_64-w64-mingw32 \
    -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -D_WIN32_IE=0x0A00 \
    -DNTDDI_VERSION=0x0A000008 -D__USE_MINGW_ANSI_STDIO=0 \
    -DWH_MOD "-DWH_MOD_ID=L\"$mod_id\"" "-DWH_MOD_VERSION=L\"$mod_ver\"" \
    -include windhawk_api.h \
    -Wall -Wno-unused-function \
    "$src" "$lib" $opts -o "$out"
echo "OK: $out ($(stat -c %s "$out") bytes)"
