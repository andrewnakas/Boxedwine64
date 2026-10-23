// click_probe.mjs <url> <waitMs> <buttonText> <afterMs> <shot.png> [regex]
// Boot, wait, click the first button whose text contains buttonText, wait, screenshot.
import { launch, sleep } from "./common.mjs";
const [url, wait, text, after, shot, re = "PAGEERROR"] = process.argv.slice(2);
const rx = new RegExp(re);
const b = await launch();
const pg = await b.newPage();
pg.on("console", (m) => { const t = m.text(); if (rx.test(t)) console.log(t.slice(0, 240)); });
pg.on("pageerror", (e) => console.log("PAGEERROR " + e.message));
await pg.goto(url, { waitUntil: "domcontentloaded" });
await sleep(Number(wait));
const clicked = await pg.evaluate((text) => {
  const btn = [...document.querySelectorAll("button")].find((x) => x.textContent.includes(text));
  if (btn) btn.click(); return !!btn; }, text);
console.log("clicked '" + text + "': " + clicked);
await sleep(Number(after));
await pg.screenshot({ path: shot });
await b.close();
