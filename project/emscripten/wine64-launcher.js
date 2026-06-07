// wine64-launcher.js — minimal browser launcher for the 64-bit Boxedwine guest.
//
// Unlike boxedwine-shell.js (the full 32-bit web shell with its many query-param
// quirks and a single boxedwine.zip), this loads the two LAYERED 64-bit rootfs
// zips and passes the exact arguments the native headless command uses to boot
// real wine64 (see README "Run real wine64 headless"):
//
//   -root /root -zip glibc-rootfs64.zip -zip wine64.zip
//   -env WINEDLLPATH=... [-env HOME=... -env WINEPREFIX=... -env WINESERVER=...]
//   /usr/lib/wine/wine64 <program...>
//
// Query params (all optional):
//   ?p=--version            program + args for wine64 (default: --version)
//                           space-separated; URL-encode spaces as %20.
//                           When a program (other than --version) is given, the
//                           launcher mounts a third zip — prefix64.zip — that
//                           carries a PRE-BOOTED .wine prefix (registry +
//                           dosdevices/c:,z: + glcube.exe) at /home/username, and
//                           runs the program with HOME=/home/username /
//                           WINEPREFIX=/home/username/.wine. wine sees an existing
//                           prefix and SKIPS `wineboot --init`. This sidesteps the
//                           unsolved in-browser wineserver-daemonize boot (see
//                           ?boot=1) and is the path that can actually reach pixels
//                           (e.g. ?p=glcube.exe).
//   ?boot=1                 boot the prefix from scratch: runs `wineboot --init`
//                           with HOME=/winePrefix. NOTE: in-tab this still STALLS —
//                           wineserver64 exits status=0 right after launch (no
//                           surviving daemon), dosdevices/c: is never created, and
//                           every spawned wine process loops on ENOENT. Kept as a
//                           progress probe for the wineserver-IPC roadmap work; use
//                           ?p=<prog> (pre-booted prefix) to actually run programs.
//   ?base=<url>             base URL the zips are fetched from (default: "./").
//   ?novideo=1              pass -novideo (headless; no SDL window).
//   ?chunked=1              fetch each rootfs zip via a <zip>.manifest.json that
//                           lists <zip>.partNNN pieces, and concatenate them in
//                           the browser before mounting. This is how the GitHub
//                           Pages deploy serves the 196 MB wine64.zip: Pages caps
//                           files at 100 MB and (being unable to set CORP headers)
//                           can't satisfy COEP require-corp for a cross-origin
//                           Release asset, so the zip is split into <100 MB
//                           SAME-ORIGIN parts (tools/rootfs64/split-rootfs.sh).
//                           Without this flag (e.g. local `node server.mjs`) the
//                           whole zips are fetched directly — unchanged behaviour.
//
// Examples:
//   wine64.html                      -> wine64 --version  (fast correctness boot)
//   wine64.html?boot=1               -> wineboot --init   (stalls; roadmap probe)
//   wine64.html?p=glcube.exe         -> wine64 glcube.exe  (pre-booted prefix; GL)
//   wine64.html?p=notepad.exe        -> wine64 notepad.exe (pre-booted prefix)
//
// IN-PAGE APP SWITCHING (no full reload): after the first boot, the fetched
// rootfs zip bytes are kept in `zipCache` (module scope, survives across
// relaunches). window.launchApp("<prog>") tears down the running wasm instance
// — terminate its pthreads, drop the runtime globals, and REPLACE the canvases
// (an OffscreenCanvas transferred to a pthread via OFFSCREENCANVASES_TO_PTHREAD
// can't be transferred twice, so each relaunch needs a fresh #gl64canvas /
// #canvas) — then boots a fresh module against the cached bytes (zero refetch).
// The app bar in wine64.html calls launchApp(); this is NOT in-session process
// spawning (that needs the unsolved in-browser wineserver daemon), it's a fast
// local re-boot of wine with a new program, skipping the ~200MB download.

