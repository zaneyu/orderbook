# orderbook — live web visualizer

The **real C++ matching engine, compiled to WebAssembly**, matching a simulated market
in the browser: a live L2 depth ladder, a price chart, time-and-sales tape, and manual
order entry (limit / market / IOC / FOK). `sim.cpp` binds the same `ob::OrderBook` the
tests and benchmarks use via Embind; there is no JavaScript reimplementation.

The market is a small but genuine price process, not a flat random walk: **stochastic
volatility** (turbulence clusters), a persistent **momentum/trend** regime, fat-tailed
**news jumps**, order flow that leans with the trend, and spreads that widen in
turbulence. You can fire a bullish/bearish **news shock**, turn the **turbulence** dial,
and your own aggressive orders exert **market impact** (fills push the price). Submit a
limit order and the **working-orders** panel tracks it over its life: fills as market
flow reaches it, average price, and live mark-to-market P&L.

## Run it

```bash
cd web/app
npm install
npm run dev          # or: npm run build && npm run preview
```

Open the printed URL. The engine steps the simulated flow each frame; Pause/Play, drag
activity, fire news shocks, turn up turbulence, click a ladder level to rest an order, or
submit from the form and watch your working orders fill.

Two applications are built on the same engine, in tabs:

- **Execution & TCA** — work a parent order through the live book via *Market now*,
  *TWAP* (sliced over a horizon), or *Passive limit*, and measure the realized average
  price vs the arrival mid (slippage, in bps), fill rate, and the mid move over the
  window. Run the same order a few times to compare strategies — the best-execution
  trade-off, measured the way a desk measures it.
- **Strategies** — three autonomous bots that trade the same live book, with live PnL:
  a **market maker** (inventory-skewed Avellaneda–Stoikov quotes, PnL split into spread
  capture vs adverse selection), a **momentum** bot (buys strength, sells weakness), and
  a **mean-reversion** bot (fades moves back to the mean). Market-scenario presets (calm /
  trending / choppy / volatile / flash crash) let you watch which style wins by regime —
  momentum in trends, mean reversion in choppy markets. The bots trade only on the
  observable mid, and the price process itself becomes mean-reverting in the choppy
  regime, so the edge is real, not an oracle.

## Rebuild the WASM (only if the engine or bindings change)

```bash
source ~/emsdk/emsdk_env.sh      # Emscripten on PATH
web/build-wasm.sh                # -> web/app/public/ob.{js,wasm}
```

The generated `ob.js` / `ob.wasm` are committed so the site deploys as a static bundle
(`npm run build` -> `web/app/dist/`) without an Emscripten toolchain on the host.

## Layout

```
web/sim.cpp          Embind bindings: ob::OrderBook + a small market simulator
web/build-wasm.sh    emcc invocation
web/app/             Vite + TypeScript front-end (no framework)
  src/main.ts        engine load, render loop, controls, order entry
  src/style.css      warm-ink trading terminal (dark), tabular figures
  public/ob.{js,wasm}  the engine, as WebAssembly (generated)
```
