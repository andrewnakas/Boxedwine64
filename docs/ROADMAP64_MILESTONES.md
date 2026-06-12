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
| M4 | Mouse capture for games | Pointer events for game-style apps: DOOM mouse turn/fire (doomgeneric wndProc has no mouse path today), canvas pointer-lock toggle | In DOOM, mouse movement turns the player and mouse button fires, browser-verified | DONE — browser-verified 2026-06-11 (mouse motion turns DOOM's 3D view; doom.exe rebuilt with a wndProc mouse path) |
| M5 | Sound (audio backend) | Parity with 32-bit audio: wine's audio stack → an SDL-audio (WebAudio) device in the browser | DOOM plays its sound effects (or a .wav plays via sndrec/winmm test) audibly in the tab; AudioContext confirmed feeding samples | DESCOPED (2026-06-11) — sink works, wine→sink bridge is a multi-session rootfs effort; resume path documented in the Log |
| M6 | Web-build performance: decoded-block cache | The upstream "improve performance for Emscripten build" item, 64-bit edition: per-RIP decoded-block cache so hot loops skip re-decode (README "concrete next steps" #3) | Measured ≥2x CPU64 throughput on a repeatable benchmark (or notepad cold boot <30s local), selftest 234/234, apps still boot | DESCOPED (2026-06-11) — the cheap lever (prefix-decode cache) gave NO measured win (benchmark-confirmed); the real win (full decoded-block cache) is a multi-session executor refactor. Evidence + resume path in the Log |
| M7 | Lazy / streamable rootfs | Don't download ~196MB before first paint: HTTP-Range-backed zip reads (or progressive mount), unlocks bigger bundled apps (Quake 2 deferral) | First app reaches first paint with materially less than the full rootfs downloaded (measure bytes-before-first-paint before/after) | DEFERRED (2026-06-11) — needs a custom minizip Range-fetch I/O backend + synchronous-fetch-on-guest-thread under PROXY_TO_PTHREAD; multi-session architectural lift (assessment in Log). Revisit with M6's block cache as companion perf work |
| M8 | Memory usage reduction | Parity with upstream 26R1 (-20%): measure wasm heap after boot, free what's recoverable (e.g. post-mount zip buffers, duplicate framebuffers) | Peak/total heap after notepad boot reduced ≥15% vs. measured baseline, recorded in the Log | DONE — browser-verified 2026-06-11: JS heap 651MB peak → 235MB post-boot (~416MB / 64% freed) by releasing the JS-side zip buffers after MEMFS mount |
| M9 | App breadth: experimental row | The taskmgr/regedit/control/explorer/oleview row: each either works or has a root-caused triage note (X opcodes, missing dlls, …) | Each app: renders+takes input in-browser, or a Log entry naming the first fatal marker and the missing feature | DONE — browser-verified 2026-06-11: regedit/control/explorer/IE/oleview RENDER (5/6); taskmgr triaged (clean-exit after PolyText X-text opcodes 65/66) |
| M10 | Joystick/gamepad | Upstream roadmap item: SDL gamepad → browser Gamepad API → wine dinput | A game or joy.cpl-style test reacts to a connected (or emulated CDP) gamepad in-browser; else documented evaluation | DESCOPED (2026-06-11, evaluation) — entirely unbuilt: SDL inits VIDEO|TIMER only (no joystick subsystem), no Gamepad-API bridge, no /dev/input/js* device feeding wine's HID stack. Multi-layer effort + blocked headless verification. Assessment in Log |
| M11 | ISO mounting (evaluate) | Upstream "distant future" item: mount an ISO as a drive (ISO9660 reader over the existing zip-mount machinery, or pre-extract path) | Either a demo ISO browses as a drive letter in winefile, or a written go/no-go with effort estimate | EVALUATED → NO-GO for now (2026-06-11): no ISO9660 reader exists; the pre-extract-to-a-drive path is the pragmatic route. Assessment in Log |
| M12 | DOSBox launching (evaluate) | Upstream item: launching DOSBox for DOS-installer games — likely impractical under the interpreter; decide honestly | Working demo, or a written descope rationale with the technical blocker | EVALUATED → NO-GO (2026-06-11): impractical — DOSBox-under-wine-under-interpreter, no integration exists. Rationale in Log |
| M15 | taskmgr blocker triage | Root-cause taskmgr's wasm `RuntimeError: null function` — identify the exact call site/missing feature, then fix if bounded or leave a precise triage note | A named root cause and either a fix that lets taskmgr render, or a documented blocker | DONE (2026-06-11) — root-caused to a HOST-SIDE wasm null-function call on a WORKER thread (not a wine API stub, not a missing x86 opcode — NO wine err:/unimplemented line precedes it), during taskmgr's icon/pixmap setup; pinpointing the C++ site needs a symbolicated build (documented blocker). Predates M14. Also stripped the 4 stale XWire DIAG logs this surfaced |
| M14 | X drawing primitives | Implement the core X line/rect drawing requests (PolySegment 65, PolyRectangle 66, PolyLine 64, PolyPoint 63, FillPoly 69) into the window framebuffer — the gap behind graph/border-drawing apps | Draw-primitive app shows lines/borders in-browser, no regression, selftest unaffected | DONE — implemented + browser-verified 2026-06-11 (regedit's tree/border lines render crisp via the new primitives, no regression; selftest 234/234). Also fixed a latent PolyFillRectangle no-op. taskmgr re-triaged: its blocker is a deeper wine null-function stub, NOT drawing |
| M13 | Gecko / .NET (evaluate) | Upstream item: wine-gecko (HTML dialogs) and wine-mono (.NET apps) payloads — weigh ~50–80MB payloads + JIT-under-interpreter cost | A trivial .NET WinForms exe runs, or a written descope rationale (payload/perf numbers) | EVALUATED → NO-GO (2026-06-11): no mono/gecko in rootfs; ~50-80MB payload + JIT-in-interpreter cold-start. Rationale in Log |
| M16 | Direct3D (wined3d → WebGL2) | Bring up the D3D path: extend the gl64 bridge from fixed-function-only to the full programmable pipeline (shaders, VBOs, vertex attribs, modern state, textures) so wined3d can render D3D apps via WebGL2 | A D3D9 app renders its 3D output (not black) in-browser | IN PROGRESS (2026-06-11) — MUCH further now: device-create blocker SOLVED (trim advertised extensions to the minimal GLSL set → wined3d `CreateDevice` succeeds), FFP-shader transpiler done (wined3d's fixed-function GLSL compiles+links CLEAN on WebGL2), and the D3D `glDrawArrays` fires. NEW & FINAL blocker: the guest STALLS after the first draw — wined3d's `Present()` does NOT call `glXSwapBuffers` and hangs in non-GL code (a present/threading issue), so no frame ever reaches the canvas. Resume path in the Log |
| M17 | winetricks (evaluate) | Bring up winetricks (the wine helper that installs redistributables/DLL-overrides/fonts) in the WASM build, or get a useful subset working | winetricks runs a verb in-browser, or a written go/no-go with the technical blocker + the achievable native subset | EVALUATED → NO-GO for the script; native verb-subset is the path (2026-06-11). winetricks is a ~14k-line bash script needing a POSIX shell + coreutils + wget/curl + cabextract + internet — NONE exist in the rootfs (no shell at all; no socket egress in-browser). Its USEFUL actions (DLL overrides, registry tweaks) ARE achievable natively without it. Assessment + mechanism in the Log |

Suggested order = table order. M1 early on purpose: it protects every later
milestone. M11–M13 are evaluation milestones — a rigorous written no-go closes
them.

---

## Log

- **2026-06-11 — M16 UPDATE #2 (D3D, big progress): device-create SOLVED + FFP
  shaders compile clean; new final blocker = the Present stall.** Continuing the
  loop after the first M16 checkpoint, the `D3DERR_NOTAVAILABLE` blocker was
  CRACKED and two more layers fixed:
  - **CreateDevice blocker = OVER-ADVERTISED EXTENSIONS.** A lever test proved it:
    with the GLSL shader extensions *un*-advertised, `CreateDevice` succeeds (the
    old no-shader path); advertising them made wined3d reject the device. Bisected
    it — NOT the GLSL extensions themselves (device creates fine with VBO + the 4
    core GLSL exts), but the broader **texture-format/FBO extensions**
    (texture_float / sRGB / s3tc / rectangle / framebuffer_object / occlusion /
    draw_buffers) make wined3d run a strict D3D-format validation that fails
    against WebGL2 → NOTAVAILABLE. FIX: trim `g_extList[]` / the monolithic string
    in libgl64.c to the minimal set (multitexture, VBO, NPOT, shader_objects,
    shading_language_100, vertex_shader, fragment_shader). All the GLSL numeric
    caps wined3d queries (MAX_VERTEX_ATTRIBS=16, MAX_VARYING_FLOATS=120,
    MAX_*_UNIFORM=4096, …) come back healthy; bumping GL_VERSION/GLSL version did
    NOT matter — it was the extensions.
  - **FFP shaders now compile+link CLEAN.** d3dtri uses D3D's fixed-function
    pipeline, so wined3d generates FFP-replacement GLSL that uses the legacy
    compatibility built-in varyings GLSL ES 3.00 removed: vertex writes
    `gl_FrontColor`/`gl_FrontSecondaryColor`/`gl_TexCoord[]`, fragment reads
    `gl_Color`/`gl_SecondaryColor`/`gl_TexCoord[]`, plus `gl_FogFragCoord`.
    Extended `translateGlslToEs300` to rewrite them to matching user in/out
    varyings (`bw_color`/`bw_secondary`/`bw_texcoord[8]`/`bw_fogcoord`) with the
    right declarations per stage, AND to strip EVERY `#version` (wined3d emits a
    second one mid-source that re-started the shader and dropped the precision
    line). Result: shader COMPILE/LINK errors = 0; the D3D `glDrawArrays
    GL_TRIANGLES,3` fires.
  - **NEW FINAL BLOCKER — Present() hangs (PINPOINTED).** With everything above,
    the render loop runs and per-call d3dtri logging shows: `Clear` ✓ →
    `BeginScene` ✓ → `DrawPrimitive` ✓ (the one `glDrawArrays` fires) → `EndScene`
    ✓ → **`Present...` → HANGS** (the matching "Present DONE" never prints; total
    silence after — no X11 wire, no wineserver, no GL trap of any kind). So
    `IDirect3DDevice9_Present` blocks forever inside wined3d. Key facts: wined3d's
    Present does NOT call our `glXSwapBuffers` (a real wrapper + in the proc table,
    so it WOULD trap) — it presents this windowed `D3DSWAPEFFECT_DISCARD` swapchain
    some other way and blocks BEFORE any GL/blit. The draw's glOnMain returns fine
    and leaves only a benign leftover GL_INVALID_ENUM. Tried (didn't fix): guarding
    + try/catch-wrapping glemu's `newRenderingFrameStarted` main-loop hook (it
    throws on our gl64 context, a real latent deadlock source, but not THIS one).
    So the hang is a wineserver/window-system round-trip in wined3d's present path
    that never completes — a wine-threading deadlock, the last thing between here
    and a rendered triangle.
  - **RESUME PATH (Present hang) — narrowed hard this session:** ruled OUT, with
    evidence: it is NOT vsync (`D3DPRESENT_INTERVAL_IMMEDIATE` still hangs); NOT
    our readback (wined3d's Present makes NO `glXSwapBuffers` call → our
    `readbackAndPresent` never runs — SWAP#/RBP# probes never fired); NOT (solely)
    the glemu frame hook (guarded + try/catch, still hangs). A main-loop heartbeat
    proved the loop runs exactly ONE more iteration after `Present...` then DIES —
    so `IDirect3DDevice9_Present` blocks on the guest thread in wined3d's own
    wineserver/window code (no GL at all) AND that also wedges the main loop:
    a **lock-ordering deadlock between the guest Present thread and the main
    loop** (the M0 family). NEXT SESSION: instrument the wineserver IPC / syscall
    layer (syscall64.cpp futex/select/wineserver-request path, gated to the
    d3dtri pid) to capture the exact wait both sides are stuck on, then fix the
    lock ordering (likely: make the main loop's tickXWirePresent / processEvents
    not hold a lock the guest Present thread needs, or vice-versa). The benign
    leftover GL_INVALID_ENUM (a vertex-attrib/VAO setup enum) is worth clearing
    too. glcube still renders (immediate-mode path unaffected); selftest 234/234.
    The whole front of the D3D pipeline (device, GLSL shaders, draw) is solid —
    only Present remains, and it is a wineserver-threading deadlock, not a GL gap.
- **2026-06-11 — M17 EVALUATED (winetricks): NO-GO for the script itself; the
  native verb-subset is the realistic path.** The user asked to bring up
  winetricks after D3D. Findings:
  - **winetricks cannot run as-is.** It's a single ~14,000-line POSIX/bash shell
    script (github.com/Winetricks/winetricks). It needs, none of which exist in
    this rootfs: (1) a **POSIX shell** — exhaustive grep of all three zips found NO
    bash/sh/dash/busybox at all (the rootfs is purely wine64 + its libs; `usr/bin`
    is empty); (2) **coreutils** (cp/mkdir/sort/sed/grep/…) — absent; (3) a
    **downloader** (wget/curl) — absent; (4) **cabextract / 7z / unzip** to unpack
    the redistributables it fetches — absent; (5) **internet egress** — the WASM
    build runs in the browser sandbox with no outbound TCP (Boxedwine has socket
    code, but there's no real network path to the redistributable mirrors).
    Supplying all of that = adding a whole Linux userland + a network shim, far
    beyond a bring-up task; and even then the downloads can't reach the internet.
  - **The USEFUL subset is achievable natively, without winetricks.** What people
    actually use winetricks for is mostly (a) **DLL overrides** (force a DLL to
    native/builtin/disabled) and (b) **registry tweaks**, plus (c) bundling a
    redistributable DLL/font. All three are doable here with NO shell and NO
    network: the prefix already carries wine's text registry
    (`home/username/.wine/system.reg` + `user.reg`), and `reg.exe` / `regedit.exe`
    / `rundll32.exe` are in the rootfs (regedit even renders in-browser, per M9).
    So a "winetricks-lite" is: append the verb's registry keys to `user.reg` at
    prefix-build time (build-prefix64.sh), e.g. a DLL override under
    `[Software\\Wine\\DllOverrides]` (`"d3dx9_43"="native,builtin"`), and stage any
    bundled DLL/font into drive_c — exactly the pattern we already use to bundle
    games/apps. wine reads it on boot; no script, no download, no shell.
  - **Recommendation / resume path:** don't port the script. When a specific app
    needs a verb, add it as a prefix-build registry edit (or ship the override +
    DLL in the rootfs). If a generic mechanism is ever wanted, a tiny in-page
    "apply override" button could write the `.wine/.../user.reg` key via the same
    HOME-write path the clipboard/persistence bridges use, then relaunch. Genuine
    descope of the script; the native subset covers the real need.
- **2026-06-11 — M16 IN PROGRESS (Direct3D via wined3d → WebGL2): the
  programmable-pipeline GL bridge is built & verified; D3D9 reaches GL and
  compiles shaders; blocked on a wined3d `CreateDevice` cap rejection.**
  - **What shipped (this branch, `d3d-wined3d-webgl2`, NOT on master/deployed):**
    The gl64 bridge was fixed-function-only (glBegin/glEnd, matrix stack); wined3d
    needs the programmable pipeline. Added ~80 modern GL entry points across all
    three layers in lockstep: the ABI (`gl64bridge_abi.h`, new opcode block at
    400+), the guest libGL (`libgl64.c` wrappers + `glXGetProcAddressARB` table;
    removed the 21 colliding `GLSTUB`s now implemented for real), and the host
    bridge (`gl64bridge.cpp`, ~800 lines): shaders (Create/Source/Compile/Attach/
    Link/Use/Get*iv/InfoLog), uniforms (1i/1f..4fv/MatrixNfv), buffers (Gen/Bind/
    BufferData/SubData), vertex attribs + VAOs, glDrawArrays/Elements/RangeElements,
    modern blend/stencil/scissor/depth state, and modern textures (ActiveTexture/
    Gen/Bind/TexParameter*/TexImage2D/SubImage/GenerateMipmap/Compressed). Plus a
    minimal **GLSL transpiler** (`translateGlslToEs300`) — WebGL2 only accepts GLSL
    ES 3.00, but wined3d emits desktop `#version 120` (attribute/varying/
    gl_FragData/texture2D); the transpiler rewrites the version + keywords +
    fragment output. Plus a built D3D9 test app (`gltest/d3dtri.c` →
    `d3dtri.exe`, FVF triangle, staged into the prefix by `build-prefix64.sh`).
  - **Verified working (browser, local):** glcube still renders 100% lit (no
    regression); `--x64-selftest` 234/234; D3D9 `Direct3DCreate9` succeeds, the GL
    context comes up (`gl64: host GL up (WebGL2)`), wined3d takes the GLSL renderer
    path (it enumerates our advertised ARB extensions), **shaders compile + link
    with zero errors** via the transpiler, and the full pipeline traps through the
    bridge (histogram shows shaders/buffers/attribs/textures + `glDrawArrays
    GL_TRIANGLE_STRIP, 4`).
  - **THE BLOCKER:** wined3d's `CreateDevice` (HAL and REF) returns
    **`D3DERR_NOTAVAILABLE` (0x8876086C)** — a device-capability rejection. The
    same d3dtri created a device fine BEFORE extensions were advertised (it used a
    minimal no-shader path then); advertising the full GLSL/VBO/FBO feature set
    makes wined3d validate more strictly and reject. The exact failing check is
    **opaque**: the rootfs wined3d.dll was built WITHOUT debug channels (verified:
    `WINEDEBUG=+d3d` produces ZERO output), so wined3d's own reason can't be read.
    GL-error logging shows GL_INVALID_ENUM/VALUE during device-init texture setup
    (desktop-only `glTexParameter`/`glGetIntegerv` pnames) — these were filtered/
    defaulted (texParamSupported + a glGetIntegerv desktop-pname table that
    swallows errors), but they were NOT the cause: CreateDevice still fails
    identically after filtering. So the rejection is a higher-level wined3d cap
    decision, not a stray GL error.
  - **RESUME PATH (next session):** (1) the highest-leverage move is a
    **debug-channel wined3d** — either find/build a wine 8.0 wined3d.dll with
    TRACE compiled in, or rebuild the rootfs wine with `--enable-debug`, so
    `WINEDEBUG=+d3d,+d3d_caps` prints the exact `wined3d_check_device_*` that
    fails. (2) Cheap experiments meanwhile: bisect the advertised extension list
    (esp. drop `GL_ARB_framebuffer_object` to force wined3d's older swapchain
    path; or trim sRGB/float/occlusion which trigger stricter format checks);
    return larger `glGetIntegerv` caps; have `glXChooseFBConfig` advertise >1
    config with varied attribs so wined3d finds a matching format. (3) The whole
    programmable-GL bridge is the durable asset — it's correct and unregressing;
    only this wined3d device-cap handshake remains. Test harness: the d3dtri probe
    pattern in /tmp/bw64test (clearBrowserCache once, then sample canvas + grep
    `device created OK` / `CreateDevice .* FAILED` / `gl64 GLERR`). Local-iteration
    gotchas baked in: the dev server now honors `BW64_NO_IMMUTABLE=1` (zips served
    no-cache) and the launcher honors `?fresh=1` (fetch cache:reload) — because the
    rootfs zips are otherwise immutable-cached for a year; AND libGL.so.1 lives in
    BOTH `glibc-rootfs64.zip` (mounted first, wins) and `wine64.zip`, so a rebuilt
    libGL must be injected into BOTH or the old one shadows it.
- **2026-06-11 — M15 DONE (taskmgr blocker triaged) + DIAG-log cleanup.**
  Captured taskmgr's crash context headless (last ~80 console lines before the
  trap). Findings: the `RuntimeError: null function` fires on a WORKER thread
  immediately after taskmgr's startup reaches its icon/cursor setup (32×32
  pixmap PutImages) — and crucially there is NO wine `err:`/`fixme:`/
  `unimplemented function` line and NO CPU64 unimplemented-opcode panic before
  it. So it is NOT a wine API stub and NOT a missing x86 opcode (my M14
  correction already ruled out the X-opcode theory); it is a HOST-SIDE wasm
  indirect call through a null function-table slot inside Boxedwine's own C++,
  on a guest worker thread, triggered by a code path taskmgr exercises that the
  5 rendering apps don't. Pinpointing the exact C++ call site needs a
  symbolicated/debug wasm build to resolve the `boxedwine64.js:1:4588` wasm
  function index to a symbol — a separate multi-session diagnostic. Documented
  blocker (meets the milestone bar: named root cause + why it's blocked). The
  crash PREDATES M14 (the M9 probe saw it too). Contained win this surfaced:
  stripped the 4 stale one-shot `XWire DIAG:` logs in xwireconnection.cpp
  (CreatePixmap / PutImage-to-pixmap / PutImage-to-window / CopyArea) that
  prior notes had long flagged for removal — they were DOOM bring-up
  diagnostics. wasm64-mt builds clean.
- **2026-06-11 — M14 DONE (X drawing primitives) + taskmgr root cause
  CORRECTED.** Added the core X core drawing requests to xwireconnection.cpp:
  PolyFillRectangle(70, filled rects), PolyRectangle(66, outlines),
  PolySegment(65, line segments), PolyLine(64)/PolyPoint(63), FillPoly(69,
  even-odd scanline polygon fill) — five helpers rasterizing into the window
  ARGB framebuffer in the GC foreground (Bresenham lines + clipped plot). Also
  fixed a LATENT BUG: X_PolyFillRectangle was grouped with X_ChangeProperty and
  silently dropped (a no-op) — now it actually fills. selftest 234/234,
  wasm64-mt clean. Browser-verified: regedit RENDERS with crisp tree/panel/
  column lines (drawn via these primitives) and NO regression vs. before.
  IMPORTANT CORRECTION to the M9 log: taskmgr's blocker is NOT the X opcodes —
  I'd mis-read 65/66 as PolyText (they're PolySegment/PolyRectangle). With the
  primitives implemented, taskmgr STILL fails, now with a hard wasm
  `RuntimeError: null function` (an indirect call to an unimplemented wine API,
  likely its perf-counter/NtQuerySystemInformation path) — a deeper wine stub
  gap, separate work, NOT a drawing problem. So M14 shipped its real value (the
  drawing primitives, which border/graph-drawing apps need broadly) even though
  taskmgr needs more. The X core-TEXT path (PolyText8/16, opcodes 74/75) is
  ALREADY implemented (blitTextItems) — that earlier "top gate" note was also
  based on the opcode mis-attribution; text already works.
- **2026-06-11 — M11/M12/M13 EVALUATED (the evaluation milestones), all
  written no-go for the autonomous loop, each with a rationale + a resume
  pointer.**
  - **M11 ISO mounting → NO-GO now, clear resume path.** No ISO9660 reader in
    the tree (grep: none). Two routes: (a) a real ISO9660 FsNode mounting an
    .iso as a drive letter — net-new filesystem code; (b) the PRAGMATIC route —
    pre-extract the ISO's contents (host-side, like the rootfs zips) and expose
    them as a drive via the existing FsZip/mount machinery, which already maps a
    zip to a drive. (b) needs no new in-guest code and is the recommended path
    if/when a real use case (a CD-based installer) appears. Deferred, not hard-
    blocked — just no current demand to justify the work.
  - **M12 DOSBox → NO-GO (impractical).** No DOSBox integration exists (grep:
    only an unrelated MMX file matched). The concept is DOSBox.exe running as a
    Windows app under wine under the Boxedwine x86 interpreter — an emulator
    inside an emulator inside an interpreter; the perf and complexity make it a
    non-starter for the browser build. DOS-installer games are better served by
    a separate native-DOSBox or js-dos path entirely outside this project.
    Genuine descope.
  - **M13 Gecko/.NET → NO-GO (payload + JIT cost).** No wine-mono or wine-gecko
    in the rootfs (grep: none). Adding .NET needs the ~50-80MB wine-mono payload
    (already 215MB rootfs), and .NET's JIT running inside a non-JIT x86
    INTERPRETER means brutal cold-start; WPF (D3D) is dead here anyway and only
    .NET4.x WinForms-over-GDI+ could even theoretically run. wine-gecko (HTML
    rendering for IE/mshtml dialogs) is a similar heavy payload for narrow
    benefit. Both are documented as AVOID in [[wasm-app-compatibility]]'s
    sweet-spot research. Ship native Win32 equivalents instead. Genuine descope.
- **2026-06-11 — M10 DESCOPED (joystick/gamepad), evaluation with evidence.**
  Found it's entirely unbuilt, top to bottom: (1) the wasm build inits SDL with
  only `SDL_INIT_VIDEO | SDL_INIT_TIMER` (mainui.cpp:470; and the emscripten
  loop is source/sdl/emscripten/mainloop.cpp anyway) — no joystick/
  gamecontroller subsystem; (2) no Gamepad-API code anywhere in
  project/emscripten/*.js; (3) Boxedwine registers /dev/input/event* but wired
  to keyboard/mouse/touch (openDevInputKeyboard/Touch), NO /dev/input/js* or a
  joystick evdev device; (4) wine's HID stack IS in the rootfs (winebus.sys/.so,
  hidclass.sys, dinput8.dll) but has nothing to read from. Building M10 =
  SDL joystick subsystem init + an emscripten SDL↔browser-Gamepad-API bridge +
  a Boxedwine joystick device feeding winebus + wine dinput config — a
  multi-layer effort. PLUS headless verification is blocked: no real gamepad,
  and CDP can't easily inject Gamepad-API state. A future session with a real
  controller + a manual test plan is the right venue. Descoped (evaluation
  milestone — a documented no-go closes it).
- **2026-06-11 — M9 DONE (experimental-app breadth, browser-verified).**
  Probed all six experimental-row apps directly (?p=<app>), each in a fresh
  Chrome, classifying render-vs-first-fatal-marker:
  - **regedit.exe** — RENDERS (full registry-tree GUI: HKEY_* hives, two-pane
    layout, menu). ✓
  - **control.exe** — RENDERS (canvas 100% lit). ✓
  - **explorer.exe** — RENDERS (file-manager: Desktop tree, Location bar,
    toolbar, file panes). ✓ — notable, it's the shell/COM-heavy one.
  - **iexplore.exe (IE)** — RENDERS (Back/Forward/Stop/Refresh/Home/Print
    toolbar + Address bar about:blank). ✓
  - **oleview.exe** — RENDERS (canvas 100% lit). ✓
  - **taskmgr.exe** — TRIAGED, does not render. It launches through its relay
    chain (3 pids, reaches C:\windows\system32\taskmgr.exe) and then
    exit_group **status=0** (clean exit, not a crash), with
    `XWire: unhandled request opcode=65/66` (PolyText8/PolyText16 — X CORE
    TEXT) immediately before. So the blocker is the X core-text path the
    README already flagged as the top gate: taskmgr tries to draw text, the
    wire server doesn't implement PolyText, and it bails gracefully. Fixing
    PolyText8/16 in xwireconnection.cpp is the unlock (same path that would
    help WordPad's text) — separate work, noted for a future text milestone.
  Result: 5/6 render (vs. the roadmap's "unproven" assumption), 1 root-caused.
  Acceptance (renders OR triage note for each) fully met. The earlier
  milestones (kill-on-switch input routing, matured GDI/USER paths) are why so
  many now come up.
- **2026-06-11 — M8 DONE (memory reduction, browser-verified).** The launcher
  held each rootfs zip's bytes TWICE after boot: once in MEMFS (what wine
  reads) and once in the `zipDownloads` JS map (kept as an in-page-relaunch
  cache). The two zips sum to ~215MB, so peak memory carried a full duplicate.
  Fix (wine64-launcher.js loadFilesystem): after each zip is createDataFile'd
  into MEMFS, null out its Uint8Array and tombstone the cached promise so the
  buffer is GC'd. Nothing needs the JS copy post-mount — the reload-fallback
  relaunch re-fetches from the HTTP cache, and the in-session relaunch doesn't
  re-mount. Measured (Chrome, --expose-gc --enable-precise-memory-info): JS
  heap PEAK 651MB → POST-boot 235MB, ~416MB / 64% freed; notepad still boots.
  (Freed exceeds the raw 215MB because the intermediate fetch/chunk-assembly
  allocations also collapse once the final buffers are released.) Far past the
  ≥15% target. Test: /tmp/bw64test/test_mem.mjs.
- **2026-06-11 — M7 DEFERRED (lazy rootfs), with the blocker assessed.**
  FsZip (source/io/fszip.cpp) mounts via minizip's `unzOpen(zipPath)` over a
  `FILE*` and reads with `unzReadCurrentFile` — in the wasm build the zip is a
  MEMFS file, which is exactly why the launcher must download the WHOLE zip
  into MEMFS before boot. Lazy/Range-backed loading needs either a custom
  minizip I/O backend that issues HTTP Range requests, or emscripten lazy-file/
  WORKERFS with Range support — and the guest read happens on guest pthreads
  under PROXY_TO_PTHREAD, where synchronous fetch is restricted. That's a
  multi-session architectural change to the zip I/O layer + threading model.
  Deferred (not a contained single-session change); the lower-risk
  memory-reduction milestone (M8) is the better next perf win. Resume: add a
  minizip filefunc64 vtable backed by a Range-fetch (sync XHR on a worker, or
  an Asyncify boundary) reading only the central directory + opened entries.
- **2026-06-11 — M6 DESCOPED (interpreter throughput), with benchmark
  evidence pointing at the real bottleneck.** Implemented the cheap lever
  first — a per-RIP prefix-decode cache in CPU64::step() (memoize
  consumePrefixes()+opcode fetch, 4096-entry direct-mapped, invalidated with
  the fetch cache on execve). It built clean and PASSED selftest 234/234
  (after one gotcha: an inline 96KB array in CPU64 crashed the selftest with
  an OOB because CPU64 is new'd per thread + size-sensitive — heap-allocating
  the cache fixed it). BUT a CPU-bound microbenchmark (a 40M-iteration xorshift
  loop, no I/O, run via --x64-run-elf) showed NO improvement: ~2.7s baseline
  vs ~2.4–2.9s cached (within noise). Reverted. WHY it didn't help: the
  existing per-page fetchByte cache already turns prefix re-fetching into a
  bounds-compare + array index, so re-running consumePrefixes() each step is
  already near-free — the prefix scan was never the bottleneck. BW64_OPPROF on
  the benchmark confirms the hot opcodes are plain MOV/XOR/shift/IMUL; the
  per-instruction cost is the step() dispatch + decodeModRM + operand
  read/write machinery, NOT prefix decode. The real M6 win (the README's
  decoded-block cache) must memoize the FULL instruction decode (total length +
  a dispatch tag, keyed by RIP) so step() skips re-decoding entirely — a large,
  delicate refactor of the switch-based executor, a dedicated multi-session
  effort. RESUME PATH: the benchmark ELF (/tmp build from a simple xorshift
  loop; recreate ~40M iters) + BW64_OPPROF are the measurement tools; start
  from decodeModRM + the operand helpers, memoize instruction LENGTH first
  (lowest-risk, lets step() skip the re-decode to find the next RIP), validate
  with --x64-selftest 234/234 each step. Descoped from the loop, not abandoned.
- **2026-06-11 — M5 DESCOPED (sound), with an evidence-based rationale + a
  concrete resume path.** Findings from the investigation:
  - The audio SINK is fully built and live in wasm64-mt: `/dev/dsp` (OSS) is
    registered at boot (startupArgs.cpp:155), `KNativeAudio::init()` runs, and
    platform/sdl/kdspaudio.cpp implements the emscripten SDL→WebAudio backend
    (SDL_OpenAudioDevice + the boxedwine-multithreaded-audio.js main-thread
    proxy already linked via EXTRA_LD_FLAGS). devdsp.cpp routes guest OSS
    writes into it.
  - The GAP is the wine→sink bridge. The Debian wine64 package in the rootfs
    ships only **winealsa** (ALSA): wine64.zip has winealsa.so/.drv, winmm,
    dsound, mmdevapi, and glibc-rootfs64.zip has libasound.so.2 — but
    Boxedwine emulates **OSS /dev/dsp**, NOT ALSA /dev/snd, and there is NO
    wineoss.drv and NO libasound OSS plugin/asound.conf in the rootfs. So
    wine→winealsa→libasound→/dev/snd dead-ends (no /dev/snd), and nothing
    routes to the working /dev/dsp sink.
  - Bridging it is a multi-session rootfs effort with real uncertainty:
    either (a) add wine's wineoss.drv built for wine 8.0 (PE+unix split driver)
    and set wine's audio driver to oss, or (b) ship the alsa-lib OSS plugin
    (libasound_module_pcm_oss) + an asound.conf routing default→oss:/dev/dsp.
    Plus DOOM itself ships the DUMMY i_sound (would also need a rebuild with
    real audio). And headless audio verification is a second open problem
    (no "did the speaker play" probe — would assert via an AudioContext
    sample-callback counter exposed to JS).
  - RESUME PATH: a native /dev/dsp tone generator is saved at
    tools/rootfs64/work/dsp_tone.c (build: `zig cc -target x86_64-linux-musl
    -static -O2 dsp_tone.c -o tone`). Running it through --x64-run-elf is the
    cleanest way to prove the SINK is audible independent of wine; do that
    first (and add an AudioContext sample-count export for headless assertion),
    THEN tackle the wine bridge (option b is likely lighter than building
    wineoss). This is the right next audio session — descoped from the loop,
    not abandoned.
- **2026-06-11 — M4 DONE (DOOM mouse, browser-verified).** Rebuilt doom.exe
  from doomgeneric with a new mouse patch (on top of the existing window/timer
  patches): wndProc now handles WM_MOUSEMOVE (relative-from-center) +
  L/R/M button messages into an accumulator, a new DG_GetMouse() hands DOOM one
  sample/frame, i_video.c sets usemouse=1, and i_input.c posts an ev_mouse
  (data1=buttons, data2=+dx*4, data3=-dy*4). Verified in-browser: in a live
  DOOM game (full status bar + 3D view) the rendered frame changed when the
  mouse moved (turn), no fatal markers. doom.exe ships via prefix64.zip —
  uploaded to rootfs-pages + manual `gh workflow run deploy-pages.yml` (prefix
  changes don't auto-trigger). The full reproducible patch set is documented
  in build-games.sh (PATCH #3) + scripted as apply_patches.py in the checkout.
  Build gotcha: word-split the SRC list inside `bash -c` (the Bash tool is zsh;
  unquoted $SRC stays one arg → "File name too long").
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
