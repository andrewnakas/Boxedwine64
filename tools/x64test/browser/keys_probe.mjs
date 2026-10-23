// keys_probe.mjs <url> <waitMs> <keys...> <shot.png> — focus the canvas, press keys
// ("Alt+f" chords or single keys), screenshot. Last arg is the screenshot path.
import { launch, sleep } from "./common.mjs";
const args = process.argv.slice(2); const [url, wait] = args; const shot = args[args.length - 1]; const keys = args.slice(2, -1);
const b = await launch(); const pg = await b.newPage();
await pg.goto(url, { waitUntil: "domcontentloaded" }); await sleep(Number(wait));
await pg.focus("#canvas").catch(() => {});
for (const k of keys) {
  const parts = k.split("+");
  for (const p of parts.slice(0, -1)) await pg.keyboard.down(p);
  await pg.keyboard.press(parts[parts.length - 1]); await sleep(150);
  for (const p of parts.slice(0, -1).reverse()) await pg.keyboard.up(p);
  await sleep(1500);
}
await sleep(4000); await pg.screenshot({ path: shot }); await b.close();
