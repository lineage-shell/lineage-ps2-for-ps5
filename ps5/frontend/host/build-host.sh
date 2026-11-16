#!/usr/bin/env bash
# PS5 port frontend: builds fe_host (the shelf on a PC, fe_host.cpp) next to this script.
#
#   ps5/frontend/host/build-host.sh
#   VK_ICD_FILENAMES=/opt/pw-browsers/chromium-1194/chrome-linux/vk_swiftshader_icd.json \
#     ps5/frontend/host/fe_host --data /tmp/fe-data --out /tmp/fe-shots "wait 1" "shot shelf"
#
# Needs clang++ (18 or later), zlib's headers and the machine's libvulkan.so.1; SwiftShader (from a Chromium build)
# stands in for a GPU.
#
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fe=$(cd -- "$here/.." && pwd)
pcsx2=$(cd -- "$fe/../.." && pwd)
obj=${FE_HOST_OBJ:-$here/obj}
CXX=${CXX:-clang++-18}
CC=${CC:-clang-18}
mkdir -p "$obj"
# laptop-preview: the compiles run side by side (FE_HOST_JOBS at a time, default: every core); a failed one fails the build before the link.
jobs_max=${FE_HOST_JOBS:-$(nproc 2>/dev/null || echo 4)}
rm -f "$obj/.failed"
spawn() {
  while (($(jobs -rp | wc -l) >= jobs_max)); do wait -n || true; done
  { "$@" || touch "$obj/.failed"; } &
}
finish() {
  wait
  if [[ -f $obj/.failed ]]; then
    echo "build-host.sh: a compile failed" >&2
    exit 1
  fi
}

flags=(-O1 -g -Wall -Wno-unused-function -I"$fe" -I"$pcsx2/pcsx2" -I"$pcsx2/3rdparty/vulkan/include" -I"$pcsx2/3rdparty/libchdr/include"
  -I"$pcsx2/ps5/third_party/lz4" -I"$pcsx2/3rdparty/rapidjson/include" -I"$pcsx2/ps5/third_party/libarchive/libarchive")
objs=()
for src in fe_app fe_covers fe_games fe_renderer fe_text fe_vk fe_i18n fe_settings fe_options fe_sound fe_texpacks fe_texwrite fe_patchdl fe_bios fe_update fe_ps2; do
  o="$obj/$src.o"
  if [[ ! -f $o || $fe/$src.cpp -nt $o || -n $(find "$fe" -maxdepth 1 -name '*.h' -newer "$o" -print -quit) ||
    -n $(find "$fe/shelf3" -maxdepth 1 \( -name '*.h' -o -name '*.inc' \) -newer "$o" -print -quit) ]]; then # shelf v3: its .h/.inc too
    spawn "$CXX" -std=c++20 "${flags[@]}" -c "$fe/$src.cpp" -o "$o"
  fi
  objs+=("$o")
done
# shelf v3 (AI-assisted): its own files (shelf3/).
for src in s3_bios s3_kit s3_memcard s3_seq; do
  o="$obj/$src.o"
  if [[ ! -f $o || $fe/shelf3/$src.cpp -nt $o || -n $(find "$fe" "$fe/shelf3" -maxdepth 1 -name '*.h' -newer "$o" -print -quit) ]]; then
    spawn "$CXX" -std=c++20 "${flags[@]}" -c "$fe/shelf3/$src.cpp" -o "$o"
  fi
  objs+=("$o")
done
spawn "$CC" -O1 -I"$fe/third_party/qrcodegen" -c "$fe/third_party/qrcodegen/qrcodegen.c" -o "$obj/qrcodegen.o"
spawn "$CC" -O1 -c "$pcsx2/ps5/third_party/lz4/lz4.c" -o "$obj/lz4.o"
spawn "$CC" -O1 -I"$pcsx2/3rdparty/libchdr/include" -c "$here/chd_stub.c" -o "$obj/chd_stub.o"
# 2026-10-05: the vendored libarchive (its Linux config), for the texture packs (fe_texpacks.cpp).
la="$pcsx2/ps5/third_party/libarchive"
la_objs=()
for c in "$la"/libarchive/*.c; do
  o="$obj/la_$(basename "$c" .c).o"
  [[ -f $o && ! $c -nt $o ]] || spawn "$CC" -O1 -w -DHAVE_CONFIG_H -I"$la" -I"$la/libarchive" -c "$c" -o "$o"
  la_objs+=("$o")
done
# b176: the 7z reader (fe_sevenzip.c) and the LZMA SDK files it needs (14: this harness links chd_stub.c, not libchdr, so LzmaDec.c is built here too).
sdk="$pcsx2/3rdparty/lzma"
sz_objs=()
for s in 7zArcIn 7zBuf 7zCrc 7zCrcOpt 7zDec 7zStream Bcj2 Bra Bra86 BraIA64 CpuArch Delta Lzma2Dec LzmaDec; do
  o="$obj/sz_$s.o"
  [[ -f $o && ! $sdk/src/$s.c -nt $o ]] || spawn "$CC" -O1 -w -DZ7_ST -I"$sdk/include" -c "$sdk/src/$s.c" -o "$o"
  sz_objs+=("$o")
done
spawn "$CC" -O1 -Wall -Wextra -Wno-unused-parameter -DZ7_ST -I"$sdk/include" -c "$fe/fe_sevenzip.c" -o "$obj/fe_sevenzip.o"
sz_objs+=("$obj/fe_sevenzip.o")
# laptop-preview: FE_HOST_WINDOW=1 adds --window (host_window.h: an SDL3 window, controller and sound); FE_HOST_OUT names the binary
# (default: fe_host next to this script). Without it the build is as before (no SDL3 needed).
win_flags=()
win_libs=()
if [[ ${FE_HOST_WINDOW:-0} == 1 ]]; then
  win_flags=(-DFE_HOST_WINDOW $(pkg-config --cflags sdl3 2>/dev/null))
  win_libs=($(pkg-config --libs sdl3 2>/dev/null || echo -lSDL3))
fi
out=${FE_HOST_OUT:-$here/fe_host}
spawn "$CXX" -std=c++20 "${flags[@]}" "${win_flags[@]}" -c "$here/fe_host.cpp" -o "$obj/fe_host.o"
finish
"$CXX" -o "$out" "$obj/fe_host.o" "${objs[@]}" "$obj/qrcodegen.o" "$obj/lz4.o" "$obj/chd_stub.o" "${la_objs[@]}" "${sz_objs[@]}" -lz -ldl -lpthread "${win_libs[@]}"
echo "built $out"
