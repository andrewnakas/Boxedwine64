// frameclick_probe.mjs <url> <waitMs> <frameW> <frameH> <fx> <fy> <shot.png>
// Click at guest-frame coordinates (fx,fy) of a frameW x frameH presented frame
// that SDL letterboxes into the canvas (aspect-preserving, top-left aligned), then screenshot.
import { launch, sleep } from "./common.mjs";
const [url, wait, fw, fh, fx, fy, shot] = process.argv.slice(2).map((v, i) => (i >= 2 && i <= 5 ? Number(v) : v));
const b = await launch(); const pg = await b.newPage();
await pg.goto(url, { waitUntil: "domcontentloaded" }); await sleep(Number(wait));
console.log("before click: fullscreenElement=" + await pg.evaluate(() => document.fullscreenElement ? document.fullscreenElement.id : "none"));
const r = await pg.evaluate(() => { const c = document.getElementById("canvas").getBoundingClientRect(); return { x: c.x, y: c.y, w: c.width, h: c.height }; });
// SDL (Emscripten) draws the letterboxed frame top-left aligned in the canvas
const s = Math.min(r.w / fw, r.h / fh), ox = 0, oy = 0;
const px = r.x + ox + fx * s, py = r.y + oy + fy * s;
console.log(`canvas ${JSON.stringify(r)} scale ${s.toFixed(3)} -> click page (${px.toFixed(0)},${py.toFixed(0)})`);
await pg.mouse.move(px, py); await sleep(300); await pg.mouse.down(); await sleep(120); await pg.mouse.up();
await sleep(6000);
console.log("after click: fullscreenElement=" + await pg.evaluate(() => document.fullscreenElement ? document.fullscreenElement.id : "none") +
            " pointerLock=" + await pg.evaluate(() => document.pointerLockElement ? document.pointerLockElement.id : "none"));
await pg.screenshot({ path: shot }); await b.close();
