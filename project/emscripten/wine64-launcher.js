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
    var DO_BOOT = param("boot") === "1";
    var NOVIDEO = param("novideo") === "1";
    var CHUNKED = param("chunked") === "1"; // fetch zips as <zip>.partNNN via manifest
    var PROG = param("p"); // may be null
    // Run a real program against the pre-booted prefix when ?p= names something
    // other than the bare --version correctness boot (and we're not doing ?boot=1).
    var USE_PREFIX = !DO_BOOT && PROG && PROG.length && PROG !== "--version";

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
    // The zips are large (wine64.zip ~205MB); fetch them, drop them in the VFS,
    // push the argv, then release the run dependency so main() proceeds.
    function loadFilesystem() {
        console.log("wine64-launcher: loading 64-bit rootfs from " + (BASE || "./"));
        var glibcDone = false, wineDone = false;
        function report(name, recv, total) {
            var pct = total ? Math.round((recv / total) * 100) : 0;
            setStatusText("Downloading " + name + " " + pct + "%");
            if (progressElement) {
                progressElement.hidden = false;
                progressElement.value = recv;
                progressElement.max = total;
            }
        }
        // report(label, recv, total) matches fetchBytes's onProgress(label, …)
        // signature, so it can be passed straight through.
        fetchZipToVfs(GLIBC_ZIP, report)
            .then(function () {
                glibcDone = true;
                return fetchZipToVfs(WINE_ZIP, report);
            })
            .then(function () {
                wineDone = true;
                // Third overlay: the tiny pre-booted prefix (+glcube.exe), only
                // when we'll actually run a program against it.
                if (USE_PREFIX) {
                    return fetchZipToVfs(PREFIX_ZIP, report);
                }
            })
            .then(function () {
                if (progressElement) progressElement.hidden = true;
                if (spinnerElement) spinnerElement.hidden = true;
                setStatusText(DO_BOOT ? "Booting wine prefix..." :
                              USE_PREFIX ? "Starting " + PROG + " (pre-booted prefix)..." :
                              "Starting wine64...");

                if (DO_BOOT) prepareWinePrefix();

                var args = buildArguments();
                for (var i = 0; i < args.length; i++) Module["arguments"].push(args[i]);
                console.log("wine64 argv: " + JSON.stringify(args));

                Module["removeRunDependency"]("loadWine64Fs");
            })
            .catch(function (err) {
                console.error("wine64-launcher failed: " + err);
                setStatusText("Failed to load rootfs: " + err);
            });
    }

    // --- Emscripten Module --------------------------------------------------
    var canvas = document.getElementById("canvas");
    if (canvas) {
        canvas.addEventListener("webglcontextlost", function (e) {
            alert("WebGL context lost. Reload the page.");
            e.preventDefault();
        }, false);
        canvas.width = 800;
        canvas.height = 600;
    }

    var outputEl = document.getElementById("output");

    window.Module = {
        arguments: [],
        canvas: canvas,
        preRun: [function () {
            // TEMP DEBUG: host-side GL trace (read via getenv in gl64bridge.cpp).
            // Emscripten getenv() reads Module.ENV; set it before main() runs.
            try { Module["ENV"] = Module["ENV"] || {}; Module["ENV"]["BW64_GLTRACE"] = "1"; } catch (e) {}
            // Hold main() until the rootfs is in the VFS.
            Module["addRunDependency"]("loadWine64Fs");
            loadFilesystem();
        }],
        print: function () {
            var text = Array.prototype.slice.call(arguments).join(" ");
            console.log(text);
            if (outputEl) {
                outputEl.value += text + "\n";
                outputEl.scrollTop = outputEl.scrollHeight;
            }
        },
        printErr: function () {
            var text = Array.prototype.slice.call(arguments).join(" ");
            console.error(text);
            if (outputEl) {
                outputEl.value += text + "\n";
                outputEl.scrollTop = outputEl.scrollHeight;
            }
        },
        setStatus: function (text) {
            if (text) setStatusText(text);
        },
        totalDependencies: 0,
        monitorRunDependencies: function () {}
    };

    setStatusText("Loading wine64 (WASM)...");
    window.onerror = function (msg, file, line, col, error) {
        setStatusText("Exception — see console");
        console.error(msg, file, line, col, error);
    };
})();
