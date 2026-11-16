# PS2 shelf v3: hooks into upstream files

The v3 shelf (the PS2 menu rebuilt from our OSDSYS 2.00 spec) lives in its own files. Upstream files
only get short, marked calls, so merges from `upstream` (Swordpdf/PS5SX2) stay easy.

## Our files
| File | What |
|---|---|
| `ps5/frontend/shelf3/s3_bios.h/.cpp` | Reads the menu's artwork from the player's own BIOS at run time: fonts, textures, 3D icons, sound effects (SPU2 ADPCM + ADSR). Only SCPH-70004 v2.00 EUR is accepted (container SHA-1s); anything else: generic artwork. Holds offsets, hashes and the font metrics only. |
| `ps5/frontend/shelf3/s3_kit.h/.cpp` | Drawing kit in PAL field units (640 x 256, widened to x -107..747 for 16:9; 4:3 has black sides): BIOS font text, prompts, Browser gradients/bands/flare, the ico renderer (lights, shapes, painter sort). Draws at the display's native size. |
| `ps5/frontend/shelf3/s3_memcard.h/.cpp` | PCSX2 memory cards (card files with or without ECC, folder cards): saves, icon.sys, icons; Delete (a copy of the card goes to `<card>.shelf-backup` first; ECC is rewritten only where the card's own ECC matched). |
| `ps5/frontend/shelf3/s3_app.h` | The v3 state (`s3::Shelf3`) and the App members (`S3_APP_MEMBERS`). |
| `ps5/frontend/shelf3/s3_app.inc` | App code, part 1: 50 Hz clock, fades, BIOS sounds, main menu (tunnel, orbs, trails, spotlight, words), Browser (Memory Card / Library / Disc with the appear motion), memory card page, save menu, delete. |
| `ps5/frontend/shelf3/s3_app2.inc` | App code, part 2: Library (6x3 / 4x2; 4:3: 5x3 / 3x2, Options filter and sort, keypad search, empty), game page (save states, favourite, play/load/delete), settings (System Configuration look, L1/R1 tabs, per-game overrides). |
| `ps5/frontend/shelf3/s3_seq.h/.cpp` | The menu music sequencer (SSsq + the BOOT bank): CLOKS, BOOTS, TNNLS. |
| `ps5/frontend/shelf3/s3_opening.inc` | The PS2 opening drawn from boot.md 2 (camera path, fog, lights, cubes, logo, towers from playtime.dat), into the main menu. |
| `docs/SHELF-HOOKS.md` | This file. |

No Sony data is in the repo: every texture, glyph, icon and sound comes from the user's BIOS in
`/data/PCSX2/bios` when the shelf starts (it degrades to the eboot's font and plain shapes without one).
gs.ini keys of ours: `PS5SX2/ShelfLayout`, `ShelfShape`, `DiscSlot`, `LibraryIcon` (shelf/stack/case/card), `AutoSave`,
`StartupAnimation` (Fast Boot (Opening)), `StartupBiosRun`. Runtime files of ours: `/data/PCSX2/shelf/favourites.txt` (one serial a line) and
`/data/PCSX2/logs/shelf-state.txt` (the save state the game page picked; read once by main-boot).

## Hooks (all marked `shelf v3` in the source)
| File | Hook |
|---|---|
| `ps5/frontend/fe_app.h` | `#include "shelf3/s3_app.h"`; `S3_APP_MEMBERS` at the top of App's private part. |
| `ps5/frontend/fe_app.cpp` `App::Update` | `else if (S3Active()) { S3Update(dt, in); ... }` before the search/account/sheet branches; the startup.wav / ambient drone block skips both while S3Active(). |
| `ps5/frontend/fe_app.cpp` `App::Build` | `if (S3Active()) S3Build(...)` before the sheet/screens; the old search overlay skipped; `f.ui_images` keeps the kit's ranges with the badges' (sorted). |
| `ps5/frontend/fe_app.cpp` `ApplyHidden` | `S3Keep()` (the Show filter) and `S3SortShelf()` (the sort). |
| `ps5/frontend/fe_app.cpp` `PositionOf` | a plain search (the Library may be sorted). |
| `ps5/frontend/fe_app.cpp` `SearchMatches` | `S3SearchMatches()` (word starts). |
| `ps5/frontend/fe_app.cpp` `PollDriveDisc` | returns early (the disc is the Browser's object). |
| `ps5/frontend/fe_app.cpp` `PollDiscAsk` / `UpdateDiscAsk` / `UpdateBiosChoice` | the disc question only after X on the Browser's disc (`m_s3.ask_allowed_until`); `m_s3.resync` so buttons held in those panels don't reach the shelf. |
| `ps5/frontend/fe_app.cpp` `TakeDiscArt` | also takes the art while the Browser shows the disc. |
| `ps5/frontend/fe_app.cpp` `Shutdown` | `SetStream(nullptr)` (the menu music stops). |
| `ps5/frontend/fe_app.h` `AppConfig` | `drive_eject`. |
| `ps5/frontend/fe_app.cpp` (end) | `#include "shelf3/s3_app.inc"`, `"shelf3/s3_app2.inc"`. |
| `ps5/frontend/fe_renderer.h/.cpp` | `FrameDesc::ui_under` (UI drawn under the cases); textured UI ranges in the scene pass; the UI pipelines blend premultiplied (`PipelineDesc::Premultiplied`). |
| `ps5/frontend/shaders/ui.frag` (+ `fe_shaders.inc`) | premultiplied output; `params.w >= 16` additive; modes 9/10/11 (GS modulate, repeating, flat), 12/13 (texel alpha ignored). Regenerate with `build_shaders.sh`. |
| `ps5/frontend/fe_sound.h/.cpp` | `PlayClip`, `OverrideSfx` (BIOS effects for every Sfx), `SetStream` (the sequencers; Idle false while set). |
| `ps5/frontend/fe_renderer.h` | `SceneSet()` (the scene image for the glass's refraction). `post.frag`: blur up to 3x. |
| `ps5/frontend/fe_options.cpp` | Shelf group: Library Layout, Disc Slot, Screen Shape; StartupAnimation shown as Fast Boot (Opening); Game group: Auto Save. |
| `ps5/frontend/fe_ps5.cpp` | the display mode of the TV's physical resolution (native 1080p/1440p/4K) instead of the biggest; `drive_eject` (PREVENT ALLOW MEDIUM REMOVAL + START STOP UNIT LoEj through the pass device). |
| `ps5/frontend/host/fe_host.cpp` | a stand-in `drive_eject`. |
| `ps5/frontend/host/build-host.sh` | builds `shelf3/*.cpp`; rebuilds fe_app when `shelf3/*.h`/`*.inc` change. |
| `ps5/coreorbis/Makefile.vk` | `shelf3/*.cpp` in `FE_SRCS`, `shelf3/*.h *.inc` in `FE_HDRS`. |
| `ps5/coreorbis/main-boot.cpp` | `#include "../frontend/shelf3/s3_memcard.h"`; `logs/shelf-state.txt` -> `VMBootParameters::save_state`; Auto Save (`PS5SX2/AutoSave`) saves the resume state in `OrbisBackToMenuCpu`; the BIOS-run startup (P2-24) only with gs.ini `PS5SX2/StartupBiosRun=true` (the shelf draws the opening itself); `orbis_shelf3_intro_card()` after gs.ini is applied for the startup intro: slot 1 becomes `memcards/shelf-intro.ps2`, a copy of the player's card with `/BEDATA-SYSTEM/history` built from playtime.dat, so the BIOS's own opening raises the towers (checked in PCSX2 2.8.2 with this BIOS: towers appear); no "starting" toast while the opening plays (it showed over it). |

`git grep -n "shelf v3" -- ':!ps5/frontend/shelf3'` lists every hook. The older shelf branches
(ps2-console-shelf, ps2-shelf-v2, laptop-preview) also changed upstream files (`fe_ps2*.{h,cpp}`,
`fe_app.*`, `bg.frag`, ...); `git diff upstream/main --stat -- ps5` shows all of them.

## Upstream merge (one command)
```sh
git fetch upstream && git checkout ps2-shelf-v3 && git merge --no-edit upstream/main
```
On a conflict, keep upstream's side and add back the hook lines from the table above
(`git grep -n "shelf v3"` on this branch's last commit shows them). Then regenerate the shaders
(`ps5/frontend/build_shaders.sh`) and check with the laptop preview (below).

## Laptop preview
```sh
cd ps5/frontend/host
CXX=g++ CC=gcc FE_HOST_WINDOW=1 FE_HOST_OBJ=~/.cache/ps2-shelf-v3/obj FE_HOST_OUT=~/.cache/ps2-shelf-v3/fe_host bash build-host.sh
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/intel_icd.json ~/.cache/ps2-shelf-v3/fe_host --root "$PWD/.." \
  --data <stand-in /data/PCSX2 with bios/ and memcards/> --games preview/games.txt --home --window
```
Scripted shots: replace `--window` with `--out <dir> "wait 4" "shot menu" "press cross" ...`
("sleep N" waits real seconds for covers; "drive <serial> <title>" puts a disc in).
