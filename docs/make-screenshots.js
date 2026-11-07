// Makes README screenshots of the Bluefy control page and the flasher.
// Runs in GitHub Actions (see .github/workflows/screenshots.yml).
// The Heltec board and the phone GPS are SIMULATED: no real device or location is used.
const { chromium } = require("playwright");
const path = require("path");
const fs = require("fs");

const CONTROL_URL = process.env.CONTROL_URL || "https://charlesphillips3.github.io/rf-survey-control/";
const OUT = path.join(__dirname, "screenshots");
fs.mkdirSync(OUT, { recursive: true });

// simulated board + GPS, injected before the page loads
const MOCK = () => {
  const SVC = "8a1c0001-5b2f-4c1e-9a6d-2f0b6e3c9d10";
  const STAT = "8a1c0003-5b2f-4c1e-9a6d-2f0b6e3c9d10", SPEC = "8a1c0004-5b2f-4c1e-9a6d-2f0b6e3c9d10";
  const chrs = {};
  const mk = u => { const c = new EventTarget(); c.uuid = u; c.startNotifications = async () => c;
    c.writeValueWithoutResponse = async d => { const t = new TextDecoder().decode(d); if (t[0] === "G") window.__go(t.slice(1)); }; return c; };
  const svc = { getCharacteristic: async u => (chrs[u] = chrs[u] || mk(u)) };
  const dev = new EventTarget(); dev.name = "RF-Survey-5510";
  dev.gatt = { connect: async () => ({ getPrimaryService: async () => svc }), disconnect() {} };
  Object.defineProperty(navigator, "bluetooth", { value: { requestDevice: async () => dev } });

  // simulated drive along a made-up route (generic location)
  let lat = 41.8800, lon = -87.6400, step = 0;
  Object.defineProperty(navigator, "geolocation", { value: { watchPosition(ok) {
    setInterval(() => { step++; if (step < 60) lat += 0.00018; else lon += 0.00022;
      ok({ coords: { latitude: lat, longitude: lon, accuracy: 4, altitude: 180 } }); }, 150); return 1; } } });

  const notify = (u, bytes) => { const c = chrs[u]; if (!c) return; const e = new Event("characteristicvaluechanged");
    Object.defineProperty(e, "target", { value: { value: new DataView(bytes.buffer) } }); c.dispatchEvent(e); };
  let k = 0, stn = null, stnId = 0, rows = 0;
  const rnd = () => Math.random();
  window.__go = label => { if (stn) { stn.left = 0; return; } stnId++; stn = { id: stnId, left: 60, start: Date.now() }; };
  setInterval(() => {
    if (!chrs[STAT]) return;
    k++; rows++;
    const hot = Math.exp(-(((k - 45) / 12) ** 2)) * 9;             // a noisier stretch mid-drive
    const spec = Array.from({ length: 130 }, (_, i) => -118 + hot + (rnd() - .5) * 3 + (rnd() < .04 ? 20 + rnd() * 12 : 0));
    const s = [...spec].sort((a, b) => a - b), f = s[26], m = s[65], p = Math.max(...spec) + 3;
    const pf = 902.1 + 0.2 * spec.indexOf(Math.max(...spec));
    if (stn) stn.left = Math.max(0, stn.left - 6);
    notify(STAT, new TextEncoder().encode(JSON.stringify({ t: "s", f: +f.toFixed(1), m: +m.toFixed(1), p: +p.toFixed(1), pf: +pf.toFixed(1),
      b: spec.filter(v => v > f + 10).length, sy: 1, lg: 1, sa: stn ? 1 : 0, si: stnId, sl: stn ? stn.left : 0, ss: k, sn: 60,
      r: rows, fs: 3, pa: 1, w: 1, bt: 12, rs: 0, bv: 3960 - Math.floor(k / 10) })));
    const b = new Uint8Array(132); b[0] = 0; spec.forEach((v, i) => b[2 + i] = Math.round((v + 160) * 2)); notify(SPEC, b);
    if (stn && stn.left === 0) {
      const mean = Array.from({ length: 130 }, () => -117 + hot + (rnd() - .5));
      const ms = [...mean].sort((a, b) => a - b);
      notify(STAT, new TextEncoder().encode(JSON.stringify({ t: "st", id: stn.id, f: +ms[26].toFixed(1), m: +ms[65].toFixed(1),
        p: +(Math.max(...mean) + 9).toFixed(1), pf: 915.3, b: 1, sw: 180, e: 0, d: Math.max(1, Math.round((Date.now() - stn.start) / 1000)) })));
      const sb = new Uint8Array(132); sb[0] = 1; sb[1] = stn.id; mean.forEach((v, i) => sb[2 + i] = Math.round((v + 160) * 2)); notify(SPEC, sb);
      stn = null;
    }
  }, 300);
};

(async () => {
  const browser = await chromium.launch();
  const phone = { viewport: { width: 390, height: 844 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true,
                  userAgent: "Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Bluefy" };
  const ctx = await browser.newContext(phone);
  const p = await ctx.newPage();
  await p.addInitScript(MOCK);
  await p.goto(CONTROL_URL, { waitUntil: "networkidle" });
  await p.evaluate(() => { localStorage.clear(); });
  await p.reload({ waitUntil: "networkidle" });
  await p.click("#btnConn");
  await p.waitForTimeout(9000);                       // drive a bit
  await p.fill("#label", "Stop 1");
  await p.click("#btnGo");
  await p.waitForTimeout(1200);
  await p.screenshot({ path: path.join(OUT, "control-go.png") });            // GO countdown, top of page
  await p.waitForTimeout(5000);                       // station finishes
  await p.fill("#label", "Stop 2");
  await p.waitForTimeout(6000);
  await p.click("#btnGo");
  await p.waitForTimeout(5500);
  await p.waitForTimeout(6000);                       // keep driving
  await p.evaluate(() => { document.getElementById("map").scrollIntoView({ block: "center" }); });
  await p.waitForTimeout(2500);                       // let tiles load
  await p.screenshot({ path: path.join(OUT, "control-map.png") });
  await p.evaluate(() => window.scrollTo(0, 0));
  await p.screenshot({ path: path.join(OUT, "control-full.png"), fullPage: true });

  // flasher (desktop size): prefer the newest release's flasher, which has the firmware built in
  const relFlasher = path.join(__dirname, "..", "release", "heltec-flasher.html");
  const flasher = fs.existsSync(relFlasher) ? relFlasher : path.join(__dirname, "..", "flasher", "index.html");
  console.log("flasher screenshot from:", flasher);
  const d = await browser.newPage({ viewport: { width: 900, height: 900 }, deviceScaleFactor: 1.5 });
  await d.goto("file://" + flasher);
  await d.waitForTimeout(1000);
  await d.screenshot({ path: path.join(OUT, "flasher.png"), fullPage: true });

  await browser.close();
  console.log("screenshots:", fs.readdirSync(OUT));
})();
