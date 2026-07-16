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
  mk: HTMLElement;
  mkClass: string;
  last: number;
  side: "ask" | "bid";
  tick: number;
  idx: number;
};

let askRows: Row[] = [];
let bidRows: Row[] = [];
let lastAsks: [number, number][] = [];
let lastBids: [number, number][] = [];
// tick -> owner marker ("mm" = the market maker's quote, "you" = your resting order)
const orderMarks = new Map<number, string>();

function buildRows(host: HTMLElement, side: "ask" | "bid"): Row[] {
  const rows: Row[] = [];
  for (let i = 0; i < DEPTH; i++) {
    const el = document.createElement("div");
    el.className = `lrow ${side} empty`;
    el.setAttribute("role", "button"); // keyboard-operable: focus + Enter/Space places an order
    const p = document.createElement("span");
    p.className = "lp";
    const z = document.createElement("span");
    z.className = "lz";
    const bar = document.createElement("i");
    bar.className = "lbar";
    const mk = document.createElement("i");
    mk.className = "lmark";
    el.append(p, z, bar, mk);
    host.appendChild(el);
    const row: Row = { el, p, z, bar, mk, mkClass: "lmark", last: -1, side, tick: -1, idx: -1 };
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
      row.el.removeAttribute("tabindex"); // empty levels aren't focusable/actionable
      row.el.removeAttribute("aria-label");
      row.p.textContent = "";
      row.z.textContent = "";
      row.bar.style.transform = "scaleX(0)";
      if (row.mkClass !== "lmark") { row.mk.className = row.mkClass = "lmark"; }
      row.last = -1;
    }
    return;
  }
  const [tick, qty] = level;
  const tickChanged = row.tick !== tick;
  row.tick = tick;
  row.el.classList.remove("empty");
  if (row.el.tabIndex !== 0) row.el.tabIndex = 0;
  const owner = orderMarks.get(tick); // "mm" or "you" if a bot/you rest here
  const mkClass = owner ? `lmark ${owner}` : "lmark";
  const ownerChanged = mkClass !== row.mkClass;
  if (ownerChanged) { row.mk.className = row.mkClass = mkClass; }
  // Rewrite the label only when the level/owner changed or the row is focused (a screen
  // reader needs fresh size there) — rebuilding 22 labels per frame sprays mutations at
  // assistive tech ~60x/s for sizes nobody is reading.
  if (tickChanged || ownerChanged || document.activeElement === row.el)
    row.el.setAttribute(
      "aria-label",
      `${row.side === "bid" ? "bid" : "ask"} ${fmtP(tick)}, size ${fmtN(qty)}` +
        (owner === "mm" ? ", market maker quote" : owner === "you" ? ", your order" : "") +
        `, ${row.side === "bid" ? "buy" : "sell"} here`,
    );
  row.p.textContent = fmtP(tick);
  row.z.textContent = fmtN(qty);
  row.bar.style.transform = `scaleX(${Math.max(0.02, qty / max)})`;
  // Flash only on a qty change AT THE SAME price — a row reused for a different level
  // (book shifted) inherits a different qty without any trade having happened there.
  if (animate && !tickChanged && row.last !== -1 && row.last !== qty) {
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
let lastTapeHTML = "";
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
  window.addEventListener("resize", sizeSpark);
  wireControls();
  wireLadder();

  $("boot").remove();
  $("app").hidden = false;
  sizeSpark(); // size the canvas AFTER the app is laid out (hidden => zero size)
  const mq = window.matchMedia("(prefers-reduced-motion: reduce)");
  reduceMotion = mq.matches;
  mq.addEventListener("change", (e) => (reduceMotion = e.matches));

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

  // mark which visible levels hold the market maker's quotes or your own resting orders
  orderMarks.clear();
  const mm = snap.mm;
  if (mm && mm.on) {
    if (mm.bidPx >= 0) orderMarks.set(mm.bidPx, "mm");
    if (mm.askPx >= 0) orderMarks.set(mm.askPx, "mm");
  }
  for (const o of (snap.mine as any[]) ?? []) if (!o.done && o.filled < o.orig) orderMarks.set(o.price, "you");

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
  if (html !== lastTapeHTML) {
    $("tape").innerHTML = html; // only reparse when the tape actually changed
    lastTapeHTML = html;
  }

  lastMidPrice = toPrice(snap.midTick) ?? lastMidPrice;
  updateMarketState(snap);
  renderMine(snap);
  updateExec(snap);
  updateStrats(snap);

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
type Mine = { id: number; side: number; price: number; orig: number; filled: number; avgTick: number; done: boolean };
const mineFilled = new Map<number, number>(); // order id -> last filled, to flash on a new fill
let lastEvicted = 0; // sim's cumulative live-eviction counter, to surface cap evictions
let lastMineHTML = "";

function renderMine(snap: any) {
  const mine: Mine[] = snap.mine || [];
  // The sim counts LIVE orders cancelled by the tracked-order cap; toast on increase.
  // (Inferring eviction from an id vanishing false-fired when a crossing order FILLED
  // resting orders and their husks were purged within the same submit call.)
  const ev = (snap.evicted as number) ?? 0;
  if (ev > lastEvicted) showFill("order cap reached — oldest working order cancelled");
  lastEvicted = ev; // assignment also handles the counter dropping to 0 on reset
  const box = $("myorders");
  if (!mine.length) {
    if (!box.hidden) box.hidden = true;
    return;
  }
  box.hidden = false;
  const mid = toPrice(snap.midTick);
  let html = "";
  const seen = new Set<number>();
  for (const o of mine) {
    seen.add(o.id);
    const dir = o.side === 0 ? 1 : -1;
    const avg = o.avgTick >= 0 ? toPrice(o.avgTick) : null;
    let pnl = avg !== null && mid !== null && o.filled > 0 ? (mid - avg) * o.filled * dir : null;
    if (pnl !== null && Math.abs(pnl) < 0.005) pnl = 0; // avoid a red "-0.00"
    const pcls = pnl === null ? "" : pnl > 0 ? "up" : pnl < 0 ? "down" : "";
    const pct = Math.round((o.filled / o.orig) * 100);
    const status = o.done ? "filled" : o.filled > 0 ? `${pct}% working` : "resting";
    const flash = mineFilled.has(o.id) && (mineFilled.get(o.id) as number) < o.filled ? " hit" : "";
    mineFilled.set(o.id, o.filled);
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
  if (html !== lastMineHTML) {
    // only touch the DOM when it changed (preserves selection, avoids reparse churn)
    $("molist").innerHTML = html;
    lastMineHTML = html;
  }
}

// ---- market state: regime badge, volatility meter, live price chart ---------
const HIST = 300; // samples held
const SAMPLE_MS = 120; // wall-clock spacing between chart samples (~36s window)
let lastSampleT = 0;
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
  const now = performance.now();
  if (mid !== null && now - lastSampleT >= SAMPLE_MS) {
    lastSampleT = now;
    hist.push(mid);
    if (hist.length > HIST) hist.shift();
  }

  const trend = snap.trend as number; // -1..1
  const vol01 = snap.vol01 as number; // 0..1
  const reg = $("regime");
  let label: string, cls: string;
  if (trend > 0.3) {
    label = "trending up";
    cls = "up";
  } else if (trend < -0.3) {
    label = "trending down";
    cls = "down";
  } else {
    label = "ranging";
    cls = "flat";
  }
  if (vol01 > 0.6) label += " · volatile";
  reg.textContent = label;
  reg.className = cls;

  $("volfill").style.transform = `scaleX(${Math.max(0.02, vol01)})`;
  $("volfill").className = vol01 > 0.6 ? "hot" : "";
  $("volmeter").setAttribute("aria-valuenow", String(Math.round(vol01 * 100)));

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

  const line = trend > 0.3 ? css("--bid") : trend < -0.3 ? css("--ask") : css("--amber");

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

// ---- input parsing (validated; never silently coerce to a crossing price) ---
const MAX_QTY = 1_000_000;
function readQty(): number | null {
  const raw = $<HTMLInputElement>("qty").value.trim();
  const n = parseInt(raw, 10);
  if (!raw || !Number.isFinite(n) || n <= 0) return null;
  return Math.min(n, MAX_QTY);
}
function readPrice(): number | null {
  const raw = $<HTMLInputElement>("price").value.trim();
  const n = Number(raw); // Number('') === 0 and Number('abc') === NaN, both rejected below
  if (!raw || !Number.isFinite(n) || n <= 0) return null;
  return n;
}
const ORDER_NAMES = ["limit", "market", "IOC", "FOK"];
// Only limit orders rest; market/IOC/FOK never do — their remainder is cancelled/killed.
function fillSummary(type: number, sideN: number, qty: number, at: string, res: any): string {
  const w = sideN ? "sell" : "buy";
  const filled = fmtN(res.filled);
  if (type === 0) return `limit ${w} ${fmtN(qty)}${at} → filled ${filled}, resting ${fmtN(res.resting)}`;
  const head = `${ORDER_NAMES[type]} ${w} ${fmtN(qty)}${at} → filled ${filled}`;
  if (res.resting > 0)
    return type === 3
      ? `${head} → killed, ${fmtN(res.resting)} unfilled (all-or-nothing)`
      : `${head}, ${fmtN(res.resting)} unfilled (cancelled)`;
  return head;
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
    // keyboard: focusing a level shows its cumulative depth; Enter/Space places the order
    host.addEventListener("focusin", (e) => {
      const r: Row | undefined = (e.target as any)?._row;
      if (!r || r.tick < 0) return;
      hoverSide = r.side;
      hoverIdx = r.idx;
      refreshCum();
    });
    host.addEventListener("focusout", clearCum);
    host.addEventListener("keydown", (e) => {
      if (e.key !== "Enter" && e.key !== " " && e.code !== "Space") return;
      const r: Row | undefined = (e.target as any)?._row;
      if (!r || r.tick < 0) return;
      e.preventDefault();
      placeAt(r);
    });
  }
}

let cumTextT = 0; // last write to the aria-live #cum readout (throttled to ~4 Hz)
let cumKey = ""; // side+idx of the last written readout — a row CHANGE bypasses the throttle

function refreshCum() {
  if (!hoverSide || hoverIdx < 0) return;
  for (const row of askRows) row.el.classList.toggle("cum", hoverSide === "ask" && row.tick >= 0 && row.idx <= hoverIdx);
  for (const row of bidRows) row.el.classList.toggle("cum", hoverSide === "bid" && row.tick >= 0 && row.idx <= hoverIdx);
  // #cum is aria-live: updating it every frame under a held cursor firehoses screen
  // readers. The highlight stays instant, and moving to a DIFFERENT row updates the
  // text immediately (a stale readout for the wrong row is a lie, not a throttle);
  // only same-row frame-by-frame churn settles at ~4 Hz.
  const now = performance.now();
  const key = `${hoverSide}:${hoverIdx}`;
  if (key === cumKey && now - cumTextT < 250) return;
  cumKey = key;
  cumTextT = now;
  const arr = hoverSide === "ask" ? lastAsks : lastBids;
  let cq = 0;
  let cn = 0;
  for (let k = 0; k <= hoverIdx && k < arr.length; k++) {
    const [t, q] = arr[k];
    cq += q;
    cn += (toPrice(t) || 0) * q;
  }
  $("cum").textContent = `Σ ${fmtN(cq)} · $${fmtK(cn)}`;
}

function clearCum() {
  hoverSide = null;
  hoverIdx = -1;
  cumTextT = 0; // a fresh hover updates the readout immediately
  cumKey = "";
  for (const row of askRows) row.el.classList.remove("cum");
  for (const row of bidRows) row.el.classList.remove("cum");
  $("cum").textContent = "";
}

function placeAt(r: Row) {
  const qty = readQty();
  if (qty === null) return showFill("enter a valid quantity");
  const s = r.side === "bid" ? 0 : 1; // the bid side buys, the ask side sells
  setSide(s);
  const res = sim.submit(0, s, r.tick, qty);
  if (res.rejected) return showFill("order rejected");
  showFill(fillSummary(0, s, qty, ` @ ${fmtP(r.tick)}`, res));
  if (!reduceMotion) {
    r.el.classList.remove("flash");
    void r.el.offsetWidth;
    r.el.classList.add("flash");
  }
}

// ---- form controls ----------------------------------------------------------
function setSide(v: number) {
  side = v;
  $("sideSeg")
    .querySelectorAll("button")
    .forEach((b) => {
      const on = +(b.dataset.v || 0) === v;
      b.classList.toggle("on", on);
      b.setAttribute("aria-pressed", on ? "true" : "false");
    });
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
    // Kill any running execution FIRST: its timer would keep slicing into the fresh
    // book, its TCA would be benchmarked against a pre-reset arrival mid, and a
    // surviving passive id would collide with the reset id counter.
    discardExec();
    sim.reset();
    prevLast = -1;
    hist.length = 0;
    mineFilled.clear();
    lastEvicted = 0;
    lastMineHTML = "";
    lastTapeHTML = "";
    hMm.length = hMom.length = hMr.length = 0; // reset also flattens the strategies
    stratSampleT = 0;
    document.querySelectorAll(".btn.sc").forEach((x) => x.classList.remove("on"));
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
    const qty = readQty();
    if (qty === null) return showFill("enter a valid quantity");

    let tick = START_TICK; // market ignores price; any valid tick is fine
    let at = "";
    let clamped = "";
    if (type !== 1) {
      const typed = readPrice();
      if (typed === null) return showFill("enter a valid price"); // never default to a crossing price
      tick = toTick(typed);
      const eff = toPrice(tick);
      at = ` @ ${fmtP(tick)}`;
      clamped = eff !== null && Math.abs(eff - typed) > 0.005 ? ` (clamped from ${typed.toFixed(2)})` : "";
    }
    const res = sim.submit(type, side, tick, qty);
    if (res.rejected) return showFill("order rejected");
    showFill(fillSummary(type, side, qty, at + clamped, res));
  };

  // Space toggles the sim — but only when focus is on a non-interactive element, so it
  // never steals Space from a focused button / input / the keyboard-operable ladder.
  window.addEventListener("keydown", (e) => {
    if (e.code !== "Space") return;
    const t = e.target as HTMLElement | null;
    if (t && t.closest('button, a, input, select, textarea, [role="button"]')) return;
    e.preventDefault();
    togglePlay();
  });
}

