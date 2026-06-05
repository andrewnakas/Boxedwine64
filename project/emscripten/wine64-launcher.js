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
//   ?boot=1                 boot the prefix: runs `wineboot --init` with HOME/
//                           WINEPREFIX/WINESERVER env instead of a bare program.
//                           NOTE: in-tab this currently runs the full execve
//                           chain (wine64-preloader -> wine64) and then stops at
//                           `chdir /winePrefix/.wine: No such file or directory`
//                           because the read-only zip overlay has no writable
//                           WINEPREFIX and there is no in-browser wineserver IPC
//                           yet (roadmap steps 3/4). `--version` is the verified
//                           correctness boot; `?boot=1` is a progress probe.
//   ?base=<url>             base URL the zips are fetched from (default: "./").
//   ?novideo=1              pass -novideo (headless; no SDL window).
//
// Examples:
//   wine64.html                      -> wine64 --version  (fast correctness boot)
//   wine64.html?boot=1               -> wineboot --init   (full prefix bring-up)
//   wine64.html?p=notepad.exe        -> wine64 notepad.exe (GUI, needs GL/X11)

(function () {
    "use strict";

    var GLIBC_ZIP = "glibc-rootfs64.zip";
    var WINE_ZIP = "wine64.zip";
    var ROOT = "/root";
    var WINE64 = "/usr/lib/wine/wine64";

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
    var PROG = param("p"); // may be null

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
        if (NOVIDEO) args.push("-novideo");

        if (DO_BOOT) {
            // Full prefix bring-up + wineserver handshake.
            args.push("-env", "HOME=/winePrefix");
            args.push("-env", "WINEPREFIX=/winePrefix/.wine");
            args.push("-env", "WINESERVER=/usr/lib/wine/wineserver64");
            args.push("-env", "WINEDLLPATH=/usr/lib/x86_64-linux-gnu/wine");
            args.push(WINE64, "wineboot", "--init");
        } else {
            // Bare wine64 <program...>; default to --version.
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

    // --- fetch a zip into the Emscripten VFS with progress -------------------
    function fetchZipToVfs(name, onProgress) {
        return fetch(BASE + name).then(function (resp) {
            if (!resp.ok) throw new Error("fetch " + name + " -> HTTP " + resp.status);
            var total = Number(resp.headers.get("Content-Length")) || 0;
            if (!resp.body || !total) {
                return resp.arrayBuffer().then(function (buf) {
                    return new Uint8Array(buf);
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
                    if (onProgress) onProgress(received, total);
                    return pump();
                });
            })();
        }).then(function (bytes) {
            try {
                Module.FS.createDataFile("/", name, bytes, true, true);
            } catch (e) {
                console.log("createDataFile " + name + " failed: " + e);
                throw e;
            }
            console.log("loaded " + name + " (" + bytes.length + " bytes) into VFS");
            return bytes.length;
        });
    }

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
        fetchZipToVfs(GLIBC_ZIP, function (r, t) { report(GLIBC_ZIP, r, t); })
            .then(function () {
                glibcDone = true;
                return fetchZipToVfs(WINE_ZIP, function (r, t) { report(WINE_ZIP, r, t); });
            })
            .then(function () {
                wineDone = true;
                if (progressElement) progressElement.hidden = true;
                if (spinnerElement) spinnerElement.hidden = true;
                setStatusText(DO_BOOT ? "Booting wine prefix..." : "Starting wine64...");

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
