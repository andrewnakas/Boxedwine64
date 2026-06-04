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

BOX="${BW64_BIN:-$ROOT_DIR/project/mac-xcode/build_dd/Build/Products/Release/Boxedwine.app/Contents/MacOS/Boxedwine}"
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

# Prefix path (used by the staging workaround below and the prefix prep later).
PREFIX="$BASE_ROOT/home/username/.wine"

# Pin wine's prefix-update check OFF. wineboot's update_timestamp() compares the
# CONTENTS of .wine/.update-timestamp against wine.inf's mtime and, if they don't
# match (or the file is missing/0), runs a full multi-process `wineboot --update`
# — the "The Wine configuration in … is being updated, please wait…" dialog that
# then grinds in the slow prefix-init path. The wine source also honors the
# literal string "disable" in that file to SKIP the update entirely
# (programs/wineboot/wineboot.c: `if (!strncmp(buffer,"disable",...)) goto done;`).
# This committed prefix is already initialized, so pin it to "disable" rather than
# DELETING the timestamp (deleting it forced the update every run). Set
# BW64_FORCE_UPDATE=1 to allow wine to update the prefix (e.g. after a wine bump).
pin_update_timestamp() {
    [ "${BW64_FORCE_UPDATE:-0}" = "1" ] && return 0
    printf 'disable\n' > "$PREFIX/.update-timestamp" 2>/dev/null || true
}

