#!/bin/bash
# run_wine64_gui.sh — interactive front end for the wine64 GUI bring-up.
#
# Like the 32-bit Boxedwine "run a program" UI: instead of being hardwired to
# notepad.exe, this lists the GUI-capable PE programs found in the staged wine64
# rootfs and lets you pick one (or pass one on the command line), then launches
# it through the full Boxedwine64 kernel WITH video enabled (a real window).
#
# Usage:
#   tools/run_wine64_gui.sh                  # menu of known GUI apps
#   tools/run_wine64_gui.sh notepad          # launch by short name
#   tools/run_wine64_gui.sh notepad.exe      # launch by exe name
#   tools/run_wine64_gui.sh C:\\path\\to\\app.exe   # launch an arbitrary guest exe
#   tools/run_wine64_gui.sh /full/guest/path/app.exe [args...]
#
# This is the GUI sibling of run_wine64.sh (which stays headless via -novideo
# for diagnostics). The heavy lifting — prefix self-heal, transient cleanup,
# layered zips, env — is shared with run_wine64.sh via the common block below.
set -uo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RF="$ROOT_DIR/tools/rootfs64"
DIST="$RF/dist"
GLIBC_ZIP="$DIST/glibc-rootfs64.zip"
WINE_ZIP="$DIST/wine64.zip"
BASE_ROOT="$RF/root"
WIN_DIR="usr/lib/x86_64-linux-gnu/wine/x86_64-windows"

# Snapshot the exe listing once. Piping `unzip -l | grep -q` under `pipefail`
# races: grep -q closes the pipe on first match, unzip takes SIGPIPE, and the
# pipeline reports failure — so membership tests come back false at random.
# Capture the full listing here and match against this string instead.
WINE_ZIP_LIST="$(unzip -l "$WINE_ZIP" 2>/dev/null || true)"

has_guest_exe() {
    # $1 = base name (no .exe). Return 0 if that exe exists in the rootfs/zip.
    local base="$1"
    [ -f "$BASE_ROOT/$WIN_DIR/${base}.exe" ] && return 0
    case "$WINE_ZIP_LIST" in
        *"$WIN_DIR/${base}.exe"*) return 0 ;;
        *) return 1 ;;
    esac
}

