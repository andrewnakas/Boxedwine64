#!/bin/bash
# build-games.sh — cross-compile the bundled games to 64-bit Windows PE with
# mingw-w64. Output .exe land next to this script; build-prefix64.sh copies them
# into prefix64.zip so the in-browser app launcher (wine64.html "Games" row) can
# run them against the pre-booted prefix.
#
# Requires: x86_64-w64-mingw32-gcc (brew install mingw-w64).
#
# snake.exe / tetris.exe are self-contained Win32+GDI sources in src/ (the proven
# render path under the emulator — same as notepad/winemine: RegisterClass /
# CreateWindow / PeekMessage loop / double-buffered BitBlt).
#
# doom.exe is built SEPARATELY from doomgeneric (GPLv2, https://github.com/ozkl/
# doomgeneric) using its Win32 GDI backend (doomgeneric_win.c) — see the comment
# block at the bottom for the exact command. It is NOT rebuilt here because it
# needs the doomgeneric checkout; the prebuilt doom.exe + the freely-
# redistributable shareware doom1.wad are committed alongside it.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CC=x86_64-w64-mingw32-gcc
command -v "$CC" >/dev/null || { echo "ERROR: $CC not found (brew install mingw-w64)" >&2; exit 1; }

for g in snake tetris; do
  echo "=== building $g.exe ==="
  "$CC" -O2 -std=gnu11 -mwindows "$HERE/src/$g.c" -o "$HERE/$g.exe" -lgdi32 -luser32
  file "$HERE/$g.exe"
done
echo "=== done (snake.exe, tetris.exe) ==="

# --- doom.exe (run once, from a doomgeneric checkout) -----------------------
# git clone --depth 1 https://github.com/ozkl/doomgeneric
# cd doomgeneric/doomgeneric
# SRC="dummy.c am_map.c doomdef.c doomstat.c dstrings.c d_event.c d_items.c \
#   d_iwad.c d_loop.c d_main.c d_mode.c d_net.c f_finale.c f_wipe.c g_game.c \
#   hu_lib.c hu_stuff.c info.c i_cdmus.c i_endoom.c i_joystick.c i_scale.c \
#   i_sound.c i_system.c i_timer.c memio.c m_argv.c m_bbox.c m_cheat.c m_config.c \
#   m_controls.c m_fixed.c m_menu.c m_misc.c m_random.c p_ceilng.c p_doors.c \
#   p_enemy.c p_floor.c p_inter.c p_lights.c p_map.c p_maputl.c p_mobj.c \
#   p_plats.c p_pspr.c p_saveg.c p_setup.c p_sight.c p_spec.c p_switch.c \
#   p_telept.c p_tick.c p_user.c r_bsp.c r_data.c r_draw.c r_main.c r_plane.c \
#   r_segs.c r_sky.c r_things.c s_sound.c sha1.c sounds.c statdump.c st_lib.c \
#   st_stuff.c tables.c v_video.c wi_stuff.c w_checksum.c w_file.c w_main.c \
#   w_wad.c z_zone.c w_file_stdc.c i_input.c i_video.c doomgeneric.c \
#   doomgeneric_win.c"
# x86_64-w64-mingw32-gcc -O2 -std=gnu11 -DNORMALUNIX -w -I. $SRC \
#   -o doom.exe -lgdi32 -luser32 -lwinmm
# (-std=gnu11 because doom's `boolean { false, true }` enum clashes with C23
#  keywords. No audio: the base source set uses the dummy i_sound, not SDL.)
# shareware WAD (freely redistributable by id Software):
#   doom1.wad  md5 f0cefca49926d00903cf57551d901abe  (~4.2 MB)
