// boot_loop.mjs <url> <runs> <perRunWallMs> <outDir>
// N sequential cold boots in fresh headless Chromes; PASS = "XWire: first window
// mapped" within the wall. Full console per run in outDir/bootlog_NNN.log.
import { launch, sleep } from "./common.mjs";
import { writeFileSync, appendFileSync, mkdirSync } from "node:fs";
import { join } from "node:path";
const [URL_, RUNS_ = "3", WALL_ = "150000", OUT = "/tmp/bootloop"] = process.argv.slice(2);
mkdirSync(OUT, { recursive: true });
let pass = 0;
for (let run = 1; run <= Number(RUNS_); run++) {
  const log = join(OUT, `bootlog_${String(run).padStart(3, "0")}.log`);
  writeFileSync(log, `=== run ${run} url=${URL_} ===\n`);
  const t0 = Date.now(); const ts = () => ((Date.now() - t0) / 1000).toFixed(1);
  let mapped = false, at = 0, b = null;
  try {
    b = await launch();
    const pg = await b.newPage();
    pg.on("console", (m) => { const t = m.text(); appendFileSync(log, `[${ts()}s] ${t.slice(0, 400)}\n`);
      if (!mapped && t.includes("XWire: first window mapped")) { mapped = true; at = ts(); } });
    pg.on("pageerror", (e) => appendFileSync(log, `[${ts()}s] PAGEERROR ${e.message}\n${e.stack || ""}\n`));
    await pg.goto(URL_, { waitUntil: "domcontentloaded", timeout: 60000 });
    while (Date.now() - t0 < Number(WALL_) && !mapped) await sleep(1000);
    if (mapped) await sleep(3000);
  } catch (e) { appendFileSync(log, `HARNESS-ERROR ${e.message}\n`); }
  finally { try { if (b) await b.close(); } catch {} }
  if (mapped) pass++;
  console.log(`run ${run}: ${mapped ? "PASS mapped@" + at + "s" : "FAIL"}`);
}
console.log(`TALLY: ${pass}/${RUNS_} passed`);
