import "./style.css";

// ---- price domain -----------------------------------------------------------
// Integer ticks of $0.01 over a wide band ($0.01–$999.99); the instrument opens at
// $100.00 (tick 10000). The band is far enough from normal movement that it never
// binds — a real venue's price band, not an artificial ±10% wall.
const TICK_SIZE = 0.01;
const TICKS = 100000;
const START_TICK = 10000; // $100.00
const DEPTH = 11;

const toPrice = (t: number) => (t < 0 ? null : t * TICK_SIZE);
const fmtP = (t: number) => {
  const p = toPrice(t);
  return p === null ? "—" : p.toFixed(2);
};
const toTick = (price: number) =>
  Math.max(1, Math.min(TICKS - 2, Math.round(price / TICK_SIZE)));
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
let priceDirty = false; // once the user edits the price, stop auto-tracking the mid (WYSIWYG)

async function boot() {
  const obUrl = new URL("ob.js", document.baseURI).href;
  const mod: any = await import(/* @vite-ignore */ obUrl);
  const Module = await mod.default();
  sim = new Module.Sim(TICKS, START_TICK);

  askRows = buildRows($("asks"), "ask");
  bidRows = buildRows($("bids"), "bid");
  spark = $<HTMLCanvasElement>("spark");
  sctx = spark.getContext("2d")!;
  sizeSpark();
  window.addEventListener("resize", sizeSpark);
  wireControls();
  wireLadder();

  $("boot").remove();
  $("app").hidden = false;
  reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  let lastFrame = performance.now();
  const frame = () => {
    const now = performance.now();
    let dt = (now - lastFrame) / (1000 / 60); // wall-clock, in 60fps-frame units
    lastFrame = now;
    if (!Number.isFinite(dt) || dt < 0) dt = 0;
    if (dt > 4) dt = 4; // cap a hitch / tab-away so the price doesn't leap
    if (running && speed > 0) {
      sim.step(speed, dt);
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

  // Track the live mid ONLY until the user takes over the field. Never overwrite a
  // price the user typed — doing so once turned a "sell @ 120" into a market sell.
  const priceEl = $<HTMLInputElement>("price");
  if (!priceDirty && document.activeElement !== priceEl && !priceEl.disabled) {
    const midTick =
      snap.bestBid >= 0 && snap.bestAsk >= 0 ? Math.round((snap.bestBid + snap.bestAsk) / 2) : START_TICK;
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

  updateMarketState(snap);
  renderMine(snap);

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

// ---- your working orders: track each user limit order over its life ---------
type Mine = { side: number; price: number; orig: number; filled: number; avgTick: number; done: boolean };
const mineFilled = new Map<string, number>(); // key -> last filled, to flash on a new fill

function renderMine(snap: any) {
  const mine: Mine[] = snap.mine || [];
  const box = $("myorders");
  if (!mine.length) {
    if (!box.hidden) box.hidden = true;
    return;
  }
  box.hidden = false;
  const mid = toPrice(snap.midTick);
  let html = "";
  const seen = new Set<string>();
  for (let i = 0; i < mine.length; i++) {
    const o = mine[i];
    const key = `${o.side}:${o.price}:${o.orig}:${i}`;
    seen.add(key);
    const dir = o.side === 0 ? 1 : -1;
    const avg = o.avgTick >= 0 ? toPrice(o.avgTick) : null;
    const pnl = avg !== null && mid !== null && o.filled > 0 ? (mid - avg) * o.filled * dir : null;
    const pcls = pnl === null ? "" : pnl > 0.0005 ? "up" : pnl < -0.0005 ? "down" : "";
    const pct = Math.round((o.filled / o.orig) * 100);
    const status = o.done ? "filled" : o.filled > 0 ? `${pct}% working` : "resting";
    const flash = mineFilled.has(key) && (mineFilled.get(key) as number) < o.filled ? " hit" : "";
    mineFilled.set(key, o.filled);
    html +=
      `<div class="mo ${o.side === 0 ? "buy" : "sell"}${o.done ? " done" : ""}${flash}">` +
      `<span class="mo-side">${o.side === 0 ? "BUY" : "SELL"}</span>` +
      `<span class="mo-qty mono">${fmtN(o.filled)}/${fmtN(o.orig)}</span>` +
      `<span class="mo-at mono">@ ${fmtP(o.price)}</span>` +
      `<span class="mo-avg mono">avg ${avg !== null ? avg.toFixed(2) : "—"}</span>` +
      `<span class="mo-pnl mono ${pcls}">${pnl === null ? "" : (pnl >= 0 ? "+" : "") + pnl.toFixed(2)}</span>` +
      `<span class="mo-status">${status}</span>` +
      `<i class="mo-bar" style="transform:scaleX(${o.filled / o.orig})"></i>` +
      `</div>`;
  }
  for (const k of mineFilled.keys()) if (!seen.has(k)) mineFilled.delete(k);
  $("molist").innerHTML = html;
}

// ---- market state: regime badge, volatility meter, live price chart ---------
const HIST = 260;
const hist: number[] = []; // recent mid, in price
let spark: HTMLCanvasElement;
let sctx: CanvasRenderingContext2D;
let sw = 0;
let sh = 0;

function sizeSpark() {
  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  const r = spark.getBoundingClientRect();
  sw = Math.max(1, Math.round(r.width));
  sh = Math.max(1, Math.round(r.height));
  spark.width = Math.round(sw * dpr);
  spark.height = Math.round(sh * dpr);
  sctx.setTransform(dpr, 0, 0, dpr, 0, 0);
}

function css(name: string) {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}

function updateMarketState(snap: any) {
  const mid = toPrice(snap.midTick);
  if (mid !== null) {
    hist.push(mid);
    if (hist.length > HIST) hist.shift();
  }

  const trend = snap.trend as number; // -1..1
  const vol01 = snap.vol01 as number; // 0..1
  const reg = $("regime");
  let label: string, cls: string;
  if (trend > 0.18) {
    label = "trending up";
    cls = "up";
  } else if (trend < -0.18) {
    label = "trending down";
    cls = "down";
  } else {
    label = "ranging";
    cls = "flat";
  }
  if (vol01 > 0.62) label += " · volatile";
  reg.textContent = label;
  reg.className = cls;

  $("volfill").style.transform = `scaleX(${Math.max(0.02, vol01)})`;
  $("volfill").className = vol01 > 0.62 ? "hot" : "";

  drawSpark(trend);
}

function drawSpark(trend: number) {
  if (!sctx || hist.length < 2) return;
  sctx.clearRect(0, 0, sw, sh);
  let lo = Infinity;
  let hi = -Infinity;
  for (const v of hist) {
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  const pad = Math.max((hi - lo) * 0.12, 0.02);
  lo -= pad;
  hi += pad;
  const span = hi - lo || 1;
  const padX = 1;
  const x = (i: number) => padX + (i / (HIST - 1)) * (sw - 2 * padX);
  const y = (v: number) => sh - 4 - ((v - lo) / span) * (sh - 8);
  const n = hist.length;
  const off = HIST - n; // right-align the newest sample

  const line = trend > 0.18 ? css("--bid") : trend < -0.18 ? css("--ask") : css("--amber");

  // faint area fill under the line
  sctx.beginPath();
  sctx.moveTo(x(off), sh);
  for (let i = 0; i < n; i++) sctx.lineTo(x(off + i), y(hist[i]));
  sctx.lineTo(x(off + n - 1), sh);
  sctx.closePath();
  sctx.fillStyle = line;
  sctx.globalAlpha = 0.07;
  sctx.fill();
  sctx.globalAlpha = 1;

  // the price line
  sctx.beginPath();
  for (let i = 0; i < n; i++) {
    const px = x(off + i);
    const py = y(hist[i]);
    i === 0 ? sctx.moveTo(px, py) : sctx.lineTo(px, py);
  }
  sctx.lineWidth = 1.5;
  sctx.strokeStyle = line;
  sctx.lineJoin = "round";
  sctx.stroke();

  // marker on the latest price
  const lx = x(off + n - 1);
  const ly = y(hist[n - 1]);
  sctx.beginPath();
  sctx.arc(lx, ly, 2.2, 0, Math.PI * 2);
  sctx.fillStyle = line;
  sctx.fill();
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
    hist.length = 0;
    mineFilled.clear();
  };

  $("bull").onclick = () => {
    sim.news(1);
    showFill("bullish news shock — fair value jumps up");
  };
  $("bear").onclick = () => {
    sim.news(-1);
    showFill("bearish news shock — fair value jumps down");
  };
  const turbEl = $<HTMLInputElement>("turb");
  const applyTurb = () => sim.setTurbulence(+turbEl.value / 100);
  turbEl.oninput = applyTurb;
  applyTurb();
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
  // Editing the price takes it out of auto-track; clearing it hands tracking back.
  priceEl.addEventListener("input", () => {
    priceDirty = priceEl.value.trim() !== "";
  });

  $<HTMLFormElement>("order").onsubmit = (e) => {
    e.preventDefault();
    const type = +typeEl.value;
    const qty = Math.max(1, parseInt($<HTMLInputElement>("qty").value) || 0);
    const typed = parseFloat(priceEl.value) || 100;
    const tick = toTick(typed);
    const eff = toPrice(tick); // what the book actually used, after clamping to a valid tick
    const res = sim.submit(type, side, tick, qty);
    const names = ["limit", "market", "IOC", "FOK"];
    const at = type === 1 ? "" : ` @ ${fmtP(tick)}`;
    const clamped = type !== 1 && eff !== null && Math.abs(eff - typed) > 0.005 ? ` (clamped from ${typed.toFixed(2)})` : "";
    showFill(`${names[type]} ${side ? "sell" : "buy"} ${qty}${at}${clamped} → filled ${res.filled}, resting ${res.resting}`);
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
