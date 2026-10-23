// fs_probe.mjs <url> <waitMs> <regex> <path|cat:path>...
// Run, then list guest MEMFS dirs (Module.FS; zip-backed files are NOT visible)
// or print files ("cat:" prefix; *.log decoded as UTF-16LE for msiexec logs).
import { launch, sleep } from "./common.mjs";
const [url, wait, re, ...paths] = process.argv.slice(2);
const rx = new RegExp(re);
const b = await launch();
const pg = await b.newPage();
pg.on("console", (m) => { const t = m.text(); if (rx.test(t)) console.log(t.slice(0, 260)); });
pg.on("pageerror", (e) => console.log("PAGEERROR " + e.message));
await pg.goto(url, { waitUntil: "domcontentloaded" });
await sleep(Number(wait));
console.log(await pg.evaluate((paths) => {
  const FS = window.Module && window.Module.FS; if (!FS) return "no FS";
  const out = [];
  const walk = (p, d) => { let names; try { names = FS.readdir(p); } catch (e) { out.push(p + " (missing)"); return; }
    for (const n of names) { if (n === "." || n === "..") continue; const q = p + "/" + n; let st; try { st = FS.stat(q); } catch (e) { continue; }
      out.push(q + (FS.isDir(st.mode) ? "/" : " " + st.size)); if (FS.isDir(st.mode) && d < 8) walk(q, d + 1); } };
  for (const p of paths) {
    if (p.startsWith("cat:")) { try { const bytes = FS.readFile(p.slice(4));
      out.push(new TextDecoder(p.endsWith(".log") && bytes[1] === 0 ? "utf-16le" : "utf-8").decode(bytes)); }
      catch (e) { out.push(p + " unreadable: " + e); } }
    else walk(p, 0);
  }
  return out.join("\n"); }, paths));
await b.close();
