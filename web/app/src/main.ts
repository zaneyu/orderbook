import "./style.css";

// ---- price domain -----------------------------------------------------------
const TICKS = 2048;
const MIDTICK = 1024;
const TICK_SIZE = 0.01;
const DEPTH = 11;

const toPrice = (t: number) => (t < 0 ? null : 100 + (t - MIDTICK) * TICK_SIZE);
const fmtP = (t: number) => {
  const p = toPrice(t);
  return p === null ? "—" : p.toFixed(2);
};
const toTick = (price: number) =>
  Math.max(0, Math.min(TICKS - 1, Math.round((price - 100) / TICK_SIZE + MIDTICK)));
const fmtN = (n: number) => Math.round(n).toLocaleString("en-US");

const $ = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id) as T;

// ---- ladder rows (built once, updated each frame) ---------------------------
type Row = { el: HTMLElement; p: HTMLElement; z: HTMLElement; bar: HTMLElement; last: number };
function buildRows(host: HTMLElement, side: "ask" | "bid"): Row[] {
  const rows: Row[] = [];
  for (let i = 0; i < DEPTH; i++) {
    const el = document.createElement("div");
    el.className = `lrow ${side} empty`;
    const p = document.createElement("span");
    p.className = "lp";
    const z = document.createElement("span");
    z.className = "lz";
    const bar = document.createElement("i");
    bar.className = "lbar";
    el.append(p, z, bar);
    host.appendChild(el);
    rows.push({ el, p, z, bar, last: -1 });
  }
  return rows;
}

function paint(row: Row, level: [number, number] | null, max: number, animate: boolean) {
  if (!level) {
    if (!row.el.classList.contains("empty")) {
      row.el.classList.add("empty");
      row.p.textContent = "";
      row.z.textContent = "";
      row.bar.style.transform = "scaleX(0)";
      row.last = -1;
    }
    return;
  }
  const [tick, qty] = level;
  row.el.classList.remove("empty");
  row.p.textContent = fmtP(tick);
  row.z.textContent = fmtN(qty);
  row.bar.style.transform = `scaleX(${Math.max(0.02, qty / max)})`;
  if (animate && row.last !== -1 && row.last !== qty) {
    row.el.classList.remove("flash");
    void row.el.offsetWidth; // restart the animation
    row.el.classList.add("flash");
  }
  row.last = qty;
}

// ---- boot -------------------------------------------------------------------
let sim: any;
let running = true;
let speed = 250;
let prevLast = -1;
let stepAccum = 0;
let rateT = performance.now();

async function boot() {
  // ob.js/ob.wasm live at the deploy root (Vite public/); resolve against the
  // document so it works from a subpath too, not relative to the bundled module.
  const obUrl = new URL("ob.js", document.baseURI).href;
  const mod: any = await import(/* @vite-ignore */ obUrl);
  const Module = await mod.default();
  sim = new Module.Sim(TICKS);

  const askRows = buildRows($("asks"), "ask");
  const bidRows = buildRows($("bids"), "bid");
  wireControls();

  $("boot").remove();
  $("app").hidden = false;
  reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  const frame = () => {
    if (running && speed > 0) {
      sim.step(speed);
      stepAccum += speed;
    }
    render(sim.snapshot(DEPTH), askRows, bidRows);
    requestAnimationFrame(frame);
  };
  requestAnimationFrame(frame);
}

let reduceMotion = false;

