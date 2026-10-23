// eval_probe.mjs <url> <waitMs> <js-expression> — load, wait, print JSON of the expression
import { launch, sleep } from "./common.mjs";
const [url, wait, expr] = process.argv.slice(2);
const b = await launch(); const pg = await b.newPage();
await pg.goto(url, { waitUntil: "domcontentloaded" }); await sleep(Number(wait));
console.log(JSON.stringify(await pg.evaluate(expr), null, 1)); await b.close();
