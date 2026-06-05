// cdp-attach.mjs — attach to an ALREADY-RUNNING cross-origin-isolated Chrome
// (DevTools on 127.0.0.1:9222), open a fresh tab on the wine64 page, stream
// console + the #output textarea, and report whether the match string appears.
//
// Usage: node cdp-attach.mjs <url> [timeoutMs] [matchString] [debugPort]
import { createHash } from "node:crypto";
import { connect } from "node:net";
import { request } from "node:http";

const URL_ = process.argv[2] || "http://127.0.0.1:8000/Build/Wasm64Mt/wine64.html?novideo=1";
const TIMEOUT = Number(process.argv[3] || 240000);
const MATCH = process.argv[4] || "wine-8.0";
const DPORT = Number(process.argv[5] || 9222);

function httpJson(path, method = "GET") {
    return new Promise((res, rej) => {
        const req = request({ host: "127.0.0.1", port: DPORT, path, method }, (r) => {
            let d = ""; r.on("data", c => d += c); r.on("end", () => { try { res(JSON.parse(d)); } catch (e) { res(d); } });
        });
        req.on("error", rej); req.end();
    });
}

function wsConnect(wsUrl, onMessage, onOpen) {
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
            handshook = true; buf = buf.slice(idx + 4); if (onOpen) onOpen();
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
    sock.on("error", (e) => { console.error("ws error", e.message); process.exit(1); });
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
    return { send, close: () => sock.end() };
}

(async () => {
    // Open a brand-new tab on the target URL for a clean boot.
    const created = await httpJson("/json/new?" + encodeURIComponent(URL_), "PUT");
    const wsUrl = created.webSocketDebuggerUrl;
    if (!wsUrl) { console.error("could not open tab:", created); process.exit(1); }
    const targetId = created.id;
    console.log("opened tab", targetId, "->", URL_);

    let id = 0, found = false, lastOutputLen = 0;
    const lines = [];
    function record(tag, text) {
        const line = `[${tag}] ${text}`;
        lines.push(line); console.log(line);
        if (text.includes(MATCH)) found = true;
    }

    const ws = wsConnect(wsUrl, (msg) => {
        let m; try { m = JSON.parse(msg); } catch { return; }
        if (m.method === "Runtime.consoleAPICalled") {
            const text = (m.params.args || []).map(a =>
                a.value !== undefined ? a.value : (a.description || a.type)).join(" ");
            record(m.params.type, text);
        } else if (m.method === "Runtime.exceptionThrown") {
            const d = m.params.exceptionDetails;
            record("EXC", (d.exception && (d.exception.description || d.exception.value)) || d.text);
        } else if (m.id !== undefined && m.result && m.result.result &&
                   typeof m.result.result.value === "string") {
            const v = m.result.result.value;
            if (v.length > lastOutputLen) {
                const fresh = v.slice(lastOutputLen);
                lastOutputLen = v.length;
                fresh.split("\n").forEach(l => { if (l.trim()) record("output", l); });
            }
        }
    }, () => {
        ws.send({ id: ++id, method: "Runtime.enable" });
        ws.send({ id: ++id, method: "Page.enable" });
    });

    const start = Date.now();
    const poll = setInterval(() => {
        ws.send({ id: ++id, method: "Runtime.evaluate",
            params: { expression: "(document.getElementById('output')||{}).value || ''", returnByValue: true } });
        if (found) {
            console.log(`\n=== MATCH FOUND: "${MATCH}" ===`);
            clearInterval(poll);
            httpJson("/json/close/" + targetId).finally(() => process.exit(0));
        } else if (Date.now() - start > TIMEOUT) {
            console.log(`\n=== TIMEOUT after ${TIMEOUT}ms; "${MATCH}" not seen (${lines.length} lines) ===`);
            clearInterval(poll);
            httpJson("/json/close/" + targetId).finally(() => process.exit(2));
        }
    }, 2000);
})();