function render(snap: any, askRows: Row[], bidRows: Row[]) {
  const asks: [number, number][] = snap.asks;
  const bids: [number, number][] = snap.bids;
  let max = 1;
  for (const [, q] of asks) max = Math.max(max, q);
  for (const [, q] of bids) max = Math.max(max, q);

  // asks: best (index 0) sits at the bottom, nearest the mid line
  for (let i = 0; i < DEPTH; i++) paint(askRows[DEPTH - 1 - i], asks[i] ?? null, max, !reduceMotion);
  for (let i = 0; i < DEPTH; i++) paint(bidRows[i], bids[i] ?? null, max, !reduceMotion);

  // quote header
  const last = snap.lastTrade as number;
  const lastEl = $("last");
  lastEl.textContent = fmtP(last);
  lastEl.className = last > prevLast ? "up" : last < prevLast ? "down" : "";
  prevLast = last;
  $("bid").textContent = fmtP(snap.bestBid);
  $("ask").textContent = fmtP(snap.bestAsk);
  const sp =
    snap.bestBid >= 0 && snap.bestAsk >= 0 ? (snap.bestAsk - snap.bestBid) * TICK_SIZE : null;
  $("spread").textContent = sp === null ? "—" : sp.toFixed(2);
  $("midtxt").textContent = sp === null ? "" : `spread ${sp.toFixed(2)}`;

  // keep the order-entry price on the live touch unless the user is editing it
  const priceEl = $<HTMLInputElement>("price");
  if (document.activeElement !== priceEl && !priceEl.disabled) {
    const midTick =
      snap.bestBid >= 0 && snap.bestAsk >= 0
        ? Math.round((snap.bestBid + snap.bestAsk) / 2)
        : MIDTICK;
    priceEl.value = fmtP(midTick);
  }

  // tape
  const tape: { price: number; qty: number }[] = snap.tape;
  const host = $("tape");
  let html = "";
  for (let i = 0; i < Math.min(tape.length, 22); i++) {
    const t = tape[i];
    const older = tape[i + 1];
    const dir = older ? (t.price > older.price ? "up" : t.price < older.price ? "down" : "") : "";
    html += `<div class="trow ${dir}"><span class="tp">${fmtP(t.price)}</span><span class="tz">${fmtN(t.qty)}</span></div>`;
  }
  host.innerHTML = html;

  // stats
  $("trades").textContent = fmtN(snap.trades);
  $("resting").textContent = fmtN(snap.resting);
  const now = performance.now();
  if (now - rateT > 500) {
    $("sps").textContent = fmtN((stepAccum / (now - rateT)) * 1000);
    stepAccum = 0;
    rateT = now;
  }
}

// ---- controls ---------------------------------------------------------------
let side = 0; // 0 buy, 1 sell

function wireControls() {
  const play = $("play");
  play.textContent = "Pause";
  play.onclick = () => {
    running = !running;
    play.textContent = running ? "Pause" : "Play";
  };
  $("reset").onclick = () => {
    sim.reset();
    prevLast = -1;
  };
  const speedEl = $<HTMLInputElement>("speed");
  const speedv = $("speedv");
  speed = +speedEl.value;
  speedv.textContent = String(speed);
  speedEl.oninput = () => {
    speed = +speedEl.value;
    speedv.textContent = String(speed);
  };

  $("sideSeg").querySelectorAll("button").forEach((b) =>
    b.addEventListener("click", () => {
      side = +(b.dataset.v || 0);
      $("sideSeg")
        .querySelectorAll("button")
        .forEach((x) => x.classList.toggle("on", x === b));
    }),
  );

  const typeEl = $<HTMLSelectElement>("type");
  const priceEl = $<HTMLInputElement>("price");
  typeEl.onchange = () => {
    priceEl.disabled = typeEl.value === "1"; // market ignores price (CSS greys :disabled)
  };

  $<HTMLFormElement>("order").onsubmit = (e) => {
    e.preventDefault();
    const type = +typeEl.value;
    const qty = Math.max(1, parseInt($<HTMLInputElement>("qty").value) || 0);
    const tick = toTick(parseFloat(priceEl.value) || 100);
    const res = sim.submit(type, side, tick, qty);
    const names = ["limit", "market", "IOC", "FOK"];
    const msg = $("fillmsg");
    msg.textContent = `${names[type]} ${side ? "sell" : "buy"} ${qty} → filled ${res.filled}, resting ${res.resting}`;
    msg.classList.add("show");
    setTimeout(() => msg.classList.remove("show"), 1800);
  };
}

boot().catch((err) => {
  const b = document.getElementById("boot");
  if (b) b.textContent = "failed to load the engine: " + err;
});
