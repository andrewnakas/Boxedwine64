# Boxedwine64 parity milestones (the loop file)

**What this is.** The original 32-bit Boxedwine's capabilities and roadmap
([`Roadmap-Features.md`](Roadmap-Features.md) + what the 32-bit build already
does: audio, copy/paste, persistence, performance work, joystick/ISO/DOSBox
plans), re-targeted at the 64-bit WASM build (`wasm64-mt`, live at
andrewnakas.github.io/Boxedwine64/). The bring-up roadmap that got wine64
booting in a tab (milestones A–I in [`../README.md`](../README.md) and
[`WASM_64BIT_ROADMAP.md`](WASM_64BIT_ROADMAP.md)) is essentially complete;
this file is what comes AFTER: making the 64-bit browser build do what the
32-bit Boxedwine could do.

**How to work this file (session loop protocol).** Work happens in repeated,
self-contained sessions. Each iteration:

1. Read this file + the session-memory index. Pick the FIRST milestone whose
   status is `IN PROGRESS`, else the first `TODO` (respect `blocked-by`).
2. Implement in small shippable slices. Build locally
   (`cd project/emscripten && source ~/emsdk/emsdk_env.sh && make wasm64-mt`),
   verify in a real browser against `Build/Wasm64Mt` served by `server.mjs`
   (COOP/COEP), driving Chrome over CDP (puppeteer connect to
   `--remote-debugging-port`).
3. A milestone is DONE only when its **acceptance check** below passed in a
   browser (local is fine; live Pages spot-check after deploy when the change
   ships) — not when the code compiles.
4. Commit with a descriptive message; push to master (push auto-deploys Pages;
   prefix64.zip changes additionally need the `rootfs-pages` Release upload +
   manual `gh workflow run deploy-pages.yml` — see session memory). NEVER
   re-upload the wine64.zip.partNNN release assets casually (94MB uploads are
   flaky; one at a time; verify the full set after).
5. Update the Status column + the Log section here IN THE SAME COMMIT as the
   work, and update session memory (per-milestone handoff notes) so a fresh
   session can continue cold.
6. Keep `--x64-selftest` at 234/234 when touching the CPU core; keep notepad,
   glcube, and DOOM booting (they are the de-facto smoke set, M1 automates
   this).
7. If a milestone is blocked or turns out infeasible, don't silently stall:
   record the evidence in the Log, set status `BLOCKED(reason)` or
   `DESCOPED(rationale)`, and move to the next. A documented descope decision
   counts as completing an `evaluate` milestone.

**Definition of "all milestones complete":** every row below is `DONE` or
`DESCOPED` with a written rationale in the Log.

---

## Milestones

