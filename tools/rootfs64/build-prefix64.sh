#!/bin/bash
# build-prefix64.sh — (re)assemble the PRE-BOOTED wine prefix overlay that the
# browser launcher mounts as a third zip (see project/emscripten/wine64-launcher.js,
# ?p=<prog>). The launcher runs a program with HOME=/home/username /
# WINEPREFIX=/home/username/.wine against this prefix so wine SKIPS wineboot.
#
# Why this script exists: the original prefix64.zip was staged ad-hoc and was
# missing the C:\users\username user-profile directory tree. wine's registry
# (user.reg "Shell Folders" / system.reg ProfileList "ProfilesDirectory"="C:\users")
# points Desktop/Personal(Documents)/AppData/etc. at C:\users\username\..., and
# %USERPROFILE% resolves to C:\users\username. With those dirs ABSENT, comdlg32's
# GetSaveFileName / shell32 IShellFolder enumeration found nothing — Notepad's
# "Save As" dialog showed a GRAY/empty file list and Save silently failed.
# Normally wineboot --init creates this tree, but this prefix is pre-booted and
# never runs wineboot, so we materialize the dirs here.
#
# The prefix maps c: -> ../drive_c (i.e. home/username/.wine/drive_c). We add
# empty profile dirs under there; `zip` stores empty directories fine and the
# kernel overlays a writable native root on top so Save can actually write.
#
# Idempotent: takes the existing dist/prefix64.zip as the base, adds the profile
# tree, and rewrites the zip in place. Output lands in tools/rootfs64/dist/
# (gitignored — build artifact). Re-copy to project/emscripten/Build/Wasm64Mt/
# afterward to deploy (this script does that if the Build dir exists).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
DIST="$HERE/dist"
ZIP="$DIST/prefix64.zip"
DEPLOY="$HERE/../../project/emscripten/Build/Wasm64Mt/prefix64.zip"

[ -f "$ZIP" ] || { echo "ERROR: $ZIP not found (build the base prefix first)" >&2; exit 1; }

# drive_c is rooted under the .wine prefix dir inside the zip.
PREFIX_IN_ZIP="home/username/.wine"
DRIVE_C="$PREFIX_IN_ZIP/drive_c"

# The standard Wine user-profile tree (what wineboot --init would create for the
# "username" profile), plus the Public profile shell32 also references. Paths are
# relative to drive_c (== C:\). These match the registry Shell Folders targets.
PROFILE_DIRS=(
  users/username/Desktop
  users/username/Documents
  users/username/Downloads
  users/username/Music
  users/username/Pictures
  users/username/Videos
  users/username/Favorites
  users/username/Links
  users/username/Contacts
  users/username/Searches
  users/username/AppData/Roaming/Microsoft/Windows/Start Menu/Programs/Administrative Tools
  users/username/AppData/Roaming/Microsoft/Windows/Start Menu/Programs/StartUp
  users/username/AppData/Roaming/Microsoft/Windows/Network Shortcuts
  users/username/AppData/Roaming/Microsoft/Windows/Printer Shortcuts
  users/username/AppData/Roaming/Microsoft/Windows/Recent
  users/username/AppData/Roaming/Microsoft/Windows/SendTo
  users/username/AppData/Roaming/Microsoft/Windows/Templates
  users/username/AppData/Local/Microsoft/Windows/INetCache
  users/username/AppData/Local/Microsoft/Windows/INetCookies
  users/username/AppData/Local/Microsoft/Windows/History
  users/username/AppData/Local/Temp
  users/Public/Desktop
  users/Public/Documents
  users/Public/Music
  users/Public/Pictures
  users/Public/Videos
)

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

echo "=== unpacking base prefix64.zip ==="
unzip -q "$ZIP" -d "$STAGE"

echo "=== materializing C:\\users profile tree under $DRIVE_C ==="
for d in "${PROFILE_DIRS[@]}"; do
  mkdir -p "$STAGE/$DRIVE_C/$d"
done
# Drop a .keep in each leaf so the dir survives any tooling that prunes empties,
# and so Save As shows a non-empty, browsable target. (zip stores empty dirs, but
# the marker also makes the populated tree visible when debugging the VFS.)
find "$STAGE/$DRIVE_C/users" -type d -empty -exec touch '{}/.keep' \;

echo "=== rezipping prefix64.zip ==="
rm -f "$ZIP"
( cd "$STAGE" && zip -qry9 "$ZIP" . )
echo "--- new prefix64.zip ---"
ls -lh "$ZIP"
echo "--- profile tree in zip ---"
unzip -l "$ZIP" | grep -i 'users/' | head -40

if [ ! -e "$DEPLOY" ] && [ ! -L "$DEPLOY" ]; then
  if [ -d "$(dirname "$DEPLOY")" ]; then
    echo "=== deploying to $DEPLOY ==="
    cp "$ZIP" "$DEPLOY"
  else
    echo "NOTE: $(dirname "$DEPLOY") not present — skipped deploy." >&2
  fi
elif [ "$ZIP" -ef "$DEPLOY" ]; then
  # Build/ prefix64.zip is a symlink back to dist/ — deploy is automatic.
  echo "=== Build/ prefix64.zip -> dist (symlink); deploy is automatic ==="
else
  echo "=== deploying to $DEPLOY ==="
  cp "$ZIP" "$DEPLOY"
fi
echo "=== done ==="
