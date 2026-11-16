#!/usr/bin/env bash
# laptop-preview (AI-assisted): installs the "PS2 Shelf Preview" launcher for this user (no root):
#   ~/.local/bin/ps2-shelf-preview                        the command (runs preview.sh; any preview.sh arguments)
#   ~/.local/share/applications/ps2-shelf-preview.desktop  the app menu entry (a dialog: v2, current or pick changes; right-click for v2 / current)
# The command runs preview.sh from the repo's checkout when it has one, else from the laptop-preview branch (git archive), so it keeps
# working whichever branch the repo is on.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(git -C "$here" rev-parse --show-toplevel)
bin=$HOME/.local/bin/ps2-shelf-preview
apps=${XDG_DATA_HOME:-$HOME/.local/share}/applications
icons=${XDG_DATA_HOME:-$HOME/.local/share}/icons
mkdir -p "$(dirname "$bin")" "$apps" "$icons"

cat >"$bin" <<EOF
#!/usr/bin/env bash
# PS2 Shelf Preview (installed by ps5/frontend/host/preview/install-launcher.sh on branch laptop-preview of PS5SX2).
#   ps2-shelf-preview [v2|current|--with P2-01,P2-07|--choose] [--fullscreen] [--rebuild] [--list] [--help]
repo=\${PS5SX2_REPO:-$repo}
cache=\${XDG_CACHE_HOME:-\$HOME/.cache}/ps2-shelf-preview
s=\$repo/ps5/frontend/host/preview/preview.sh
if [[ ! -x \$s ]]; then # the checkout is on another branch: the tooling from laptop-preview
  tool=\$cache/tool
  rm -rf "\$tool" && mkdir -p "\$tool"
  git -C "\$repo" archive laptop-preview ps5/frontend/host/preview ps5/frontend/host/fe_host.cpp ps5/frontend/host/host_window.h \\
    ps5/frontend/host/build-host.sh | tar -x -C "\$tool" || { echo "no laptop-preview branch in \$repo" >&2; exit 1; }
  s=\$tool/ps5/frontend/host/preview/preview.sh
fi
export PS5SX2_REPO=\$repo PREVIEW_SELF=\$0
if [[ -t 1 ]]; then exec "\$s" "\$@"; fi
mkdir -p "\$cache"
exec "\$s" "\$@" >"\$cache/last.log" 2>&1 # started from the menu: the log goes here
EOF
chmod +x "$bin"

icon=applications-games
src=$HOME/ps5-work/forks/ps5sx2/PPSA99204/sce_sys/icon0.png # the fork's own PS2-style icon (made locally, not in git)
if [[ -f $src ]]; then
  cp "$src" "$icons/ps2-shelf-preview.png"
  icon=$icons/ps2-shelf-preview.png
fi

cat >"$apps/ps2-shelf-preview.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=PS2 Shelf Preview
GenericName=PS5SX2 PS2 shelf preview
Comment=Try the PS5SX2 PS2 shelf in a window: keyboard or DualShock 4 / DualSense, with the console sounds
Exec=$bin --choose
Icon=$icon
Terminal=false
Categories=Game;
Keywords=PS2;PS5SX2;shelf;preview;
StartupWMClass=ps2-shelf-preview
Actions=v2;current;pick;

[Desktop Action v2]
Name=v2 (all changes)
Exec=$bin --gui v2

[Desktop Action current]
Name=Current (ps2-console-shelf)
Exec=$bin --gui current

[Desktop Action pick]
Name=Pick P2 changes...
Exec=$bin --choose
EOF
command -v update-desktop-database >/dev/null && update-desktop-database "$apps" >/dev/null 2>&1 || true
command -v kbuildsycoca6 >/dev/null && kbuildsycoca6 >/dev/null 2>&1 || true
echo "installed: $bin and $apps/ps2-shelf-preview.desktop"
