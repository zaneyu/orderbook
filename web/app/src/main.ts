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
const fmtK = (n: number) => (n >= 1e6 ? (n / 1e6).toFixed(2) + "M" : (n / 1e3).toFixed(1) + "k");

const $ = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id) as T;

// ---- ladder rows (built once, updated each frame) ---------------------------
type Row = {
  el: HTMLElement;
  p: HTMLElement;
  z: HTMLElement;
  bar: HTMLElement;
  last: number;
  side: "ask" | "bid";
  tick: number;
  idx: number;
};

let askRows: Row[] = [];
let bidRows: Row[] = [];
let lastAsks: [number, number][] = [];
let lastBids: [number, number][] = [];

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
    const row: Row = { el, p, z, bar, last: -1, side, tick: -1, idx: -1 };
    (el as any)._row = row;
    rows.push(row);
  }
  return rows;
}

function paint(row: Row, level: [number, number] | null, idx: number, max: number, animate: boolean) {
  row.idx = idx;
  if (!level) {
    row.tick = -1;
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
  row.tick = tick;
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
let reduceMotion = false;
let side = 0; // 0 buy, 1 sell

async function boot() {
  const obUrl = new URL("ob.js", document.baseURI).href;
  const mod: any = await import(/* @vite-ignore */ obUrl);
  const Module = await mod.default();
  sim = new Module.Sim(TICKS);

  askRows = buildRows($("asks"), "ask");
  bidRows = buildRows($("bids"), "bid");
  wireControls();
  wireLadder();

  $("boot").remove();
  $("app").hidden = false;
  reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  const frame = () => {
    if (running && speed > 0) {
      sim.step(speed);
      stepAccum += speed;
    }
    render(sim.snapshot(DEPTH));
    requestAnimationFrame(frame);
  };
  requestAnimationFrame(frame);
}

function render(snap: any) {
  lastAsks = snap.asks;
  lastBids = snap.bids;
  let max = 1;
  for (const [, q] of lastAsks) max = Math.max(max, q);
  for (const [, q] of lastBids) max = Math.max(max, q);

  for (let i = 0; i < DEPTH; i++) paint(askRows[DEPTH - 1 - i], lastAsks[i] ?? null, i, max, !reduceMotion);
  for (let i = 0; i < DEPTH; i++) paint(bidRows[i], lastBids[i] ?? null, i, max, !reduceMotion);

  const last = snap.lastTrade as number;
  const lastEl = $("last");
  lastEl.textContent = fmtP(last);
  lastEl.className = last > prevLast ? "up" : last < prevLast ? "down" : "";
  prevLast = last;
  $("bid").textContent = fmtP(snap.bestBid);
  $("ask").textContent = fmtP(snap.bestAsk);
  const sp = snap.bestBid >= 0 && snap.bestAsk >= 0 ? (snap.bestAsk - snap.bestBid) * TICK_SIZE : null;
  $("spread").textContent = sp === null ? "—" : sp.toFixed(2);
  $("midtxt").textContent = sp === null ? "" : `spread ${sp.toFixed(2)}`;

  const priceEl = $<HTMLInputElement>("price");
  if (document.activeElement !== priceEl && !priceEl.disabled) {
    const midTick =
      snap.bestBid >= 0 && snap.bestAsk >= 0 ? Math.round((snap.bestBid + snap.bestAsk) / 2) : MIDTICK;
    priceEl.value = fmtP(midTick);
  }

  const tape: { price: number; qty: number }[] = snap.tape;
  let html = "";
  for (let i = 0; i < Math.min(tape.length, 22); i++) {
    const t = tape[i];
    const older = tape[i + 1];
    const dir = older ? (t.price > older.price ? "up" : t.price < older.price ? "down" : "") : "";
    html += `<div class="trow ${dir}"><span class="tp">${fmtP(t.price)}</span><span class="tz">${fmtN(t.qty)}</span></div>`;
  }
  $("tape").innerHTML = html;

  $("trades").textContent = fmtN(snap.trades);
  $("resting").textContent = fmtN(snap.resting);
  const now = performance.now();
  if (now - rateT > 500) {
    $("sps").textContent = fmtN((stepAccum / (now - rateT)) * 1000);
    stepAccum = 0;
    rateT = now;
  }

  if (hoverIdx >= 0) refreshCum(); // keep cumulative highlight live under a held cursor
}

// ---- direct ladder interaction ---------------------------------------------
let hoverSide: "ask" | "bid" | null = null;
let hoverIdx = -1;

function wireLadder() {
  for (const host of [$("asks"), $("bids")]) {
    host.addEventListener("mousemove", (e) => {
      const el = (e.target as HTMLElement).closest(".lrow") as any;
      const r: Row | undefined = el?._row;
      if (!r || r.tick < 0) return clearCum();
      hoverSide = r.side;
      hoverIdx = r.idx;
      refreshCum();
    });
    host.addEventListener("mouseleave", clearCum);
    host.addEventListener("click", (e) => {
      const el = (e.target as HTMLElement).closest(".lrow") as any;
      const r: Row | undefined = el?._row;
      if (!r || r.tick < 0) return;
      placeAt(r);
    });
  }
}

function refreshCum() {
  if (!hoverSide || hoverIdx < 0) return;
  const arr = hoverSide === "ask" ? lastAsks : lastBids;
  let cq = 0;
  let cn = 0;
  for (let k = 0; k <= hoverIdx && k < arr.length; k++) {
    const [t, q] = arr[k];
    cq += q;
    cn += (toPrice(t) || 0) * q;
  }
  for (const row of askRows) row.el.classList.toggle("cum", hoverSide === "ask" && row.tick >= 0 && row.idx <= hoverIdx);
  for (const row of bidRows) row.el.classList.toggle("cum", hoverSide === "bid" && row.tick >= 0 && row.idx <= hoverIdx);
  $("cum").textContent = `Σ ${fmtN(cq)} · $${fmtK(cn)}`;
}

function clearCum() {
  hoverSide = null;
  hoverIdx = -1;
  for (const row of askRows) row.el.classList.remove("cum");
  for (const row of bidRows) row.el.classList.remove("cum");
  $("cum").textContent = "";
}

function placeAt(r: Row) {
  const qty = Math.max(1, parseInt($<HTMLInputElement>("qty").value) || 0);
  const s = r.side === "bid" ? 0 : 1; // click the bid side to buy, the ask side to sell
  setSide(s);
  const res = sim.submit(0, s, r.tick, qty);
  showFill(`limit ${s ? "sell" : "buy"} ${qty} @ ${fmtP(r.tick)} → filled ${res.filled}, resting ${res.resting}`);
  r.el.classList.remove("flash");
  void r.el.offsetWidth;
  r.el.classList.add("flash");
}

// ---- form controls ----------------------------------------------------------
function setSide(v: number) {
  side = v;
  $("sideSeg")
    .querySelectorAll("button")
    .forEach((b) => b.classList.toggle("on", +(b.dataset.v || 0) === v));
}
function showFill(msg: string) {
  const el = $("fillmsg");
  el.textContent = msg;
  el.classList.add("show");
  window.clearTimeout((showFill as any)._t);
  (showFill as any)._t = window.setTimeout(() => el.classList.remove("show"), 2200);
}
function togglePlay() {
  running = !running;
  $("play").textContent = running ? "Pause" : "Play";
}

function wireControls() {
  $("play").textContent = "Pause";
  $("play").onclick = togglePlay;
  $("reset").onclick = () => {
    sim.reset();
    prevLast = -1;
  };
  const speedEl = $<HTMLInputElement>("speed");
  speed = +speedEl.value;
  $("speedv").textContent = String(speed);
  speedEl.oninput = () => {
    speed = +speedEl.value;
    $("speedv").textContent = String(speed);
  };

  $("sideSeg").querySelectorAll("button").forEach((b) =>
    b.addEventListener("click", () => setSide(+(b.dataset.v || 0))),
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
    showFill(`${names[type]} ${side ? "sell" : "buy"} ${qty} → filled ${res.filled}, resting ${res.resting}`);
  };

  // spacebar toggles the simulation (unless typing in a field)
  window.addEventListener("keydown", (e) => {
    if (e.code === "Space" && !(e.target instanceof HTMLInputElement) && !(e.target instanceof HTMLSelectElement)) {
      e.preventDefault();
      togglePlay();
    }
  });
}

boot().catch((err) => {
  const b = document.getElementById("boot");
  if (b) b.textContent = "failed to load the engine: " + err;
});
