// gamepad_probe.mjs <url> <waitMs> [regex] [screenshot.png]
// Like console_probe.mjs, but the page sees a fake standard gamepad (4 axes,
// 17 buttons): axis 0 sweeps -1..1 every 4s, axis 1 = -axis 0, button 0 is
// pressed during the first half of each sweep. Lets headless runs check the
// browser-gamepad -> wine joystick path (M30).
import { launch, sleep } from "./common.mjs";
const [url, wait = "60000", re = ".", shot] = process.argv.slice(2);
const rx = new RegExp(re);
const b = await launch();
const pg = await b.newPage();
await pg.evaluateOnNewDocument(() => {
    const t0 = performance.now();
    const pad = () => {
        const ph = ((performance.now() - t0) % 4000) / 4000;
        const x = ph * 2 - 1;
        return {
            id: "Fake Gamepad (Boxedwine probe)", index: 0, connected: true, mapping: "standard",
            timestamp: performance.now(),
            axes: [x, -x, 0, 0],
            buttons: Array.from({ length: 17 }, (_, i) => ({ pressed: i === 0 && ph < 0.5, touched: false, value: i === 0 && ph < 0.5 ? 1 : 0 })),
        };
    };
    navigator.getGamepads = () => [pad(), null, null, null];
});
pg.on("console", (m) => { const t = m.text(); if (rx.test(t)) console.log(t.slice(0, Number(process.env.PROBE_MAXLEN || 300))); });
pg.on("pageerror", (e) => console.log("PAGEERROR " + e.message + " " + (e.stack || "").slice(0, 400)));
await pg.goto(url, { waitUntil: "domcontentloaded" });
await sleep(Number(wait));
if (shot) await pg.screenshot({ path: shot });
await b.close();