# ---------------------------------------------------------------------------
# Bootstrap-loader workaround (the "kernel32 / 3D no-opening" gate).
#
# The FIRST wine process (the one named on the cmdline = the bootstrap) only
# searches its own application directory and C:\windows\system32 for builtin
# DLLs — it never searches the wine builtin DLL dir (x86_64-windows, where
# kernel32.dll actually lives). C:\windows\system32 in this prefix has no
# kernel32.dll, so launching an exe that lives anywhere ELSE (e.g. C:\app.exe)
# dies with "could not load kernel32.dll" (status c0000135) before it runs a
# single instruction. Apps spawned LATER by wineserver/services don't hit this
# (wineserver maps kernel32 for them), but the bootstrap does.
#
# Proven workaround: launch the exe from INSIDE the builtin dir, because wine
# searches the main exe's own directory first and that dir holds every builtin
# DLL. So we stage the target into the builtin dir (base-rootfs overlay, which
# layers on top of wine64.zip) and launch it from there. Builtin-dir exes
# (notepad, winecfg, ...) are launched as-is. Set BW64_NO_STAGE=1 to skip.
# ---------------------------------------------------------------------------
guest_to_host() {
    # Map a guest path (/..., C:\..., or Z:\...) to its host file under the
    # rootfs, so we can copy it. Echoes the host path or empty.
    local g="$1" hp=""
    case "$g" in
        [A-Za-z]:\\*)
            # Windows drive path -> dosdevices link target. c:->drive_c, z:->/
            local drive="$(printf '%s' "$g" | cut -c1 | tr 'A-Z' 'a-z')"
            local rest="$(printf '%s' "$g" | cut -c4- | tr '\\' '/')"
            if [ "$drive" = "c" ]; then hp="$PREFIX/drive_c/$rest"
            elif [ "$drive" = "z" ]; then hp="$BASE_ROOT/$rest"
            else hp=""; fi
            ;;
        /*) hp="$BASE_ROOT$g" ;;   # absolute guest path = under rootfs root
    esac
    [ -n "$hp" ] && [ -f "$hp" ] && echo "$hp" || echo ""
}

if [ "${BW64_NO_STAGE:-0}" != "1" ]; then
    case "$GUEST_PE" in
        /$WIN_DIR/*)
            : ;;  # already in the builtin dir — the bootstrap can find kernel32
        *)
            HOST_PE="$(guest_to_host "$GUEST_PE")"
            if [ -n "$HOST_PE" ]; then
                STAGE_DIR="$BASE_ROOT/$WIN_DIR"
                STAGE_NAME="$(basename "$HOST_PE")"
                mkdir -p "$STAGE_DIR"
                # Only stage if not already provided by the zip/rootfs under that
                # name; remove our staged copy on exit so the tree stays clean.
                if [ ! -e "$STAGE_DIR/$STAGE_NAME" ]; then
                    cp -f "$HOST_PE" "$STAGE_DIR/$STAGE_NAME"
                    STAGED_COPY="$STAGE_DIR/$STAGE_NAME"
                    trap 'rm -f "$STAGED_COPY" 2>/dev/null; rmdir -p "$(dirname "$STAGED_COPY")" 2>/dev/null || true' EXIT
                fi
                echo "stage:   '$GUEST_PE' -> /$WIN_DIR/$STAGE_NAME (bootstrap-loader workaround)"
                GUEST_PE="/$WIN_DIR/$STAGE_NAME"
            else
                echo "warn:    could not locate '$GUEST_PE' on the host to stage it;" >&2
                echo "         launching as-is may fail with 'could not load kernel32.dll'." >&2
            fi
            ;;
    esac
fi

# ---- shared prefix prep (mirrors run_wine64.sh) ----
rm -f "$PREFIX"/regf*.tmp 2>/dev/null || true
pin_update_timestamp
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
    rm -f "$PREFIX"/regf*.tmp 2>/dev/null || true
    pin_update_timestamp
    find "$BASE_ROOT/run/user/1000/wine" -maxdepth 1 -name 'server-1-*' \
        ! -name 'server-1-4ee' -exec rm -rf {} + 2>/dev/null || true
fi

echo "using:   $BOX"
echo "guest:   wine64 $GUEST_PE $*"
echo "video:   ENABLED (interactive window) — supervised: waits out a slow boot, restarts a dead one"

# ---------------------------------------------------------------------------
# Supervised launch (so the GUI ALWAYS reaches a window, not a ~50/50 lottery).
#
# Diagnosis (RIP-sampler, 2026-06-02): the boot never deadlocks and never misses
# a wakeup — wineserver (pid 14) is the single bottleneck and grinds through its
# startup CONTINUOUSLY at interpreter speed while every other process correctly
# blocks on it; the window maps once it finishes. The perceived "wedge" was just
# a slow-but-progressing boot being killed too early (the activity log stops
# growing during wineserver's internal grind even though it IS progressing).
#
# So the reliable strategy is: be patient, and only RESTART if the box actually
# dies (crash/exit) before mapping a window. We watch the box's stdout for the
# "first window mapped" marker. If the box exits before that, we relaunch (fresh
# attempt; each is independent). Once the window maps we stop supervising and
# stream output through for the rest of the session.
# ---------------------------------------------------------------------------

# Tunables (override via env): how many fresh attempts, and how long to wait for
# a window before giving up on a SILENT box (one that is alive but printed no new
# output at all for this long — a true hang, not slow progress).
MAX_ATTEMPTS="${BW64_GUI_MAX_ATTEMPTS:-6}"
# A progressing boot dribbles output the whole way (slowest observed map ~300s).
# Only treat the box as hung if it emits NOTHING new for this long.
SILENT_GIVEUP_SECS="${BW64_GUI_SILENT_SECS:-240}"   # no NEW output for this long = restart

run_box() {
    # NOTE: no -novideo here, so a real SDL window opens and the in-process X11
    # wire server drives keyboard/mouse/menus into winex11 (Milestone F Phase 2).
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
}

cleanup_transients() {
    rm -f "$PREFIX"/regf*.tmp 2>/dev/null || true
    pin_update_timestamp
    find "$BASE_ROOT/run/user/1000/wine" -maxdepth 1 -name 'server-1-*' \
        ! -name 'server-1-4ee' -exec rm -rf {} + 2>/dev/null || true
}

attempt=1
while [ "$attempt" -le "$MAX_ATTEMPTS" ]; do
    [ "$attempt" -gt 1 ] && echo ">>> boot attempt $attempt/$MAX_ATTEMPTS (previous attempt died before mapping a window)"
    LOG="$(mktemp -t bw64gui.XXXXXX)"

    # Run the box in the background, tee-ing its (filtered) output to both the
    # terminal and a log we watch for the window-mapped marker + liveness.
    run_box | tee "$LOG" &
    PIPE_PID=$!
    # The box itself is the grandchild; find it so we can detect its exit and,
    # on a restart, kill it cleanly.
    sleep 1
    BOX_PID="$(pgrep -n -f "$BOX .* $GUEST_PE" 2>/dev/null || true)"

    mapped=0
    last_size=0
    silent_for=0
    while kill -0 "$PIPE_PID" 2>/dev/null; do
        if grep -qiE 'first window mapped|host screen sized' "$LOG" 2>/dev/null; then
            mapped=1
            break
        fi
        sz=$(wc -c < "$LOG" 2>/dev/null || echo 0)
        if [ "$sz" = "$last_size" ]; then
            silent_for=$((silent_for+3))
        else
            silent_for=0; last_size=$sz
        fi
        # A box that is ALIVE but has emitted no new output for SILENT_GIVEUP_SECS
        # is a genuine hang (not slow progress — progress always dribbles output);
        # kill it and start a fresh attempt.
        if [ "$silent_for" -ge "$SILENT_GIVEUP_SECS" ]; then
            # Re-check the marker once more before pulling the plug — the window
            # may have mapped in the same tick the silence timer expired.
            if grep -qiE 'first window mapped|host screen sized' "$LOG" 2>/dev/null; then
                mapped=1; break
            fi
            echo ">>> no progress for ${SILENT_GIVEUP_SECS}s — restarting"
            [ -n "${BOX_PID:-}" ] && kill -9 "$BOX_PID" 2>/dev/null || true
            pkill -9 -f "$BOX " 2>/dev/null || true
            break
        fi
        sleep 3
    done

    if [ "$mapped" = 1 ]; then
        echo ">>> window mapped — handing over (Ctrl-C to quit)"
        rm -f "$LOG" 2>/dev/null || true
        wait "$PIPE_PID" 2>/dev/null || true   # stay attached for the session
        exit 0
    fi

    # Reap whatever's left and clean transients before the next attempt.
    wait "$PIPE_PID" 2>/dev/null || true
    rm -f "$LOG" 2>/dev/null || true
    cleanup_transients
    attempt=$((attempt+1))
done

echo "error: could not boot a window after $MAX_ATTEMPTS attempts." >&2
echo "       raise BW64_GUI_MAX_ATTEMPTS / BW64_GUI_SILENT_SECS, or the host may be too loaded." >&2
exit 1