BOX="$ROOT_DIR/project/mac-xcode/build_dd/Build/Products/Release/Boxedwine.app/Contents/MacOS/Boxedwine"
if [ ! -x "$BOX" ]; then
    for p in "$HOME/Library/Developer/Xcode/DerivedData"/Boxedwine-*/Build/Products/*/Boxedwine.app/Contents/MacOS/Boxedwine; do
        [ -x "$p" ] && BOX="$p"
    done
fi
[ -x "$BOX" ] || { echo "error: no Boxedwine build found (build the Xcode Release target first)" >&2; exit 1; }

# A curated short-name -> exe map of the GUI-interesting programs shipped in the
# wine64 zip. Extend freely; anything not listed can still be launched by
# passing its full guest path. (Order = menu order.)
declare -a NAMES=(
    "notepad"     "Notepad — text editor (the proven-good GUI app)"
    "winecfg"     "Wine configuration panel"
    "regedit"     "Registry editor"
    "taskmgr"     "Task manager"
    "clock"       "Analog clock"
    "write"       "WordPad-style editor"
    "explorer"    "Wine desktop / file explorer"
    "progman"     "Program manager"
    "iexplore"    "Internet Explorer shell"
    "uninstaller" "Add/Remove programs"
    "winemine"    "Minesweeper (if present)"
    "control"     "Control panel (if present)"
    "cmd"         "Command prompt (console)"
)

resolve_exe() {
    # $1 = user token. Echo the guest path to launch, or empty if not found.
    local tok="$1"
    case "$tok" in
        /*)            echo "$tok"; return 0 ;;           # absolute guest path
        [A-Za-z]:\\*)  echo "$tok"; return 0 ;;           # windows-style path
        *)             : ;;
    esac
    local base="${tok%.exe}"
    local guest="/$WIN_DIR/${base}.exe"
    if has_guest_exe "$base"; then
        echo "$guest"; return 0
    fi
    echo ""; return 1
}

choose_from_menu() {
    echo "Pick a Windows program to launch in Boxedwine64 (GUI):" >&2
    local i=1
    local -a keys=()
    local n=${#NAMES[@]}
    local idx=0
    while [ $idx -lt $n ]; do
        local key="${NAMES[$idx]}"
        local desc="${NAMES[$((idx+1))]}"
        # Only show entries that actually exist in the rootfs/zip.
        if has_guest_exe "$key"; then
            printf "  %2d) %-12s %s\n" "$i" "$key" "$desc" >&2
            keys+=("$key")
            i=$((i+1))
        fi
        idx=$((idx+2))
    done
    printf "\n  Enter a number, a name, or a full guest exe path: " >&2
    local pick
    read -r pick
    case "$pick" in
        ''|*[!0-9]*) echo "$pick" ;;                       # name or path
        *)           echo "${keys[$((pick-1))]:-}" ;;      # numeric index
    esac
}

# ---- pick the target ----
if [ $# -ge 1 ]; then
    TOKEN="$1"; shift
else
    TOKEN="$(choose_from_menu)"
fi
[ -n "${TOKEN:-}" ] || { echo "no program selected" >&2; exit 1; }

GUEST_PE="$(resolve_exe "$TOKEN")"
if [ -z "$GUEST_PE" ]; then
    echo "error: could not find '$TOKEN' in the wine64 rootfs." >&2
    echo "       pass a full guest path (e.g. /$WIN_DIR/notepad.exe) to force it." >&2
    exit 1
fi

# ---- shared prefix prep (mirrors run_wine64.sh) ----
PREFIX="$BASE_ROOT/home/username/.wine"
rm -f "$PREFIX"/regf*.tmp "$PREFIX/.update-timestamp" 2>/dev/null || true
find "$BASE_ROOT/run/user/1000/wine" -maxdepth 1 -name 'server-1-*' \
    ! -name 'server-1-4ee' -exec rm -rf {} + 2>/dev/null || true

if [ ! -d "$PREFIX/drive_c/windows/system32" ]; then
    echo "prefix: drive_c missing -> running wineboot --init"
    "$BOX" \
        -root "$BASE_ROOT" -zip "$GLIBC_ZIP" -zip "$WINE_ZIP" -novideo \
        -env "HOME=/home/username" -env "USER=username" \
        -env "WINEPREFIX=/home/username/.wine" \
        -env "WINELOADER=/usr/lib/wine/wine64" \
        -env "WINESERVER=/usr/lib/wine/wineserver64" \
        -env "WINEDLLPATH=/usr/lib/x86_64-linux-gnu/wine" \
        /usr/lib/wine/wine64 wineboot --init >/dev/null 2>&1 || true
    rm -f "$PREFIX"/regf*.tmp "$PREFIX/.update-timestamp" 2>/dev/null || true
    find "$BASE_ROOT/run/user/1000/wine" -maxdepth 1 -name 'server-1-*' \
        ! -name 'server-1-4ee' -exec rm -rf {} + 2>/dev/null || true
fi

echo "using:   $BOX"
echo "guest:   wine64 $GUEST_PE $*"
echo "video:   ENABLED (interactive window) — retry past the Mode-2 boot wedge if it stalls"

# NOTE: no -novideo here, so a real SDL window opens and the in-process X11 wire
# server drives keyboard/mouse/menus into winex11 (Milestone F Phase 2).
"$BOX" \
    -root "$BASE_ROOT" \
    -zip "$GLIBC_ZIP" \
    -zip "$WINE_ZIP" \
    -env "HOME=/home/username" \
    -env "USER=username" \
    -env "WINEPREFIX=/home/username/.wine" \
    -env "WINELOADER=/usr/lib/wine/wine64" \
    -env "WINESERVER=/usr/lib/wine/wineserver64" \
    -env "WINEDLLPATH=/usr/lib/x86_64-linux-gnu/wine" \
    -env "DISPLAY=:0" \
    /usr/lib/wine/wine64 "$GUEST_PE" "$@" 2>&1 \
    | grep -avE "pixel format|redundant|new pixel|failed to choose|software renderer|Number which|Number of"