(function () {
    "use strict";

    var GLIBC_ZIP = "glibc-rootfs64.zip";
    var WINE_ZIP = "wine64.zip";
    var PREFIX_ZIP = "prefix64.zip"; // pre-booted .wine prefix (+glcube.exe), ~257KB
    var ROOT = "/root";
    var WINE64 = "/usr/lib/wine/wine64";
    // The pre-booted prefix in PREFIX_ZIP lives here (dosdevices c: -> ../drive_c,
    // z: -> /). Running a program with these is what lets wine skip wineboot.
    var PREFIX_HOME = "/home/username";
    var PREFIX_WINE = "/home/username/.wine";

    // --- query params -------------------------------------------------------
    function param(key) {
        var m = new RegExp("[?&]" + key + "=([^&#]*)").exec(window.location.search);
        return m ? decodeURIComponent(m[1].replace(/\+/g, " ")) : null;
    }
    var BASE = param("base");
    if (BASE === null) BASE = "./";
    if (BASE.length && !BASE.endsWith("/")) BASE += "/";
    var CHUNKED = param("chunked") === "1"; // fetch zips as <zip>.partNNN via manifest

    // --- current run configuration ------------------------------------------
    // These describe the run being booted RIGHT NOW. The bare-page defaults come
    // from the query string; launchApp() rewrites them per relaunch (no reload).
    var DO_BOOT, NOVIDEO, PROG, USE_PREFIX;
    function applyRunConfig(prog, opts) {
        opts = opts || {};
        PROG = prog;
        DO_BOOT = !!opts.boot;
        NOVIDEO = !!opts.novideo;
        // Run a real program against the pre-booted prefix when a program (other
        // than the bare --version correctness boot) is named and we're not booting.
        USE_PREFIX = !DO_BOOT && PROG && PROG.length && PROG !== "--version";
    }
    // Initial config from the URL (?boot / ?novideo / ?p).
    applyRunConfig(param("p"), { boot: param("boot") === "1", novideo: param("novideo") === "1" });

    // --- rootfs byte cache --------------------------------------------------
    // The big win for in-page app switching: keep the fetched zip bytes here so a
    // relaunch never re-downloads the ~200MB rootfs. Keyed by zip name -> Uint8Array.
    var zipCache = Object.create(null);
    var relaunchCount = 0; // 0 = first boot from the page load; >0 = launchApp reboot

    // --- DOM hooks ----------------------------------------------------------
    var statusElement = document.getElementById("status");
    var progressElement = document.getElementById("progress");
    var spinnerElement = document.getElementById("spinner");

    function setStatusText(text) {
        if (statusElement) statusElement.innerHTML = text;
    }

    // --- build the wine64 argv ----------------------------------------------
    function buildArguments() {
        var args = ["-root", ROOT, "-zip", GLIBC_ZIP, "-zip", WINE_ZIP];
        // The pre-booted prefix is a third overlay; mount it only when we'll use
        // it, so the bare --version / ?boot=1 paths stay byte-for-byte unchanged.
        if (USE_PREFIX) args.push("-zip", PREFIX_ZIP);
        if (NOVIDEO) args.push("-novideo");

        if (DO_BOOT) {
            // Full prefix bring-up + wineserver handshake.
            args.push("-env", "HOME=/winePrefix");
            args.push("-env", "WINEPREFIX=/winePrefix/.wine");
            args.push("-env", "WINESERVER=/usr/lib/wine/wineserver64");
            args.push("-env", "WINEDLLPATH=/usr/lib/x86_64-linux-gnu/wine");
            args.push(WINE64, "wineboot", "--init");
        } else if (USE_PREFIX) {
            // Run a program against the PRE-BOOTED prefix (prefix64.zip). HOME +
            // WINEPREFIX point at the existing /home/username/.wine, so wine finds
            // a populated prefix and does NOT run wineboot --init.
            args.push("-env", "HOME=" + PREFIX_HOME);
            args.push("-env", "WINEPREFIX=" + PREFIX_WINE);
            args.push("-env", "WINESERVER=/usr/lib/wine/wineserver64");
            args.push("-env", "WINEDLLPATH=/usr/lib/x86_64-linux-gnu/wine");
            args.push(WINE64);
            PROG.split(/\s+/).forEach(function (tok) {
                if (tok.length) args.push(tok);
            });
        } else {
            // Bare wine64 --version (correctness boot, no prefix needed).
            args.push("-env", "WINEDLLPATH=/usr/lib/x86_64-linux-gnu/wine");
            args.push("-env", "WINESERVER=/usr/lib/wine/wineserver64");
            args.push(WINE64);
            var prog = (PROG && PROG.length) ? PROG : "--version";
            prog.split(/\s+/).forEach(function (tok) {
                if (tok.length) args.push(tok);
            });
        }
        return args;
    }

    // --- fetch one URL to a Uint8Array, streaming progress ------------------
    // `baseReceived` lets callers offset the progress so a multi-part download
    // reports cumulative bytes against a known grand total.
    function fetchBytes(url, label, onProgress, baseReceived, grandTotal) {
        baseReceived = baseReceived || 0;
        return fetch(url).then(function (resp) {
            if (!resp.ok) throw new Error("fetch " + url + " -> HTTP " + resp.status);
            var partTotal = Number(resp.headers.get("Content-Length")) || 0;
            var total = grandTotal || partTotal;
            if (!resp.body || !partTotal) {
                return resp.arrayBuffer().then(function (buf) {
                    var b = new Uint8Array(buf);
                    if (onProgress) onProgress(label, baseReceived + b.length, total);
                    return b;
                });
            }
            var reader = resp.body.getReader();
            var chunks = [];
            var received = 0;
            return (function pump() {
                return reader.read().then(function (r) {
                    if (r.done) {
                        var out = new Uint8Array(received);
                        var off = 0;
                        chunks.forEach(function (c) { out.set(c, off); off += c.length; });
                        return out;
                    }
                    chunks.push(r.value);
                    received += r.value.length;
                    if (onProgress) onProgress(label, baseReceived + received, total);
                    return pump();
                });
            })();
        });
    }

    function dropBytesIntoVfs(name, bytes) {
        try {
            Module.FS.createDataFile("/", name, bytes, true, true);
        } catch (e) {
            console.log("createDataFile " + name + " failed: " + e);
            throw e;
        }
        // Cache the bytes so a later in-page relaunch (launchApp) can skip the
        // network entirely and just re-drop them into the fresh instance's VFS.
        zipCache[name] = bytes;
        console.log("loaded " + name + " (" + bytes.length + " bytes) into VFS");
        return bytes.length;
    }

    // --- fetch a zip into the Emscripten VFS with progress -------------------
    // Two paths, selected by the ?chunked flag:
    //   * whole-file: fetch BASE/<name> directly (local `node server.mjs`).
    //   * chunked: fetch BASE/<name>.manifest.json, then each listed part, and
    //     concatenate them into the original <name> bytes. This is how GitHub
    //     Pages serves wine64.zip (split <100 MB, same-origin) — see
    //     tools/rootfs64/split-rootfs.sh and ?chunked above.
    // Both end by createDataFile()ing the *original* zip name into the VFS, so the
    // wine64 argv (`-zip <name>`) is identical regardless of how it was fetched.
    function fetchZipToVfs(name, onProgress) {
        // In-page relaunch: bytes already in memory from a prior boot — re-drop
        // them into the new instance's VFS without touching the network.
        if (zipCache[name]) {
            var cached = zipCache[name];
            if (onProgress) onProgress(name, cached.length, cached.length);
            dropBytesIntoVfs(name, cached);
            return Promise.resolve(cached.length);
        }
        if (!CHUNKED) {
            return fetchBytes(BASE + name, name, onProgress).then(function (bytes) {
                return dropBytesIntoVfs(name, bytes);
            });
        }
        // Chunked: read the manifest, then pull and stitch the parts.
        return fetch(BASE + name + ".manifest.json").then(function (resp) {
            if (!resp.ok) throw new Error("fetch " + name + ".manifest.json -> HTTP " + resp.status);
            return resp.json();
        }).then(function (manifest) {
            var parts = manifest.parts || [name];
            var total = Number(manifest.totalBytes) || 0;
            var out = new Uint8Array(total);
            var offset = 0;
            // Fetch parts sequentially (keeps peak memory to ~one part + the
            // assembled buffer, and reports monotonic cumulative progress).
            return parts.reduce(function (chain, part) {
                return chain.then(function () {
                    var atStart = offset;
                    return fetchBytes(BASE + part, name, onProgress, atStart, total)
                        .then(function (bytes) {
                            out.set(bytes, offset);
                            offset += bytes.length;
                        });
                });
            }, Promise.resolve()).then(function () {
                if (total && offset !== total) {
                    throw new Error(name + ": assembled " + offset + " bytes, manifest said " + total);
                }
                return dropBytesIntoVfs(name, out);
            });
        });
    }

    // --- writable WINEPREFIX -----------------------------------------------
    // Guest "/" maps to the Emscripten MEMFS dir passed via -root (ROON = /root).
    // wine64 chdir()s into WINEPREFIX before creating it, so HOME (/winePrefix)
    // and the prefix dir must already exist as writable dirs. Create them in the
    // host MEMFS under ROOT so the guest sees writable /winePrefix/.wine.
    function mkdirHost(path) {
        try { Module.FS.mkdir(path); } catch (e) { /* EEXIST is fine */ }
    }
    function prepareWinePrefix() {
        // ROOT (/root in MEMFS) is the guest "/". Boxedwine MKDIR()s ROOT itself
        // inside main(), but that runs AFTER this preRun hook, so create the full
        // chain here (ROOT first) — a later MKDIR of an existing dir is a no-op.
        mkdirHost(ROOT);
        mkdirHost(ROOT + "/winePrefix");
        mkdirHost(ROOT + "/winePrefix/.wine");
        try {
            var st = Module.FS.stat(ROOT + "/winePrefix/.wine");
            console.log("created writable WINEPREFIX at " + ROOT + "/winePrefix/.wine (mode " + st.mode.toString(8) + ")");
        } catch (e) {
            console.error("WINEPREFIX dir NOT created: " + e);
        }
    }

    // --- download files the user saved -------------------------------------
    // Notepad (and any guest app) writes through Boxedwine's kernel to the
    // WRITABLE native root, which is this Emscripten MEMFS under ROOT (/root).
    // Two save locations matter, both rooted in MEMFS:
    //
    //   * Z:\home\username  — wine's DEFAULT save dir. z: maps to guest "/",
    //     guest "/" maps to MEMFS ROOT, and HOME is /home/username, so this is
    //     MEMFS ROOT + "/home/username". This is where files land if the user
    //     just hits Save without browsing. NOTE this dir also contains the whole
    //     .wine prefix (registry, glcube.exe, …) — we EXCLUDE .wine so prefix
    //     internals don't leak into the download.
    //   * C:\users\username — the shell "Documents/Desktop" profile. c: maps to
    //     .../drive_c, so this is ROOT + "/home/username/.wine/drive_c/users/
    //     username". Holds many empty Wine shell-folder dirs (AppData, Searches,
    //     Links, …) we DON'T want to walk.
    //
    // To keep the zip clean we grab only the places users actually save:
    //   - regular files sitting directly in Z:\home\username  (minus .wine)
    //   - regular files directly in C:\users\username
    //   - everything under Documents\ and Desktop\ of BOTH homes (recursively)
    // Packed into a STORE-only zip (no deps; Notepad text doesn't need DEFLATE)
    // and handed to the browser. Mirrors the FS.readdir/FS.readFile + Blob +
    // anchor pattern proven in boxedwine-shell.js (the 32-bit web shell).
    var HOME_IN_MEMFS = ROOT + "/home/username";                       // Z:\home\username
    var PROFILE_IN_MEMFS = HOME_IN_MEMFS + "/.wine/drive_c/users/username"; // C:\users\username

    function fsIsDir(path) {
        try { return Module.FS.isDir(Module.FS.stat(path).mode); }
        catch (e) { return false; }
    }

    function readFileSafe(path) {
        try { return Module.FS.readFile(path, { encoding: "binary" }); }
        catch (e) { return null; /* unreadable (e.g. a dangling link) */ }
    }

    // Collect ONLY the regular files directly inside `dir` (no recursion),
    // naming them relative to `base`. `skip` is an optional set of child names
    // to ignore (e.g. ".wine" so we don't recurse the prefix — though we don't
    // recurse here anyway, it documents intent and guards a future change).
    function collectTopLevelFiles(dir, base, out, skip) {
        var entries;
        try { entries = Module.FS.readdir(dir); } catch (e) { return; }
        entries.forEach(function (name) {
            if (name === "." || name === ".." || name === ".keep") return;
            if (skip && skip[name]) return;
            var full = dir + "/" + name;
            if (fsIsDir(full)) return; // skip subfolders at the root level
            var data = readFileSafe(full);
            if (data) out.push({ name: full.slice(base.length + 1), data: data });
        });
    }

    // Recursively collect every regular file under `dir`, naming relative to
    // `base` (so the zip preserves the folder layout, e.g. Documents/foo.txt).
    function collectFiles(dir, base, out) {
        var entries;
        try { entries = Module.FS.readdir(dir); } catch (e) { return; }
        entries.forEach(function (name) {
            if (name === "." || name === ".." || name === ".keep") return;
            var full = dir + "/" + name;
            if (fsIsDir(full)) {
                collectFiles(full, base, out);
            } else {
                var data = readFileSafe(full);
                if (data) out.push({ name: full.slice(base.length + 1), data: data });
            }
        });
    }

    // Minimal STORE-only (compression method 0) ZIP writer. Enough to bundle
    // saved text files into one download without pulling in a zip library.
    function buildStoreZip(files) {
        function crc32(bytes) {
            var c, table = buildStoreZip._t || (buildStoreZip._t = (function () {
                var t = new Uint32Array(256);
                for (var n = 0; n < 256; n++) {
                    c = n;
                    for (var k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
                    t[n] = c >>> 0;
                }
                return t;
            })());
            c = 0xFFFFFFFF;
            for (var i = 0; i < bytes.length; i++) c = table[(c ^ bytes[i]) & 0xFF] ^ (c >>> 8);
            return (c ^ 0xFFFFFFFF) >>> 0;
        }
        var enc = new TextEncoder();
        var locals = [], central = [], offset = 0;
        function u16(v) { return [v & 0xFF, (v >>> 8) & 0xFF]; }
        function u32(v) { return [v & 0xFF, (v >>> 8) & 0xFF, (v >>> 16) & 0xFF, (v >>> 24) & 0xFF]; }
        files.forEach(function (f) {
            var nameBytes = enc.encode(f.name);
            var crc = crc32(f.data), sz = f.data.length;
            var local = [].concat(
                u32(0x04034b50), u16(20), u16(0), u16(0), u16(0), u16(0),
                u32(crc), u32(sz), u32(sz), u16(nameBytes.length), u16(0)
            );
            locals.push(new Uint8Array(local), nameBytes, f.data);
            central.push([].concat(
                u32(0x02014b50), u16(20), u16(20), u16(0), u16(0), u16(0), u16(0),
                u32(crc), u32(sz), u32(sz), u16(nameBytes.length),
                u16(0), u16(0), u16(0), u16(0), u32(0), u32(offset)
            ), nameBytes);
            offset += local.length + nameBytes.length + sz;
        });
        var centralStart = offset, centralSize = 0;
        var centralChunks = [];
        central.forEach(function (c) {
            var u = (c instanceof Uint8Array) ? c : new Uint8Array(c);
            centralChunks.push(u); centralSize += u.length;
        });
        var end = new Uint8Array([].concat(
            u32(0x06054b50), u16(0), u16(0), u16(files.length), u16(files.length),
            u32(centralSize), u32(centralStart), u16(0)
        ));
        return new Blob(locals.concat(centralChunks, [end]), { type: "application/zip" });
    }

    function downloadSavedFiles() {
        if (!window.Module || !Module.FS) {
            alert("Filesystem not ready yet — wait for the app to finish loading.");
            return;
        }
        var files = [];
        // (1) wine's DEFAULT save dir Z:\home\username — top-level files (minus
        // the .wine prefix) plus its Documents\/Desktop\ trees. Name these under
        // a "home/" prefix so they can't collide with the C: profile's files.
        collectTopLevelFiles(HOME_IN_MEMFS, ROOT, files, { ".wine": 1 });
        collectFiles(HOME_IN_MEMFS + "/Documents", ROOT, files);
        collectFiles(HOME_IN_MEMFS + "/Desktop", ROOT, files);
        // (2) The C:\users\username shell profile — top-level files plus its
        // Documents\/Desktop\ trees. Named "users/username/..." (relative to its
        // own grandparent) to keep them distinct from the Z: home above.
        var PROFILE_BASE = HOME_IN_MEMFS + "/.wine/drive_c";
        collectTopLevelFiles(PROFILE_IN_MEMFS, PROFILE_BASE, files);
        collectFiles(PROFILE_IN_MEMFS + "/Documents", PROFILE_BASE, files);
        collectFiles(PROFILE_IN_MEMFS + "/Desktop", PROFILE_BASE, files);
        if (!files.length) {
            alert("No saved files found yet.\n\nSave a file from the app first " +
                  "(e.g. Notepad → File → Save As, into Documents or Desktop), " +
                  "then click Download again.");
            return;
        }
        var blob = buildStoreZip(files);
        var ts = new Date().toISOString().replace(/[:.]/g, "-").slice(0, 19);
        var a = document.createElement("a");
        a.href = URL.createObjectURL(blob);
        a.download = "wine64-files-" + ts + ".zip";
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        setTimeout(function () { URL.revokeObjectURL(a.href); }, 10000);
        console.log("downloadSavedFiles: zipped " + files.length + " file(s): " +
                    files.map(function (f) { return f.name; }).join(", "));
    }
    // Expose so the HTML button's onclick can reach it.
    window.downloadSavedFiles = downloadSavedFiles;

    // --- orchestration ------------------------------------------------------
    // The zips are large (wine64.zip ~205MB) on the FIRST boot; fetch them, drop
    // them in the VFS, push the argv, then release the run dependency so main()
    // proceeds. On an in-page relaunch the bytes come from zipCache (no network).
    // `els` are resolved fresh per boot (the canvas is replaced on relaunch).
    function loadFilesystem(els) {
        console.log("wine64-launcher: loading 64-bit rootfs from " + (BASE || "./") +
                    (relaunchCount ? " (cached, relaunch #" + relaunchCount + ")" : ""));
        function report(name, recv, total) {
            var pct = total ? Math.round((recv / total) * 100) : 0;
            setStatusText("Loading " + name + " " + pct + "%");
            if (els.progress) {
                els.progress.hidden = false;
                els.progress.value = recv;
                els.progress.max = total;
            }
        }
        // report(label, recv, total) matches fetchBytes's onProgress(label, …)
        // signature, so it can be passed straight through.
        fetchZipToVfs(GLIBC_ZIP, report)
            .then(function () {
                return fetchZipToVfs(WINE_ZIP, report);
            })
            .then(function () {
                // Third overlay: the tiny pre-booted prefix (+glcube.exe), only
                // when we'll actually run a program against it.
                if (USE_PREFIX) {
                    return fetchZipToVfs(PREFIX_ZIP, report);
                }
            })
            .then(function () {
                if (els.progress) els.progress.hidden = true;
                if (els.spinner) els.spinner.hidden = true;
                setStatusText(DO_BOOT ? "Booting wine prefix..." :
                              USE_PREFIX ? "Starting " + PROG + " (pre-booted prefix)..." :
                              "Starting wine64...");

                if (DO_BOOT) prepareWinePrefix();

                var args = buildArguments();
                for (var i = 0; i < args.length; i++) window.Module["arguments"].push(args[i]);
                console.log("wine64 argv: " + JSON.stringify(args));

                window.Module["removeRunDependency"]("loadWine64Fs");
            })
            .catch(function (err) {
                console.error("wine64-launcher failed: " + err);
                setStatusText("Failed to load rootfs: " + err);
            });
    }

    // --- in-page relaunch teardown ------------------------------------------
    // A wasm64-mt instance can't simply re-run in the same page: its pthread
    // workers and (crucially) the OffscreenCanvas transferred to a pthread via
    // OFFSCREENCANVASES_TO_PTHREAD live on. transferControlToOffscreen() can only
    // be called ONCE per canvas, so a second instance MUST get brand-new canvas
    // elements. Tear the old instance down: stop its threads, drop the runtime
    // globals, remove the old <script> + canvases, and recreate fresh canvases.
    function teardownInstance() {
        var M = window.Module;
        // 1. Terminate the old instance's pthread workers (best-effort).
        try {
            if (M && M.PThread && typeof M.PThread.terminateAllThreads === "function") {
                M.PThread.terminateAllThreads();
            }
        } catch (e) { console.warn("teardown: terminateAllThreads: " + e); }
        // 2. Remove the old module <script> so the fresh one re-executes cleanly.
        try {
            var old = document.getElementById("boxedwine64-module-script");
            if (old && old.parentNode) old.parentNode.removeChild(old);
        } catch (e) {}
        // 3. Replace the canvases. The transferred #gl64canvas can't be reused;
        //    #canvas likewise carries a dead SDL/WebGL context. Recreate both with
        //    the same ids/attrs so the fresh boot transfers untouched canvases.
        replaceCanvas("gl64canvas", function (c) {
            c.width = 1024; c.height = 1024; c.style.display = "none";
        });
        replaceCanvas("canvas", function (c) {
            c.className = "emscripten";
            c.setAttribute("oncontextmenu", "event.preventDefault()");
            c.width = 800; c.height = 600;
        });
        // 4. Drop the runtime globals the next instance must not inherit.
        try { delete window.Module; } catch (e) { window.Module = undefined; }
        try { delete window.wasmMemory; } catch (e) {}
    }

    function replaceCanvas(id, configure) {
        var old = document.getElementById(id);
        if (!old || !old.parentNode) return;
        var fresh = document.createElement("canvas");
        fresh.id = id;
        if (configure) configure(fresh);
        old.parentNode.replaceChild(fresh, old);
    }

    // --- one boot of the wasm module ----------------------------------------
    // Builds a fresh window.Module against the current run config and appends the
    // emscripten script so it boots. Safe to call repeatedly (teardown first).
    function bootWine() {
        var canvas = document.getElementById("canvas");
        if (canvas) {
            canvas.addEventListener("webglcontextlost", function (e) {
                // On relaunch this is expected (we tear the old context down); only
                // alarm the user on the live instance.
                console.warn("webglcontextlost on #canvas");
                e.preventDefault();
            }, false);
            canvas.width = 800;
            canvas.height = 600;
        }
        var els = {
            progress: document.getElementById("progress"),
            spinner: document.getElementById("spinner"),
            output: document.getElementById("output")
        };

        window.Module = {
            arguments: [],
            canvas: canvas,
            preRun: [function () {
                // Host-side GL trace (read via getenv in gl64bridge.cpp). Emscripten
                // getenv() reads Module.ENV; set it before main() runs.
                try { Module["ENV"] = Module["ENV"] || {}; Module["ENV"]["BW64_GLTRACE"] = "1"; } catch (e) {}
                // Hold main() until the rootfs is in the VFS.
                Module["addRunDependency"]("loadWine64Fs");
                loadFilesystem(els);
            }],
            print: function () {
                var text = Array.prototype.slice.call(arguments).join(" ");
                console.log(text);
                if (els.output) { els.output.value += text + "\n"; els.output.scrollTop = els.output.scrollHeight; }
            },
            printErr: function () {
                var text = Array.prototype.slice.call(arguments).join(" ");
                console.error(text);
                if (els.output) { els.output.value += text + "\n"; els.output.scrollTop = els.output.scrollHeight; }
            },
            setStatus: function (text) { if (text) setStatusText(text); },
            totalDependencies: 0,
            monitorRunDependencies: function () {}
        };

        // Gate the emscripten glue on cross-origin isolation. The wasm64-mt build
        // needs SharedArrayBuffer AND a WebGL2 context (the gl64 bridge), both of
        // which require crossOriginIsolated===true. On `node server.mjs` (real
        // COOP/COEP headers) that's true on first paint. On GitHub Pages,
        // coi-serviceworker.js injects the headers but only after it reloads the
        // page once; until then crossOriginIsolated is false and booting anyway
        // makes emscripten_webgl_create_context fail ("ensureContext FAILED") so
        // the cube never renders. So: only append the script once isolated.
        function appendModuleScript() {
            var s = document.createElement("script");
            s.id = "boxedwine64-module-script";
            s.async = true;
            s.src = "boxedwine64.js";
            document.body.appendChild(s);
        }
        if (window.crossOriginIsolated || typeof window.crossOriginIsolated === "undefined") {
            // Already isolated (real headers / coi reload landed), or a browser with
            // no isolation concept (coi shim gave up) — just boot.
            appendModuleScript();
            return;
        }
        // Not isolated yet: coi-serviceworker registers + reloads to turn it on.
        // Poll briefly for isolation; surface a clear message if it never lands.
        setStatusText("Enabling cross-origin isolation (service worker)…");
        var waited = 0, STEP = 250, LIMIT = 15000;
        var iv = setInterval(function () {
            if (window.crossOriginIsolated) { clearInterval(iv); appendModuleScript(); return; }
            waited += STEP;
            if (waited >= LIMIT) {
                clearInterval(iv);
                setStatusText("Could not enable cross-origin isolation — try a hard reload " +
                              "(⌘/Ctrl+Shift+R). SharedArrayBuffer/WebGL need COOP/COEP.");
                console.error("crossOriginIsolated still false after " + LIMIT +
                              "ms; not booting (WebGL context would fail).");
            }
        }, STEP);
    }

    // --- public: launch another app in-page (no full reload) ----------------
    // Reuses the cached rootfs bytes; tears down the running instance and boots a
    // fresh one for `prog`. opts: { boot, novideo } mirror the query params.
    var relaunchInFlight = false;
    function launchApp(prog, opts) {
        if (relaunchInFlight) { console.warn("launchApp ignored: a relaunch is in flight"); return; }
        if (!Object.keys(zipCache).length) {
            // Nothing cached yet (first boot still downloading) — fall back to a
            // navigation so we don't boot with an empty VFS. Preserve the existing
            // params (esp. ?chunked=1 on GitHub Pages) and just swap p/boot/novideo.
            try {
                var navUrl = new URL(window.location.href);
                navUrl.searchParams.set("p", prog);
                if (opts && opts.boot) navUrl.searchParams.set("boot", "1"); else navUrl.searchParams.delete("boot");
                if (opts && opts.novideo) navUrl.searchParams.set("novideo", "1"); else navUrl.searchParams.delete("novideo");
                window.location.href = navUrl.toString();
            } catch (e) {
                window.location.search = "?p=" + encodeURIComponent(prog);
            }
            return;
        }
        relaunchInFlight = true;
        relaunchCount++;
        applyRunConfig(prog, opts);
        // Reflect the choice in the URL bar without reloading, so a manual refresh
        // or shared link reproduces it.
        try {
            var url = new URL(window.location.href);
            url.searchParams.set("p", prog);
            if (opts && opts.boot) url.searchParams.set("boot", "1"); else url.searchParams.delete("boot");
            if (opts && opts.novideo) url.searchParams.set("novideo", "1"); else url.searchParams.delete("novideo");
            window.history.replaceState(null, "", url.toString());
        } catch (e) {}
        if (window.setActiveApp) try { window.setActiveApp(prog); } catch (e) {}
        setStatusText("Switching to " + prog + "…");
        var sp = document.getElementById("spinner");
        if (sp) sp.hidden = false;
        teardownInstance();
        // Give the terminated workers / replaced canvases a tick to settle before
        // the fresh instance grabs SharedArrayBuffer + transfers the new canvas.
        setTimeout(function () {
            relaunchInFlight = false;
            bootWine();
        }, 150);
    }
    window.launchApp = launchApp;
    // Expose so the HTML button's onclick can reach it.
    window.downloadSavedFiles = downloadSavedFiles;

    // --- first boot from the page load --------------------------------------
    setStatusText("Loading wine64 (WASM)...");
    window.onerror = function (msg, file, line, col, error) {
        setStatusText("Exception — see console");
        console.error(msg, file, line, col, error);
    };
    if (window.setActiveApp) try { window.setActiveApp(PROG || "--version"); } catch (e) {}
    bootWine();
})();
