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
speed, fire news shocks, turn up turbulence, click a ladder level to rest an order, or
submit from the form and watch your working orders fill.

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