// ============================================================================
//  Applications built on the engine: tabs, an execution/TCA lab, a market maker
// ============================================================================
let lastMidPrice = 100;

function readIntInput(id: string): number | null {
  const raw = $<HTMLInputElement>(id).value.trim();
  // Number(), not parseInt: parseInt("1e9") === 1 silently. Clamp to the same cap the
  // engine enforces, so JS-side accounting (sent/filled/%) can't diverge from reality.
  const n = Number(raw);
  if (!raw || !Number.isFinite(n) || !Number.isInteger(n) || n <= 0) return null;
  return Math.min(n, MAX_QTY);
}
function money(v: number): string {
  return (v >= 0 ? "+$" : "−$") + Math.abs(v).toFixed(2);
}
function setMoney(el: HTMLElement, v: number) {
  el.textContent = money(v);
  el.className = "mono " + (v > 0.005 ? "pos" : v < -0.005 ? "neg" : "");
}

// ---- tabs -------------------------------------------------------------------
function wireTabs() {
  const tabs = Array.from(document.querySelectorAll<HTMLElement>(".tab"));
  const panels = Array.from(document.querySelectorAll<HTMLElement>(".tabpanel"));
  const select = (name: string) => {
    for (const t of tabs) {
      const on = t.dataset.tab === name;
      t.classList.toggle("on", on);
      t.setAttribute("aria-selected", on ? "true" : "false");
      t.tabIndex = on ? 0 : -1;
    }
    for (const p of panels) p.hidden = p.dataset.panel !== name;
    activePanel = name;
    if (name === "strats") sizeMm(); // canvas was zero-sized while its panel was hidden
  };
  tabs.forEach((t, i) => {
    t.addEventListener("click", () => select(t.dataset.tab!));
    t.addEventListener("keydown", (e) => {
      let next = -1;
      if (e.key === "ArrowRight") next = (i + 1) % tabs.length;
      else if (e.key === "ArrowLeft") next = (i + tabs.length - 1) % tabs.length;
      else if (e.key === "Home") next = 0;
      else if (e.key === "End") next = tabs.length - 1;
      else return;
      e.preventDefault();
      tabs[next].focus();
      select(tabs[next].dataset.tab!);
    });
  });
  select("trade"); // set the initial roving tabindex / selected state
}
let activePanel = "trade";

