#!/usr/bin/env bash
# Compile the C++ matching engine (web/sim.cpp -> the real ob::OrderBook) to
# WebAssembly via Emscripten + Embind. Emits web/app/public/ob.{js,wasm}, which the
# Vite app loads. The generated files are committed so the site builds/deploys
# without an Emscripten toolchain; re-run this only when the engine or bindings change.
#
# Requires emscripten on PATH (e.g. `source ~/emsdk/emsdk_env.sh`).
set -euo pipefail
cd "$(dirname "$0")/.."

emcc web/sim.cpp -Iinclude -std=c++20 -O3 --bind \
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1 \
  -o web/app/public/ob.js

echo "wrote web/app/public/ob.{js,wasm}"
