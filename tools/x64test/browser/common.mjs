// Shared headless-Chrome launcher for the browser test probes (puppeteer from
// the global npm root; system Chrome). Tall viewport so the emulator canvas is
// on screen without resizing the page mid-run (a resize triggered a worker
// error once — see roadmap Log 2026-10-23).
import { createRequire } from "node:module";
const require = createRequire("/opt/homebrew/lib/node_modules/");
export const puppeteer = require("puppeteer");
export const CHROME = process.env.CHROME || "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
export async function launch() {
  return puppeteer.launch({ executablePath: CHROME, headless: "new", defaultViewport: { width: 1100, height: 1300 },
    args: ["--no-first-run", "--enable-features=SharedArrayBuffer", "--use-gl=angle", "--use-angle=swiftshader",
           "--enable-unsafe-swiftshader", "--ignore-gpu-blocklist", "--disable-dev-shm-usage"] });
}
export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