// ---- execution / TCA lab ----------------------------------------------------
type Exec = {
  algo: "market" | "twap" | "passive";
  side: number;
  qty: number;
  filled: number;
  notionalTick: number;
  arrivalMid: number;
  startT: number;
  passiveId?: number;
  horizonMs?: number;
  twapTimer?: number;
  slicesLeft?: number;
  sent?: number;
  slices?: number;
};
let exec: Exec | null = null;
let execSideV = 1; // default sell
const execHistory: string[] = [];

function setExecSide(v: number) {
  execSideV = v;
  $("execSide")
    .querySelectorAll("button")
    .forEach((b) => {
      const on = +(b.dataset.v || 0) === v;
      b.classList.toggle("on", on);
      b.setAttribute("aria-pressed", on ? "true" : "false");
    });
}
function setExecLive(msg: string) {
  $("execLive").textContent = msg;
}
function submitChild(side: number, qty: number) {
  if (qty <= 0 || !exec) return;
  const res = sim.submit(1, side, 0, qty); // market child
  const f = res.filled as number;
  if (f > 0 && res.avgTick >= 0) {
    exec.filled += f;
    exec.notionalTick += res.avgTick * f;
  }
}
function startExec() {
  if (exec) return;
  const qty = readIntInput("execQty");
  if (qty === null) return setExecLive("enter a valid size");
  const algo = $<HTMLSelectElement>("execAlgo").value as Exec["algo"];
  const side = execSideV;
  // Benchmark against a FRESH arrival mid — lastMidPrice is one frame stale, which is
  // the same order of magnitude as the passive edge this lab measures.
  const arrival = toPrice(sim.snapshot(1).midTick) ?? lastMidPrice;
  exec = { algo, side, qty, filled: 0, notionalTick: 0, arrivalMid: arrival, startT: performance.now() };
  $<HTMLButtonElement>("execRun").hidden = true;
  $<HTMLButtonElement>("execCancel").hidden = false;

  if (algo === "market") {
    submitChild(side, qty);
    finalizeExec();
  } else if (algo === "twap") {
    const slices = Math.min(Math.max(1, readIntInput("execSlices") ?? 20), 200);
    const horizonS = Math.min(Math.max(1, readIntInput("execHoriz") ?? 10), 300);
    exec.slices = slices;
    exec.slicesLeft = slices;
    exec.sent = 0;
    exec.horizonMs = horizonS * 1000;
    const tick = () => {
      if (!exec) return;
      // A frozen run can't stay "working" forever: if wall-clock blows past 4x the
      // horizon (paused / hidden / activity 0), record what actually happened and stop.
      if (performance.now() - exec.startT > exec.horizonMs! * 4 + 30000) {
        window.clearInterval(exec.twapTimer);
        return finalizeExec();
      }
      // Don't slice into a frozen, non-replenishing book. The sim only steps when
      // `running && speed > 0` AND the tab is visible (rAF suspends hidden tabs while
      // setInterval keeps firing) — guard on ALL THREE freeze paths, not a subset.
      if (!running || speed <= 0) return;
      if (document.visibilityState === "hidden") return;
      const done = exec.slices! - exec.slicesLeft!;
      // Even distribution with the remainder spread out — floor(qty/slices) sends
      // 0-share children whenever slices > qty and dumps everything on the last tick.
      const child = Math.round(((done + 1) * exec.qty) / exec.slices!) - exec.sent!;
      submitChild(exec.side, child);
      exec.sent! += child;
      exec.slicesLeft!--;
      if (exec.slicesLeft! <= 0 || exec.sent! >= exec.qty) {
        window.clearInterval(exec.twapTimer);
        finalizeExec();
      }
    };
    exec.twapTimer = window.setInterval(tick, Math.max(60, exec.horizonMs / slices));
  } else {
    // passive: rest a limit at our near touch (buy at best bid, sell at best ask)
    const horizonS = Math.min(Math.max(1, readIntInput("execHoriz") ?? 12), 300);
    exec.horizonMs = horizonS * 1000;
    // Rest at our near touch (buy at best bid, sell at best ask), read from a FRESH
    // top-of-book so a fast tick can't leave us posting a crossing (marketable) price.
    const s = sim.snapshot(1);
    const px = side === 0 ? s.bestBid : s.bestAsk;
    const tick = px >= 0 ? px : toTick(exec.arrivalMid);
    const res = sim.submit(0, side, tick, qty);
    if (res.rejected || res.id == null) {
      exec = null;
      $<HTMLButtonElement>("execRun").hidden = false;
      $<HTMLButtonElement>("execCancel").hidden = true;
      return setExecLive("order rejected");
    }
    exec.passiveId = res.id;
    sim.protectOrder(res.id, true); // the order cap must not evict/purge the lab's child
    // The resting order's cumulative fills are the single source of truth (read from the
    // snapshot each frame in updateExec), so we don't separately add the immediate fill.
  }
}
function updateExec(snap: any) {
  if (!exec) return;
  if (exec.algo === "passive") {
    if (exec.passiveId == null) return finalizeExec(); // should not happen; don't wedge the panel
    const m = (snap.mine as any[]).find((o) => o.id === exec!.passiveId);
    if (m) {
      exec.filled = Math.min(m.filled, exec.qty);
      exec.notionalTick = m.avgTick >= 0 ? m.avgTick * exec.filled : 0;
      if (m.done || exec.filled >= exec.qty) return finalizeExec();
    } else {
      return finalizeExec(); // the order vanished from tracking: finalize with what we know
    }
    if (running && performance.now() - exec.startT > (exec.horizonMs ?? 12000)) {
      sim.cancelOrder(exec.passiveId);
      return finalizeExec();
    }
  }
  const pct = Math.round((exec.filled / exec.qty) * 100);
  setExecLive(`working ${exec.algo} · filled ${fmtN(exec.filled)}/${fmtN(exec.qty)} (${pct}%)`);
}
function finalizeExec() {
  if (!exec) return;
  const e = exec;
  exec = null;
  if (e.twapTimer) window.clearInterval(e.twapTimer);
  if (e.passiveId != null) sim.protectOrder(e.passiveId, false); // let the husk be reaped
  $<HTMLButtonElement>("execRun").hidden = false;
  $<HTMLButtonElement>("execCancel").hidden = true;
  setExecLive("");

  const filled = Math.min(e.filled, e.qty);
  const fillPct = Math.min(100, Math.round((filled / e.qty) * 100));
  const avgPrice = filled > 0 ? (e.notionalTick / filled) * TICK_SIZE : null;
  const arrival = e.arrivalMid;
  // Read the end mid from a FRESH snapshot: a synchronous Market run finalizes before the
  // next render refreshes lastMidPrice, so the child's own impact would otherwise be missed.
  const endMid = toPrice(sim.snapshot(1).midTick) ?? lastMidPrice;
  // Implementation shortfall, desk convention: POSITIVE = cost. Filled shares pay
  // (avg − arrival) signed by side; the UNFILLED remainder is marked at the end mid,
  // so a passive order that misses its fill pays for the move away instead of the
  // adverse outcome silently vanishing from the table (selection bias).
  const sideSign = e.side === 0 ? 1 : -1; // buy: prices above arrival = cost
  const unfilled = e.qty - filled;
  const fillCost = avgPrice !== null ? sideSign * (avgPrice - arrival) * filled : 0;
  const oppCost = sideSign * (endMid - arrival) * unfilled;
  const isBps = arrival > 0 ? ((fillCost + oppCost) / (e.qty * arrival)) * 1e4 : null;
  // Side-adjusted mid move over the run (+ = the market moved against this order).
  // One run's move is mostly noise + your own impact — compare across repeated runs.
  const moveBps = arrival > 0 ? sideSign * ((endMid - arrival) / arrival) * 1e4 : 0;
  const durS = (performance.now() - e.startT) / 1000;
  const names: Record<string, string> = { market: "Market", twap: "TWAP", passive: "Passive" };
  const bp = (v: number | null) => (v === null ? "—" : (v >= 0 ? "+" : "−") + Math.abs(v).toFixed(1));
  const cls = (v: number | null) => (v === null ? "" : v >= 0.05 ? "neg" : v <= -0.05 ? "pos" : "");
  const row =
    `<tr>` +
    `<td class="algo">${names[e.algo]}</td>` +
    `<td class="dimc">${e.side ? "sell" : "buy"} ${fmtN(e.qty)}</td>` +
    `<td class="num">${avgPrice !== null ? avgPrice.toFixed(2) : "—"}</td>` +
    `<td class="num ${cls(isBps)}">${bp(isBps)}</td>` +
    `<td class="num">${fillPct}%</td>` +
    `<td class="num dimc">${bp(moveBps)}</td>` +
    `<td class="num dimc">${durS.toFixed(1)}s</td>` +
    `</tr>`;
  execHistory.unshift(row);
  if (execHistory.length > 8) execHistory.pop();
  $("execRows").innerHTML = execHistory.join("");
}
function cancelExec() {
  if (!exec) return;
  if (exec.passiveId != null) sim.cancelOrder(exec.passiveId);
  finalizeExec();
}
// Silently drop a running execution with NO TCA row — used by reset, where the book
// (and the arrival benchmark) the run started against no longer exists.
function discardExec() {
  if (!exec) return;
  if (exec.twapTimer) window.clearInterval(exec.twapTimer);
  if (exec.passiveId != null) {
    sim.cancelOrder(exec.passiveId);
    sim.protectOrder(exec.passiveId, false);
  }
  exec = null;
  $<HTMLButtonElement>("execRun").hidden = false;
  $<HTMLButtonElement>("execCancel").hidden = true;
  setExecLive("");
}
function wireExec() {
  setExecSide(execSideV);
  $("execSide")
    .querySelectorAll("button")
    .forEach((b) => b.addEventListener("click", () => setExecSide(+(b.dataset.v || 0))));
  const algoEl = $<HTMLSelectElement>("execAlgo");
  const syncAlgoFields = () => {
    const a = algoEl.value;
    document.querySelectorAll<HTMLElement>(".exec-twap").forEach((el) => (el.style.display = a === "twap" ? "" : "none"));
    document
      .querySelectorAll<HTMLElement>(".exec-horizon")
      .forEach((el) => (el.style.display = a === "twap" || a === "passive" ? "" : "none"));
  };
  algoEl.onchange = syncAlgoFields;
  syncAlgoFields();
  $<HTMLFormElement>("execForm").onsubmit = (e) => {
    e.preventDefault();
    startExec();
  };
  $("execCancel").onclick = cancelExec;
}

