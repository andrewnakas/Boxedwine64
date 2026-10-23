// console_probe.mjs <url> <waitMs> [regex] [screenshot.png]
// Load a page, print console lines matching regex (+ page errors), optional screenshot.
import { launch, sleep } from "./common.mjs";
const [url, wait = "25000", re = ".", shot] = process.argv.slice(2);
const rx = new RegExp(re);
const b = await launch();
const pg = await b.newPage();
pg.on("console", (m) => { const t = m.text(); if (rx.test(t)) console.log(t.slice(0, Number(process.env.PROBE_MAXLEN || 300))); });
pg.on("pageerror", (e) => console.log("PAGEERROR " + e.message + " " + (e.stack || "").slice(0, 400)));
pg.on("dialog", async (d) => { console.log("DIALOG " + d.message()); await d.dismiss(); });
await pg.goto(url, { waitUntil: "domcontentloaded" });
await sleep(Number(wait));
if (shot) await pg.screenshot({ path: shot });
await b.close();
