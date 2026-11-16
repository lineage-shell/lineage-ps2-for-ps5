#!/usr/bin/env bash
# laptop-preview (AI-assisted): the PS2 shelf in a window on a Linux PC (keyboard, DualShock 4 / DualSense, sound), in any version.
#
#   preview.sh                     v2: ps2-shelf-v2, every P2 change
#   preview.sh current             ps2-console-shelf as it is (aafb081)
#   preview.sh --with P2-01,P2-07  ps2-console-shelf plus only those changes (cherry-picked in v2's order)
#   preview.sh --choose            a dialog: current, v2, or tick the changes (the desktop launcher uses this)
#   (real fronts: fetch_covers.py, from xlenore/ps2-covers into ~/.local/share/ps2-shelf-preview/real-covers, local only)
#   options: --fullscreen  --rebuild  --build-only  --list (the P2 changes)  --clean (drop every cached build)  -- <more fe_host arguments>
#
# Each version gets a sparse worktree of the PS5SX2 repo and its own fe_host, cached in ~/.cache/ps2-shelf-preview/<key> and only rebuilt
# when its commits or this tooling change. The tooling (fe_host.cpp, host_window.h, build-host.sh) is laid over every tree from the folder
# this script is in, so all versions get the same window, keys and sound; the shelf's own code is the version's. Nothing goes to a PS5.
#
# Environment: PS5SX2_REPO (the repo; default: the one this script is in), PS2_SHELF_SOUNDS (default ~/ps5-work/sounds/ps2: the console's
# /data/PCSX2/console-sounds), PS2_SHELF_DATA (the stand-in /data/PCSX2; default ~/.local/share/ps2-shelf-preview/data).
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
host=$(cd -- "$here/.." && pwd)
repo=${PS5SX2_REPO:-$(git -C "$here" rev-parse --show-toplevel 2>/dev/null || (cd "$here/../../../.." && pwd))}
cache=${XDG_CACHE_HOME:-$HOME/.cache}/ps2-shelf-preview
data=${PS2_SHELF_DATA:-${XDG_DATA_HOME:-$HOME/.local/share}/ps2-shelf-preview/data}
sounds=${PS2_SHELF_SOUNDS:-$HOME/ps5-work/sounds/ps2}
shots=${XDG_PICTURES_DIR:-$HOME/Pictures}/ps2-shelf-preview
base_ref=ps2-console-shelf
v2_ref=ps2-shelf-v2

gui=0
mode=v2
with=""
fullscreen=()
rebuild=0
build_only=0
extra=()

say() { echo "[preview] $*"; }
die() {
  echo "[preview] $*" >&2
  if [[ $gui == 1 ]] && command -v zenity >/dev/null; then zenity --error --title "PS2 Shelf Preview" --text "$*" 2>/dev/null || true; fi
  exit 1
}

ref_sha() {
  git -C "$repo" rev-parse --verify -q "$1^{commit}" 2>/dev/null || git -C "$repo" rev-parse --verify -q "origin/$1^{commit}" 2>/dev/null ||
    die "no branch $1 in $repo (git fetch origin $1?)"
}

# "P2-01 <sha> <subject>" for every change on v2, oldest first.
list_changes() {
  local base v2
  base=$(ref_sha $base_ref)
  v2=$(ref_sha $v2_ref)
  git -C "$repo" log --reverse --format='%H %s' "$base..$v2" | while read -r sha subject; do
    [[ $subject =~ ^(P2-[0-9][0-9]):\ (.*)$ ]] && echo "${BASH_REMATCH[1]} $sha ${BASH_REMATCH[2]% (AI-assisted)}"
  done
}

