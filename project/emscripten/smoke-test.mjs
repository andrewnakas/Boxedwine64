// smoke-test.mjs — CI boot smoke for the wasm64-mt browser build, no npm deps.
//
// Launches headless Chrome via the DevTools Protocol (same minimal RFC6455
// client as cdp-test.mjs), loads the wine64 launcher page, and PASSES only if
// the in-browser wine boot reaches a real first paint:
//   1. console prints "XWire: first window mapped"  (X server mapped a window)
//   2. the canvas is actually lit (>= --min-lit % of sampled pixels non-black)
// Hard-fatal console markers fail the attempt immediately.
//
// The wine boot has a known intermittent wineserver startup race (several
// wineservers contend, the real one can lose and exit -> the app never paints),
// so the test runs up to --attempts fresh-Chrome attempts before failing.
//
// Usage:
//   node smoke-test.mjs <url> [--timeout=300000] [--attempts=2] [--min-lit=10]
// Chrome binary: $CHROME_BIN, else the first existing well-known path.
//
// macOS gotcha (local runs only — CI is Linux): a SECOND simultaneously
// running instance of the same Chrome binary can't spawn renderers (Mach-port
// rendezvous collision, children die with "No rendezvous client, terminating
// process") — the symptom is an attempt with ZERO console lines or a DevTools
// ECONNRESET. Quit other instances of that Chrome binary before running.
import { spawn } from "node:child_process";
import { connect } from "node:net";
import { request } from "node:http";
import { existsSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const URL_ = process.argv[2];
if (!URL_) { console.error("usage: node smoke-test.mjs <url> [--timeout=ms] [--attempts=n] [--min-lit=pct]"); process.exit(64); }
const arg = (name, dflt) => {
    const a = process.argv.find(s => s.startsWith(`--${name}=`));
    return a ? Number(a.split("=")[1]) : dflt;
};
const TIMEOUT = arg("timeout", 300000);
const ATTEMPTS = arg("attempts", 2);
const MIN_LIT = arg("min-lit", 10);

const BOOT_MARKER = "XWire: first window mapped";
const FATAL_MARKERS = ["WebAssembly.Exception", "worker sent an error", "RuntimeError", "Aborted(", "Boot failed:"];

const CHROME = process.env.CHROME_BIN || [
    "/usr/bin/google-chrome",
    "/usr/bin/google-chrome-stable",
    "/usr/bin/chromium-browser",
    "/opt/google/chrome/chrome",
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
].find(existsSync);
if (!CHROME) { console.error("no Chrome found — set CHROME_BIN"); process.exit(65); }

const PORT = 9377;

function httpJson(path) {
    return new Promise((res, rej) => {
        const req = request({ host: "127.0.0.1", port: PORT, path, method: "GET" }, (r) => {
            let d = ""; r.on("data", c => d += c); r.on("end", () => { try { res(JSON.parse(d)); } catch (e) { rej(e); } });
        });
        req.on("error", rej); req.end();
    });
}

async function getWsUrl() {
    for (let i = 0; i < 75; i++) {
        try {
            const targets = await httpJson("/json");
            const page = targets.find(t => t.type === "page" && t.webSocketDebuggerUrl);
            if (page) return page.webSocketDebuggerUrl;
        } catch {}
        await new Promise(r => setTimeout(r, 200));
    }
    throw new Error("Chrome DevTools endpoint never came up");
}

// Minimal RFC6455 text-frame client (mirrors cdp-test.mjs).
function wsConnect(wsUrl, onMessage, onError) {
    const u = new URL(wsUrl);
    const sock = connect(Number(u.port), u.hostname);
    sock.on("connect", () => {
        sock.write(
            `GET ${u.pathname}${u.search} HTTP/1.1\r\n` +
            `Host: ${u.host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n` +
            `Sec-WebSocket-Key: c21va2V0ZXN0c21va2V0ZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n`
        );
    });
    let buf = Buffer.alloc(0), handshook = false;
    const pendingSends = [];
    sock.on("data", (chunk) => {
        buf = Buffer.concat([buf, chunk]);
        if (!handshook) {
            const idx = buf.indexOf("\r\n\r\n");
            if (idx === -1) return;
            handshook = true; buf = buf.slice(idx + 4);
            // Flush frames queued before the 101 completed. Writing frames
            // before the server's handshake response lands them inside the
            // HTTP request stream — newer Chrome's parser hard-resets the
            // connection on that (the old cdp-test.mjs got away with it).
            for (const data of pendingSends.splice(0)) writeFrame(data);
        }
        while (buf.length >= 2) {
            const opcode = buf[0] & 0x0f;
            let len = buf[1] & 0x7f, off = 2;
            if (len === 126) { len = buf.readUInt16BE(2); off = 4; }
            else if (len === 127) { len = Number(buf.readBigUInt64BE(2)); off = 10; }
            if (buf.length < off + len) break;
            const payload = buf.slice(off, off + len);
            buf = buf.slice(off + len);
            if (opcode === 0x1) onMessage(payload.toString("utf8"));
            else if (opcode === 0x8) { sock.end(); return; }
        }
    });
    sock.on("error", (e) => onError(e));
    function writeFrame(data) {
        const mask = Buffer.from([0, 0, 0, 0]);
        let header;
        if (data.length < 126) header = Buffer.from([0x81, 0x80 | data.length]);
        else if (data.length < 65536) {
            header = Buffer.alloc(4); header[0] = 0x81; header[1] = 0x80 | 126;
            header.writeUInt16BE(data.length, 2);
        } else {
            header = Buffer.alloc(10); header[0] = 0x81; header[1] = 0x80 | 127;
            header.writeBigUInt64BE(BigInt(data.length), 2);
        }
        sock.write(Buffer.concat([header, mask, data]));
    }
    function send(obj) {
        const data = Buffer.from(JSON.stringify(obj));
        if (!handshook) { pendingSends.push(data); return; }
        writeFrame(data);
    }
    return { send, close: () => sock.end() };
}

// Sample the emulator canvas: % of pixels brighter than near-black. drawImage
// into a scratch 2d canvas works for both the SDL 2d canvas and WebGL canvases.
const SAMPLE_EXPR = `(() => {
  const c = document.querySelector('canvas');
  if (!c || !c.width || !c.height) return 'lit=-1';
  try {
    const t = document.createElement('canvas'); t.width = 160; t.height = 120;
    const x = t.getContext('2d');
    x.drawImage(c, 0, 0, 160, 120);
    const d = x.getImageData(0, 0, 160, 120).data;
    let lit = 0;
    for (let i = 0; i < d.length; i += 4) if (d[i] + d[i+1] + d[i+2] > 90) lit++;
    return 'lit=' + (lit * 400 / d.length).toFixed(1);
  } catch (e) { return 'lit-err:' + e.message; }
})()`;

function runAttempt(n) {
    return new Promise((resolveAttempt) => {
        const profile = mkdtempSync(join(tmpdir(), "wine64smoke-"));
        const chrome = spawn(CHROME, [
            "--headless=new",
            `--remote-debugging-port=${PORT}`,
            `--user-data-dir=${profile}`,
            "--no-first-run", "--no-default-browser-check",
            "--no-sandbox",
            "--disable-dev-shm-usage",
            "--enable-features=SharedArrayBuffer",
            "--enable-unsafe-swiftshader",
            "--window-size=1280,1024",
            "about:blank",
        ], { stdio: ["ignore", "ignore", "inherit"] });

        const recent = [];           // rolling console tail for failure triage
        let ws = null;
        let done = false;
        let pollTimer = null;
        let booted = false;

        function finish(ok, why) {
            if (done) return;
            done = true;
            if (pollTimer) clearInterval(pollTimer);
            console.log(`[attempt ${n}] ${ok ? "PASS" : "FAIL"} — ${why}`);
            if (!ok) {
                console.log(`[attempt ${n}] last console lines:`);
                for (const l of recent.slice(-40)) console.log("   " + l.slice(0, 200));
            }
            try { ws && ws.close(); } catch {}
            try { chrome.kill("SIGKILL"); } catch {}
            setTimeout(() => { try { rmSync(profile, { recursive: true, force: true }); } catch {}; resolveAttempt(ok); }, 500);
        }

        chrome.on("exit", () => { if (!done) finish(false, "chrome exited early"); });

        (async () => {
            let wsUrl;
            try { wsUrl = await getWsUrl(); } catch (e) { return finish(false, e.message); }
            let id = 0;
            const litWaiters = new Map();   // evaluate id -> resolve
            ws = wsConnect(wsUrl, (msg) => {
                let m; try { m = JSON.parse(msg); } catch { return; }
                if (m.method === "Runtime.consoleAPICalled") {
                    const text = (m.params.args || []).map(a =>
                        a.value !== undefined ? String(a.value) : (a.description || a.type)).join(" ");
                    recent.push(text);
                    if (recent.length > 400) recent.splice(0, 100);
                    if (text.includes(BOOT_MARKER)) booted = true;
                    const fatal = FATAL_MARKERS.find(f => text.includes(f));
                    if (fatal) finish(false, `fatal console marker: ${text.slice(0, 160)}`);
                } else if (m.method === "Runtime.exceptionThrown") {
                    const d = m.params.exceptionDetails;
                    const text = (d.exception && (d.exception.description || String(d.exception.value))) || d.text || "exception";
                    recent.push("EXC " + text);
                    // Uncaught page exceptions during boot are diagnostic, not
                    // automatically fatal (workers log recoverable noise);
                    // FATAL_MARKERS above decides what kills the attempt.
                } else if (m.id !== undefined && litWaiters.has(m.id)) {
                    const resolve = litWaiters.get(m.id);
                    litWaiters.delete(m.id);
                    const v = m.result && m.result.result ? m.result.result.value : "";
                    resolve(typeof v === "string" ? v : "");
                }
            }, (e) => finish(false, "ws error: " + e.message));

            ws.send({ id: ++id, method: "Runtime.enable" });
            ws.send({ id: ++id, method: "Page.enable" });
            ws.send({ id: ++id, method: "Page.navigate", params: { url: URL_ } });
            console.log(`[attempt ${n}] loading ${URL_}`);

            function sampleLit() {
                return new Promise((res) => {
                    const evalId = ++id;
                    litWaiters.set(evalId, res);
                    ws.send({ id: evalId, method: "Runtime.evaluate", params: { expression: SAMPLE_EXPR, returnByValue: true } });
                    setTimeout(() => { if (litWaiters.has(evalId)) { litWaiters.delete(evalId); res(""); } }, 5000);
                });
            }

            const start = Date.now();
            let litStreak = null;
            pollTimer = setInterval(async () => {
                if (done) return;
                if (Date.now() - start > TIMEOUT) {
                    return finish(false, booted
                        ? "window mapped but canvas never lit"
                        : `"${BOOT_MARKER}" not seen in ${TIMEOUT}ms`);
                }
                if (!booted) return;
                const s = await sampleLit();
                const m = s.match(/^lit=([\d.]+)/);
                if (m && parseFloat(m[1]) >= MIN_LIT) {
                    litStreak = (litStreak || 0) + 1;
                    if (litStreak >= 2) {   // two consecutive lit samples = stable paint
                        finish(true, `booted + canvas lit (${s}) in ${((Date.now() - start) / 1000).toFixed(0)}s`);
                    }
                } else {
                    litStreak = 0;
                }
            }, 3000);
        })();
    });
}

(async () => {
    for (let n = 1; n <= ATTEMPTS; n++) {
        if (await runAttempt(n)) {
            console.log("SMOKE: PASS");
            process.exit(0);
        }
        console.log(`[attempt ${n}] failed${n < ATTEMPTS ? " — retrying with a fresh Chrome" : ""}`);
    }
    console.log("SMOKE: FAIL");
    process.exit(1);
})();
