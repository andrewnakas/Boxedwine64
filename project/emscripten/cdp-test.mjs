// cdp-test.mjs — drive headless Chrome via the DevTools Protocol with no deps.
// Launches Chrome cross-origin-isolated (so SharedArrayBuffer works), opens the
// wine64 launcher page, streams console output, and reports whether wine-8.0
// (or another target string) appears.
//
// Usage: node cdp-test.mjs <url> [timeoutMs] [matchString]
import { spawn } from "node:child_process";
import { createHash } from "node:crypto";
import { connect } from "node:net";
import { request } from "node:http";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const URL_ = process.argv[2] || "http://127.0.0.1:8000/Build/Wasm64Mt/wine64.html?novideo=1";
const TIMEOUT = Number(process.argv[3] || 240000);
const MATCH = process.argv[4] || "wine-8.0";
const CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
const PORT = 9333;

// Keep the throwaway profile in /tmp (small) — we never download the zip to
// disk; the page holds it in WASM memory, so the profile stays tiny.
const profile = mkdtempSync(join(tmpdir(), "wine64cdp-"));

const chrome = spawn(CHROME, [
    "--headless=new",
    `--remote-debugging-port=${PORT}`,
    `--user-data-dir=${profile}`,
    "--no-first-run", "--no-default-browser-check",
    "--enable-features=SharedArrayBuffer",
    "--disable-dev-shm-usage",
    "--disable-gpu",
    "about:blank",
], { stdio: ["ignore", "ignore", "inherit"] });

function cleanup(code) {
    try { chrome.kill("SIGKILL"); } catch {}
    try { rmSync(profile, { recursive: true, force: true }); } catch {}
    process.exit(code);
}

function httpJson(path) {
    return new Promise((res, rej) => {
        const req = request({ host: "127.0.0.1", port: PORT, path, method: "GET" }, (r) => {
            let d = ""; r.on("data", c => d += c); r.on("end", () => res(JSON.parse(d)));
        });
        req.on("error", rej); req.end();
    });
}

async function getWsUrl() {
    for (let i = 0; i < 50; i++) {
        try {
            const targets = await httpJson("/json");
            const page = targets.find(t => t.type === "page" && t.webSocketDebuggerUrl);
            if (page) return page.webSocketDebuggerUrl;
        } catch {}
        await new Promise(r => setTimeout(r, 200));
    }
    throw new Error("Chrome DevTools endpoint never came up");
}

// --- minimal RFC6455 client (text frames only, no fragmentation) ------------
function wsConnect(wsUrl, onMessage) {
    const u = new URL(wsUrl);
    const sock = connect(Number(u.port), u.hostname);
    const key = createHash("md5").update(Math.random().toString()).digest("base64");
    sock.on("connect", () => {
        sock.write(
            `GET ${u.pathname}${u.search} HTTP/1.1\r\n` +
            `Host: ${u.host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n` +
            `Sec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`
        );
    });
    let buf = Buffer.alloc(0), handshook = false;
    sock.on("data", (chunk) => {
        buf = Buffer.concat([buf, chunk]);
        if (!handshook) {
            const idx = buf.indexOf("\r\n\r\n");
            if (idx === -1) return;
            handshook = true; buf = buf.slice(idx + 4);
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
    sock.on("error", (e) => { console.error("ws error", e.message); cleanup(1); });
    function send(obj) {
        const data = Buffer.from(JSON.stringify(obj));
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
    return { send };
}

(async () => {
    const wsUrl = await getWsUrl();
    let id = 0; let found = false;
    const lines = [];
    function record(tag, text) {
        const line = `[${tag}] ${text}`;
        lines.push(line);
        console.log(line);
        if (text.includes(MATCH)) { found = true; }
    }
    const ws = wsConnect(wsUrl, (msg) => {
        let m; try { m = JSON.parse(msg); } catch { return; }
        if (m.method === "Runtime.consoleAPICalled") {
            const text = (m.params.args || []).map(a =>
                a.value !== undefined ? a.value : (a.description || a.type)).join(" ");
            record(m.params.type, text);
        } else if (m.method === "Log.entryAdded") {
            record("log:" + m.params.entry.level, m.params.entry.text);
        } else if (m.method === "Runtime.exceptionThrown") {
            const d = m.params.exceptionDetails;
            record("EXC", (d.exception && (d.exception.description || d.exception.value)) || d.text);
        } else if (m.id !== undefined && m.result && m.result.result &&
                   typeof m.result.result.value === "string") {
            // response to our textarea#output Runtime.evaluate poll
            const v = m.result.result.value;
            if (v.includes(MATCH)) {
                if (!found) record("output", v.split("\n").filter(l => l.includes(MATCH)).join(" | "));
                found = true;
            }
        }
    });
    ws.send({ id: ++id, method: "Runtime.enable" });
    ws.send({ id: ++id, method: "Log.enable" });
    ws.send({ id: ++id, method: "Page.enable" });
    ws.send({ id: ++id, method: "Page.navigate", params: { url: URL_ } });

    const start = Date.now();
    const poll = setInterval(() => {
        // pull the #output textarea so we also see emulator stdout
        ws.send({
            id: ++id, method: "Runtime.evaluate",
            params: { expression:
                "(document.getElementById('output')||{}).value || '' ", returnByValue: true }
        });
        if (found) {
            console.log(`\n=== MATCH FOUND: "${MATCH}" ===`);
            clearInterval(poll);
            setTimeout(() => cleanup(0), 500);
        } else if (Date.now() - start > TIMEOUT) {
            console.log(`\n=== TIMEOUT after ${TIMEOUT}ms; "${MATCH}" not seen ===`);
            console.log("Captured " + lines.length + " lines.");
            clearInterval(poll);
            setTimeout(() => cleanup(2), 300);
        }
    }, 2000);
})();