choose() {
  command -v zenity >/dev/null || die "zenity is missing: run preview.sh current|v2|--with ... from a terminal"
  local pick
  pick=$(zenity --list --radiolist --title "PS2 Shelf Preview" --text "Which shelf?" --width 520 --height 300 \
    --column "" --column "Version" --column "What" \
    TRUE v2 "v2: all the P2 changes (ps2-shelf-v2)" \
    FALSE current "current: ps2-console-shelf as it is" \
    FALSE pick "pick P2 changes..." 2>/dev/null) || exit 0
  case $pick in
    current | v2) mode=$pick ;;
    pick)
      local rows=() id sha subject
      while read -r id sha subject; do rows+=(FALSE "$id" "$subject"); done < <(list_changes | sort)
      with=$(zenity --list --checklist --title "PS2 Shelf Preview" --text "Changes on top of the current shelf" --width 900 --height 520 \
        --separator , --column "" --column "ID" --column "Change" "${rows[@]}" 2>/dev/null) || exit 0
      mode=with
      [[ -n $with ]] || mode=current
      ;;
    *) exit 0 ;;
  esac
}

while (($#)); do
  case $1 in
    current | v2) mode=$1 ;;
    --with) mode=with; with=${2:-}; shift ;;
    --with=*) mode=with; with=${1#--with=} ;;
    --choose) gui=1; choose ;;
    --gui) gui=1 ;;
    --fullscreen) fullscreen=(--fullscreen) ;;
    --rebuild) rebuild=1 ;;
    --list) list_changes | sort | cut -d' ' -f1,3-; exit 0 ;;
    --build-only) build_only=1 ;;
    --clean)
      for t in "$cache"/*/tree; do [[ -d $t ]] && git -C "$repo" worktree remove --force "$t" 2>/dev/null || true; done
      rm -rf "$cache"; git -C "$repo" worktree prune; say "cache removed"; exit 0 ;;
    --) shift; extra=("$@"); break ;;
    -h | --help) sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown argument: $1 (try --help)" ;;
  esac
  shift
done

# The version: its base commit, the commits to pick on top, a cache key and a label.
picks=()
case $mode in
  current)
    start=$(ref_sha $base_ref); key="current-${start:0:8}"; label="current (ps2-console-shelf)" ;;
  v2)
    start=$(ref_sha $v2_ref); key="v2-${start:0:8}"; label="v2 (all changes)" ;;
  with)
    start=$(ref_sha $base_ref)
    want=$(tr ',' '\n' <<<"${with^^}" | sed 's/^ *//; s/ *$//; s/^\([0-9]\)/P2-\1/; s/^P2-\([0-9]\)$/P2-0\1/' | grep -v '^$' | sort -u)
    ids=()
    while read -r id sha subject; do
      if grep -qx "$id" <<<"$want"; then picks+=("$sha"); ids+=("$id"); fi
    done < <(list_changes)
    for w in $want; do printf '%s\n' "${ids[@]}" | grep -qx "$w" || die "no change $w on $v2_ref (preview.sh --list)"; done
    ((${#picks[@]})) || die "--with needs P2 IDs, e.g. --with P2-01,P2-07"
    joined=$(printf '%s\n' "${ids[@]}" | sort | paste -sd+)
    key="with-$joined-${start:0:8}-$(git -C "$repo" rev-parse --short=8 "$(ref_sha $v2_ref)")"
    label="current + ${joined//+/, }" ;;
esac

dir=$cache/$key
tree=$dir/tree
bin=$dir/fe_host
log=$dir/build.log
mkdir -p "$dir"

progress_pid=""
progress() {
  say "$1"
  if [[ $gui == 1 && -z $progress_pid ]] && command -v zenity >/dev/null; then
    (sleep 0.3; zenity --progress --pulsate --no-cancel --auto-close --title "PS2 Shelf Preview" --text "$1 (first time: about 2 minutes)" 2>/dev/null < <(sleep 100000)) &
    progress_pid=$!
  fi
}
stop_progress() {
  if [[ -n $progress_pid ]]; then
    pkill -P "$progress_pid" 2>/dev/null || true
    kill "$progress_pid" 2>/dev/null || true
    progress_pid=""
  fi
}
trap stop_progress EXIT

# 1. The tree (sparse: only what fe_host needs).
if [[ ! -f $tree/.preview-ready ]]; then
  progress "Preparing $label"
  [[ -d $tree ]] && { git -C "$repo" worktree remove --force "$tree" 2>/dev/null || rm -rf "$tree"; git -C "$repo" worktree prune; }
  git -C "$repo" worktree add --no-checkout --detach "$tree" "$start" >>"$log" 2>&1 || die "git worktree add failed (see $log)"
  git -C "$tree" sparse-checkout set --cone ps5 pcsx2 bin/resources 3rdparty/vulkan 3rdparty/libchdr 3rdparty/rapidjson 3rdparty/lzma >>"$log" 2>&1
  git -C "$tree" checkout -q --detach "$start" >>"$log" 2>&1 || die "checkout failed (see $log)"
  for sha in "${picks[@]}"; do
    if ! git -C "$tree" -c user.name=preview -c user.email=preview@localhost cherry-pick --allow-empty "$sha" >>"$log" 2>&1; then
      git -C "$tree" cherry-pick --abort >/dev/null 2>&1 || true
      die "$(git -C "$repo" log -1 --format=%s "$sha" | cut -c1-5) does not apply with the changes before it ($log). Try another set."
    fi
  done
  touch "$tree/.preview-ready"
fi

# 2. The tooling over it (only files that differ, so make-style times stay right).
for f in fe_host.cpp host_window.h build-host.sh; do
  cmp -s "$host/$f" "$tree/ps5/frontend/host/$f" || cp "$host/$f" "$tree/ps5/frontend/host/$f"
done

# 3. Build when needed (g++ when there is no clang++-18).
need=$rebuild
[[ -x $bin ]] || need=1
for f in fe_host.cpp host_window.h build-host.sh; do [[ $tree/ps5/frontend/host/$f -nt $bin ]] && need=1; done
if [[ $need == 1 ]]; then
  progress "Building $label"
  cxx=${CXX:-$(command -v clang++-18 || command -v clang++ || command -v g++)}
  cc=${CC:-$(command -v clang-18 || command -v clang || command -v gcc)}
  if ! FE_HOST_WINDOW=1 FE_HOST_OUT="$bin" CXX=$cxx CC=$cc "$tree/ps5/frontend/host/build-host.sh" >"$log" 2>&1; then
    tail -20 "$log" >&2
    die "the build of $label failed (see $log)"
  fi
  say "built $bin"
fi
stop_progress
[[ $build_only == 1 ]] && exit 0

# 4. The stand-in /data/PCSX2: the sample library's painted covers, the console sounds.
mkdir -p "$data/covers" "$shots"
disc="SLES-53777|Prince of Persia - The Two Thrones" # PAL, like the library
painted=$(cat "$here/games.txt" "$here/make_covers.py" | sha1sum | cut -c1-12) # repaint (missing files only) when the library changes
if [[ $(cat "$data/covers/.painted" 2>/dev/null) != "$painted" ]] && python3 -c 'import PIL' 2>/dev/null; then
  python3 -I "$here/make_covers.py" "$here/games.txt" "$data/covers" "$tree/bin/resources/fonts/Roboto-Regular.ttf" "$disc" && echo "$painted" >"$data/covers/.painted"
fi
# Real fronts over the painted ones (xlenore/ps2-covers, fetched once and kept beside the data; PS2_SHELF_REAL_COVERS=0 to skip).
if [[ ${PS2_SHELF_REAL_COVERS:-1} == 1 ]] && python3 -c 'import PIL' 2>/dev/null; then
  python3 -I "$here/fetch_covers.py" "$here/games.txt" "$(dirname "$data")/real-covers" "$data/covers" "$disc" || say "real covers: fetch failed, the painted ones stay"
fi
[[ -d $sounds ]] || say "no console sounds in $sounds: the shelf falls back to its synthesized keys (PS2_SHELF_SOUNDS=<dir>)"

# F2 in the window: the other version (current <-> v2; a --with set goes to v2).
self=${PREVIEW_SELF:-$here/preview.sh}
if [[ $mode == v2 ]]; then other=current; other_label=current; else other=v2; other_label=v2; fi
switch="$other_label|exec '$self' --gui $other ${fullscreen[*]}"

say "starting $label"
cd "$dir"
exec "$bin" --root "$tree/ps5/frontend" --data "$data" --out "$shots" --games "$here/games.txt" --sounds "$sounds" --disc "$disc" \
  --home --label "$label" --switch "$switch" --window "${fullscreen[@]}" "${extra[@]}"