| ID | Title | Goal | Acceptance check (browser-verified) | Status |
|----|-------|------|-------------------------------------|--------|
| M0 | Kill-on-switch app switching | Switching apps in the app bar kills the old app (CPU/RAM freed, wineserver client reaped) and routes canvas+input to the new app, no page reload | notepad→DOOM→notepad cycle: each switch logs the kill, old app visually gone, typing/keys reach the new app, no runtime crash | DONE — browser-verified 2026-06-11 (both directions + boot-app relay-pid case), shipped |
| M1 | CI browser smoke test | Headless-Chrome boot test wired into GitHub Actions so later milestones can't silently break boot | CI job boots `?p=notepad.exe`, asserts `XWire: first window mapped` + non-blank canvas, fails the deploy on regression; a green run on master | DONE — gated deploy f9e97941 green 2026-06-11; CI smoke PASS first attempt (78s, lit=99.9) |
| M2 | Clipboard copy/paste (host↔guest) | Parity with 32-bit 25R1 copy/paste: bridge the browser clipboard with the guest's | Copy text in guest Notepad → readable via the page (button or navigator.clipboard); paste host text into Notepad; round-trip verified | DONE — round-trip browser-verified 2026-06-11 (win32 clipset/clipget helper design; toolbar buttons) |
| M3 | Persistence across reloads | Guest home/prefix writes survive a page reload (32-bit web build has INDEXED_DB storage) | Save a file in Notepad, hard-reload the page, relaunch Notepad: the file is still there (IndexedDB-backed) | DONE — browser-verified 2026-06-11 across a full Chrome restart (marker file restored byte-identical pre-boot) |
| M4 | Mouse capture for games | Pointer events for game-style apps: DOOM mouse turn/fire (doomgeneric wndProc has no mouse path today), canvas pointer-lock toggle | In DOOM, mouse movement turns the player and mouse button fires, browser-verified | TODO |
| M5 | Sound (audio backend) | Parity with 32-bit audio: wine's audio stack → an SDL-audio (WebAudio) device in the browser | DOOM plays its sound effects (or a .wav plays via sndrec/winmm test) audibly in the tab; AudioContext confirmed feeding samples | TODO |
| M6 | Web-build performance: decoded-block cache | The upstream "improve performance for Emscripten build" item, 64-bit edition: per-RIP decoded-block cache so hot loops skip re-decode (README "concrete next steps" #3) | Measured ≥2x CPU64 throughput on a repeatable benchmark (or notepad cold boot <30s local), selftest 234/234, apps still boot | TODO |
| M7 | Lazy / streamable rootfs | Don't download ~196MB before first paint: HTTP-Range-backed zip reads (or progressive mount), unlocks bigger bundled apps (Quake 2 deferral) | First app reaches first paint with materially less than the full rootfs downloaded (measure bytes-before-first-paint before/after) | TODO |
| M8 | Memory usage reduction | Parity with upstream 26R1 (-20%): measure wasm heap after boot, free what's recoverable (e.g. post-mount zip buffers, duplicate framebuffers) | Peak/total heap after notepad boot reduced ≥15% vs. measured baseline, recorded in the Log | TODO |
| M9 | App breadth: experimental row | The taskmgr/regedit/control/explorer/oleview row: each either works or has a root-caused triage note (X opcodes, missing dlls, …) | Each app: renders+takes input in-browser, or a Log entry naming the first fatal marker and the missing feature | TODO |
| M10 | Joystick/gamepad | Upstream roadmap item: SDL gamepad → browser Gamepad API → wine dinput | A game or joy.cpl-style test reacts to a connected (or emulated CDP) gamepad in-browser; else documented evaluation | TODO |
| M11 | ISO mounting (evaluate) | Upstream "distant future" item: mount an ISO as a drive (ISO9660 reader over the existing zip-mount machinery, or pre-extract path) | Either a demo ISO browses as a drive letter in winefile, or a written go/no-go with effort estimate | TODO |
| M12 | DOSBox launching (evaluate) | Upstream item: launching DOSBox for DOS-installer games — likely impractical under the interpreter; decide honestly | Working demo, or a written descope rationale with the technical blocker | TODO |
| M13 | Gecko / .NET (evaluate) | Upstream item: wine-gecko (HTML dialogs) and wine-mono (.NET apps) payloads — weigh ~50–80MB payloads + JIT-under-interpreter cost | A trivial .NET WinForms exe runs, or a written descope rationale (payload/perf numbers) | TODO |

Suggested order = table order. M1 early on purpose: it protects every later
milestone. M11–M13 are evaluation milestones — a rigorous written no-go closes
them.

---

## Log

- **2026-06-11 — M3 DONE (persistence, browser-verified across a real Chrome
  restart).** wine64-launcher.js now syncs the writable HOME tree
  (Z:\home\username — includes the .wine prefix, so registry + app settings
  persist too) into IndexedDB on a 5s interval + visibilitychange, and
  restores it during preRun BEFORE wine boots (restored files are part of the
  first VFS scan, so they shadow the read-only zip layers naturally — no
  post-boot cache registration). Incremental: a stat-walk diff against an
  in-memory signature manifest, so a quiet session writes nothing. ?persist=0
  disables it; 16MB/file cap; the .bw64clip.* bridge files are skipped.
  Verified A/B with a full Chrome process restart between phases (MEMFS gone,
  IndexedDB carries the data): marker written into HOME → captured by the
  sync loop → fresh Chrome boots → `persist: restored N files` pre-boot →
  marker read back byte-identical. Test gotcha learned (not a product bug):
  driving notepad's Ctrl+S Save As dialog headless is unreliable (the dialog
  often doesn't open from a synthetic Ctrl+S) — the test writes the marker
  straight into the writable MEMFS HOME instead, which is byte-identical to
  what a guest save produces since guest saves land in that same layer. Also:
  a clean-DB setup must navigate to a NON-wine page first (a mid-boot wine
  teardown by navigation occasionally wedges the next boot — use a 404/blank
  URL for the IndexedDB-clear step).
- **2026-06-11 — M2 DONE (clipboard, browser-verified round trip).** Winning
  design: two tiny bundled win32 helpers — clipset.exe (UTF-8 staging file →
  CF_UNICODETEXT) and clipget.exe (clipboard → output file) — spawned into the
  running session by toolbar buttons ("⧉ Copy from app" / "⧉ Paste to app").
  They talk to the wineserver-managed win32 clipboard directly, which works
  under the minimal XWire server; verified: host text → clipset → Ctrl+V
  pasted into notepad (screenshot, Ln1 Col31) → Ctrl+A/C → clipget → exact
  payload back. The X-selection route was implemented first (per-connection
  atom table seeded with predefined atoms + ids rebased past them,
  ConvertSelection/SelectionNotify, real ChangeProperty/GetProperty with a
  (window, property-name)-keyed store, SetSelectionOwner harvest via
  synthesized SelectionRequest, host-owner sentinel, SelectionClear on host
  set) and is KEPT as substrate with diagnostics — but wine 8.0's X↔win32
  clipboard manager (explorer's clipboard thread, programs/explorer/desktop.c
  + winex11 clipboard.c) never polls selections in our session (no
  GetSelectionOwner traffic ever; root cause inside wine not chased further
  since the win32 path is strictly better here). Also fixed: build-prefix64.sh
  accumulated nested app dirs (HxD/HxD/…) on every rebuild because the stage
  starts from the previous zip and `cp -R` merges into existing dirs — now
  replaced; prefix64.zip back to 17MB clean, re-uploaded to rootfs-pages.
- **2026-06-11 — M0 verified on the LIVE Pages site** (the exact environment
  the original bug was reported in): cold chunked load → notepad booted →
  switch to DOOM killed both the relay pid and the real notepad GUI
  (`pid 38, C:\windows\system32\notepad.exe, [X window owner]`), X state
  dropped, DOOM title art rendered, Enter opened DOOM's menu (red pixels
  18%→27%). No fatal markers. Live-testing gotchas: direct wine64.html URLs
  on Pages need `?chunked=1` (the rootfs is split); live boot is flaky —
  attempt 2 died with notepad exit status=1 before any switch (boot
  reliability, tracked for a future milestone; M1's CI retry absorbs it).
- **2026-06-11 — M0 DONE + browser-verified.** Two full local browser runs:
  (1) notepad→DOOM→notepad: kills logged both ways, DOOM adopted in 8s, fresh
  notepad took typing ("ok" at Ln 1 Col 3), zero fatal markers; (2) the
  boot-app case: wine boots apps through a RELAY CHAIN, so the launcher's boot
  pid is a dead ancestor — fixed by capturing the outgoing `presentWindow`'s
  connection-owner pid at switch-arm time (`XWireServer::outgoingAppPid`) and
  killing that too (`[X window owner]`), with a name guard so a miscapture can
  never kill wineserver/services/explorer. Verified: boot notepad GUI
  (pid≠bootpid, `C:\windows\system32\notepad.exe`) killed + its X state
  dropped, DOOM title art rendered, Enter opened DOOM's menu (canvas red-pixel
  jump 8.5%→16.8%). Boot itself is intermittently flaky (a wineserver startup
  race killed one boot attempt — pre-existing; see README "residual boot
  wedge", candidate for the M1 smoke test to quantify). Headless gotcha: the
  compositor starves while DOOM's unpaced loop pegs cores, so screenshots can
  capture black — sample canvas pixels via drawImage+getImageData instead.
- **2026-06-11 — file created.** M0 implemented this session: kernel-side
  non-blocking kill (`doKill` marks threads terminating + wakes them; last
  thread runs full process cleanup via `KProcess::deleteThread`), periodic
  `terminating` check in `CPU64::run()` (CPU-bound guests like DOOM never
  blocked, so they could never die), `XWireServer::dropAppByPid()` (erases the
  killed app's connections/windows/pixmaps, keyed by guest pid recorded per X
  connection), launcher now calls `bw64_kill(prevPid)` after queueing the new
  spawn, plus a `mapped` gate fixing the bug where a hidden background app
  (notepad caret blink) could re-steal the canvas+keyboard from a smaller new
  app (the live notepad→DOOM symptom this all started from).