// ---- strategies arena (market maker · momentum · mean reversion) ------------
const MMHIST = 300;
const STRAT_COLORS: Record<string, string> = { mm: "#f2b54a", mom: "#5fb0e0", mr: "#c58ae0" };
const hMm: number[] = []; // each strategy's cumulative PnL ($) over wall-clock time
const hMom: number[] = [];
const hMr: number[] = [];
let stratSampleT = 0;
let mmSpark: HTMLCanvasElement;
let mmCtx: CanvasRenderingContext2D | null = null;

function setPos(el: HTMLElement, v: number) {
  el.textContent = fmtN(v);
  el.className = "mono " + (v > 0 ? "pos" : v < 0 ? "neg" : "");
}
function setSig(el: HTMLElement, s: number) {
  el.style.transform = `scaleX(${Math.min(1, Math.abs(s))})`;
  el.className = s > 0.02 ? "pos" : s < -0.02 ? "neg" : "";
}
function toggleStrat(btn: HTMLElement, on: boolean) {
  btn.setAttribute("aria-pressed", on ? "true" : "false");
  btn.textContent = on ? "on" : "off";
  btn.closest(".strat")?.classList.toggle("on", on);
}

function wireStrats() {
  let mmOn = false,
    momOn = false,
    mrOn = false;
  const mmBtn = $("mmToggle"),
    momBtn = $("momToggle"),
    mrBtn = $("mrToggle");
  mmBtn.onclick = () => { mmOn = !mmOn; sim.mmEnable(mmOn); toggleStrat(mmBtn, mmOn); };
  momBtn.onclick = () => { momOn = !momOn; sim.momEnable(momOn); toggleStrat(momBtn, momOn); };
  mrBtn.onclick = () => { mrOn = !mrOn; sim.mrEnable(mrOn); toggleStrat(mrBtn, mrOn); };

  // Apply the MM config only when ALL fields parse: cfg fires per keystroke, and a
  // transiently empty field used to slam in a hardcoded default (select-all + retype
  // in the inventory cap momentarily quoted against a limit the user never chose).
  const cfg = () => {
    const half = readIntInput("mmHalf");
    const size = readIntInput("mmSize");
    const inv = readIntInput("mmInv");
    if (half === null || size === null || inv === null) return; // mid-edit: keep last applied
    sim.mmConfig(half, size, inv);
  };
  ["mmHalf", "mmSize", "mmInv"].forEach((id) => $(id).addEventListener("input", cfg));
  cfg();

  // Mirrors the presets in Sim::scenario (sim.cpp) — keep in sync. Without this the
  // turbulence slider lies about engine state after a scenario click, and nudging it
  // snaps the market back to the stale displayed value.
  const SCENARIO_TURB: Record<number, number> = { 0: 8, 1: 20, 2: 26, 3: 72 };
  document.querySelectorAll<HTMLElement>(".btn.sc").forEach((b) =>
    b.addEventListener("click", () => {
      const sc = +(b.dataset.sc || 0);
      sim.scenario(sc);
      if (sc in SCENARIO_TURB) $<HTMLInputElement>("turb").value = String(SCENARIO_TURB[sc]);
      document.querySelectorAll(".btn.sc").forEach((x) => {
        x.classList.remove("on");
        x.setAttribute("aria-pressed", "false");
      });
      b.classList.add("on");
      b.setAttribute("aria-pressed", "true");
    }),
  );
  $("stratFlat").onclick = () => {
    sim.mmFlatten();
    sim.stratFlatten();
    hMm.length = hMom.length = hMr.length = 0;
  };

  mmSpark = $<HTMLCanvasElement>("mmChart");
  mmCtx = mmSpark.getContext("2d");
  sizeMm();
  window.addEventListener("resize", sizeMm);
}
function sizeMm() {
  if (!mmCtx) return;
  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  const r = mmSpark.getBoundingClientRect();
  mmSpark.width = Math.max(1, Math.round(r.width * dpr));
  mmSpark.height = Math.max(1, Math.round(r.height * dpr));
  mmCtx.setTransform(dpr, 0, 0, dpr, 0, 0);
}
function updateStrats(snap: any) {
  const mm = snap.mm,
    st = snap.strat;
  if (!mm || !st) return;
  const mmPnl = mm.pnlTick * TICK_SIZE;
  const momPnl = st.mom.pnlTick * TICK_SIZE;
  const mrPnl = st.mr.pnlTick * TICK_SIZE;
  setMoney($("mmPnl"), mmPnl);
  setPos($("mmInvv"), mm.inv);
  $("mmExtra").textContent =
    `spread ${money(mm.spreadTick * TICK_SIZE)} · adverse ${money(mm.adverseTick * TICK_SIZE)} · ` +
    `quotes ${mm.bidPx >= 0 ? fmtP(mm.bidPx) : "—"}/${mm.askPx >= 0 ? fmtP(mm.askPx) : "—"}`;
  setMoney($("momPnl"), momPnl);
  setPos($("momPos"), st.mom.inv);
  setSig($("momSig"), st.mom.signal);
  setMoney($("mrPnl"), mrPnl);
  setPos($("mrPos"), st.mr.inv);
  setSig($("mrSig"), st.mr.signal);

  // mark the leading enabled strategy (the "watch who wins" payoff)
  const runners: [string, number][] = [];
  if (mm.on) runners.push(["mm", mmPnl]);
  if (st.mom.on) runners.push(["mom", momPnl]);
  if (st.mr.on) runners.push(["mr", mrPnl]);
  const leader = runners.length > 1 ? runners.reduce((a, b) => (b[1] > a[1] ? b : a))[0] : null;
  for (const key of ["mm", "mom", "mr"])
    document.querySelector(`.strat[data-strat="${key}"]`)?.classList.toggle("leading", key === leader);

  const now = performance.now();
  if (now - stratSampleT >= SAMPLE_MS) {
    stratSampleT = now;
    hMm.push(mmPnl);
    hMom.push(momPnl);
    hMr.push(mrPnl);
    for (const h of [hMm, hMom, hMr]) if (h.length > MMHIST) h.shift();
  }
  if (activePanel === "strats") drawStrats(snap);
}
function drawStrats(snap: any) {
  if (!mmCtx) return;
  const c = mmCtx;
  const w = mmSpark.getBoundingClientRect().width;
  const h = mmSpark.getBoundingClientRect().height;
  c.clearRect(0, 0, w, h);
  const series: [number[], string, boolean][] = [
    [hMm, STRAT_COLORS.mm, snap.mm.on],
    [hMom, STRAT_COLORS.mom, snap.strat.mom.on],
    [hMr, STRAT_COLORS.mr, snap.strat.mr.on],
  ];
  // shared scale across the enabled series, always including 0
  let lo = 0,
    hi = 0;
  for (const [hist, , on] of series) if (on) for (const v of hist) { if (v < lo) lo = v; if (v > hi) hi = v; }
  const pad = Math.max((hi - lo) * 0.12, 0.5);
  lo -= pad;
  hi += pad;
  const span = hi - lo || 1;
  const x = (i: number) => (i / (MMHIST - 1)) * w;
  const y = (v: number) => h - 3 - ((v - lo) / span) * (h - 6);
  c.strokeStyle = css("--line");
  c.lineWidth = 1;
  c.beginPath();
  c.moveTo(0, y(0));
  c.lineTo(w, y(0));
  c.stroke();
  for (const [hist, color, on] of series) {
    if (!on || hist.length < 2) continue;
    const n = hist.length;
    const off = MMHIST - n;
    c.beginPath();
    for (let i = 0; i < n; i++) {
      const px = x(off + i);
      const py = y(hist[i]);
      i === 0 ? c.moveTo(px, py) : c.lineTo(px, py);
    }
    c.strokeStyle = color;
    c.lineWidth = 1.6;
    c.lineJoin = "round";
    c.stroke();
  }
}

function wireIntro() {
  const intro = $("intro");
  try {
    if (localStorage.getItem("ob_intro_dismissed") === "1") return;
  } catch {
    /* storage blocked (private mode) — just show it this session */
  }
  intro.hidden = false;
  $("introX").onclick = () => {
    intro.hidden = true;
    try {
      localStorage.setItem("ob_intro_dismissed", "1");
    } catch {
      /* ignore */
    }
  };
}

boot()
  .then(() => {
    wireTabs();
    wireExec();
    wireStrats();
    wireIntro();
  })
  .catch((err) => {
    console.error(err); // post-boot wiring errors used to vanish: #boot is gone by then
    const b = document.getElementById("boot");
    if (b) b.textContent = "failed to load the engine: " + err;
  });
