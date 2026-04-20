# IRONCLAD — Deterministic Rollback Netcode Toolkit

[![ci](https://github.com/Macho-tran/Ironclad/actions/workflows/ci.yml/badge.svg)](https://github.com/Macho-tran/Ironclad/actions/workflows/ci.yml)

C++20 rollback-netcode library + headless arena demo. Built for
gameplay/network engineers who need a *drop-in* deterministic
rollback stack: fixed-point sim, snapshot save/load, input
prediction & reconciliation, snapshot delta + adaptive arithmetic
coding, lag compensation, replay/record, desync detector, and a
deterministic in-process network simulator.

## Why this exists

Most rollback tutorials get *the prediction loop* right and stop
there. Production rollback needs four other things before it ships:

1. **Provable byte-determinism.** Two peers receiving identical
   inputs must compute identical state, *byte-for-byte*. We use a
   Q32.32 fixed-point math type, a deterministic SoA ECS, a seeded
   `xorshift64*` RNG that travels with snapshots, and a serializer
   that never touches struct padding or pointer values.
2. **A deterministic network simulator.** Latency, jitter, loss,
   and reorder are reproducible from a single seed; that's what
   makes both the unit tests and the soak test reliable.
3. **A desync detector that actually does something.** When peer
   hashes diverge, the library writes both sides' snapshot bytes
   and a sidecar metadata file you can binary-diff to find the
   exact component that drifted.
4. **A soak harness wired into CI.** A 60 s, 4 AI clients,
   150 ms RTT / 5 % loss / 30 ms jitter run is part of every
   build and fails on any desync.

## Features

| Subsystem               | Header / Source                                | Status |
| ----------------------- | ---------------------------------------------- | ------ |
| Q32.32 fixed-point      | `include/ironclad/fixed.hpp`                   | ✅     |
| 2D vec of fixed         | `include/ironclad/vec2.hpp`                    | ✅     |
| Deterministic RNG       | `include/ironclad/rng.hpp`                     | ✅     |
| xxHash3 wrapper         | `include/ironclad/hash.hpp` + `src/hash.cpp`   | ✅     |
| Tiny SoA ECS            | `include/ironclad/ecs.hpp`                     | ✅     |
| Snapshot ring + inputs  | `include/ironclad/snapshot.hpp`                | ✅     |
| XOR + RLE delta         | `include/ironclad/delta.hpp` + `src/delta.cpp` | ✅     |
| Adaptive arithmetic coder| `include/ironclad/range_coder.hpp` + `.cpp`   | ✅     |
| Loopback transport      | `include/ironclad/loopback_transport.hpp`      | ✅     |
| Network simulator       | `include/ironclad/netsim.hpp`                  | ✅     |
| Rollback Session        | `include/ironclad/session.hpp` + `src/session.cpp` | ✅ |
| Lag compensation        | `include/ironclad/lag_comp.hpp`                | ✅     |
| Recorder / replay parser| `include/ironclad/recorder.hpp` + `.cpp`       | ✅     |
| Desync dumper           | `include/ironclad/desync.hpp` + `.cpp`         | ✅     |
| Headless `arena_demo`   | `apps/arena_demo/`                             | ✅     |
| Soak smoke test         | `tests/test_soak_smoke.cpp` (CTest label `soak`) | ✅   |
| SDL2 visual front-end   | `apps/arena_view/`                             | ✅     |
| **Replay Studio**       | `include/ironclad/replay.hpp` + `apps/arena_view/studio.cpp` | ✅ |
| **Real UDP transport**  | `include/ironclad/udp_transport.hpp` + `arena_demo --net` | ✅ |
| **Lag-comp visualization** | `.iclr` v3 + `Studio::render_arena` ghost hitboxes | ✅ |
| **Pred-vs-auth ribbon** | `.iclr` v4 + `Studio::render_input_lanes` 2-row ribbon | ✅ |
| **ImGui control panel** | gated `IRONCLAD_USE_IMGUI=ON`, FetchContent v1.90 | ✅ |
| **CI artifact pipeline** | `.github/workflows/ci.yml` artifacts job          | ✅ |
| **Cross-platform replay determinism** | CI step in every matrix cell        | ✅ |
| **MSVC port** (`_mul128`/`_div128`) | `fixed.hpp`; Windows in CI matrix     | ✅ |
| **libFuzzer harnesses** | `tests/fuzz_*.cpp` (clang only, opt-in)        | ✅ |
| **Property tests**      | `tests/test_property.cpp`                      | ✅ |
| **Stress tests**        | `tests/test_stress.cpp` (label `stress`)       | ✅ |
| **HARDENING.md**        | `docs/HARDENING.md`                            | ✅ |
| Unity / Unreal bindings | —                                              | ⏸     |

## Quickstart

```bash
git clone https://github.com/Macho-tran/Ironclad
cd Ironclad
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure          # unit tests
ctest --test-dir build --output-on-failure -L soak  # 60s soak
```

Run the demo:

```bash
./build/apps/arena_demo/arena_demo \
    --frames 3600 --players 4 \
    --rtt-ms 150 --jitter-ms 30 --loss-pct 5 --reorder-pct 2 \
    --record session.iclr
```

CLI flags: `--frames`, `--players`, `--rtt-ms`, `--jitter-ms`,
`--loss-pct`, `--reorder-pct`, `--seed`, `--record PATH`,
`--quiet`.

## Replay Studio

A time-travel debugger for the `.iclr` files produced by
`arena_demo --record`. It re-simulates deterministically from the
recorded canonical inputs, so you can scrub to any frame, see the
arena state, inspect entity values, and step through rollback /
desync events visually.

```bash
# generate a 30 s sample recording
./scripts/make-sample.sh

# text summary (CI-friendly)
./build/apps/arena_demo/arena_demo --replay-info samples/sample.iclr

# interactive studio (needs SDL2)
cmake -S . -B build -DIRONCLAD_BUILD_SDL_DEMO=ON && cmake --build build --parallel
./build/apps/arena_view/arena_view --replay samples/sample.iclr

# headless single-frame screenshot (no display required)
SDL_VIDEODRIVER=offscreen \
  ./build/apps/arena_view/arena_view \
    --replay samples/sample.iclr --screenshot studio.bmp --frame 900
```

See [`docs/REPLAY_STUDIO.md`](docs/REPLAY_STUDIO.md) for the
architecture, file format (`.iclr` v1 / v2 / v3 / v4), keyboard map,
and a 2-minute demo script.

## Real UDP between two processes

```bash
# Terminal A (host, listens on :7777 as player 0)
./build/apps/arena_demo/arena_demo --net listen \
    --port 7777 --players 2 --my-id 0 --frames 1800

# Terminal B (client, connects to host as player 1)
./build/apps/arena_demo/arena_demo --net connect \
    --remote 127.0.0.1:7777 --port 7778 \
    --players 2 --my-id 1 --frames 1800 --record /tmp/match.iclr
```

Both processes converge on byte-identical state; either side can
record an `.iclr` file from the live UDP traffic.

## Hardening

See [`docs/HARDENING.md`](docs/HARDENING.md) for the test surface
(unit + property + soak + stress + sanitizers + libFuzzer +
cross-platform replay), the actual bugs found by each layer, and
how to reproduce them.

## Measured KPIs

Numbers from a single deterministic run on Linux GCC 13.3, x86_64,
seed `0xC0FFEEBEEFD00D`.

| Scenario                                       | Avg rollback / tick | Bandwidth / client | Desyncs |
| ---------------------------------------------- | ------------------- | ------------------ | ------- |
| 2 players, 60 ms RTT, 1 % loss, 5 ms jitter   | **1.52 frames**     | **4.86 KB/s**      | **0**   |
| 4 players, 150 ms RTT, 5 % loss, 30 ms jitter | 5.21 frames         | **14.57 KB/s**     | **0**   |

Both well under the 150 KB/s bandwidth KPI; the 2-player case beats
the brief's "≤ 2 frames average correction at 100–150 ms RTT" target.
The 4-player case is realistic-load — not part of the brief KPI but
exercised in CI as the soak smoke test. To reproduce, see
[`docs/KPIS.md`](docs/KPIS.md).

## Repository layout

```
include/ironclad/    public headers (header-only friendly)
src/                 .cpp implementations
third_party/         vendored xxhash + doctest (single-file each)
apps/arena_demo/     headless 4-AI demo + soak harness + .iclr recorder
apps/arena_view/     SDL2 live frontend + Replay Studio (--replay PATH)
samples/             tiny pre-recorded .iclr (made by scripts/make-sample.sh)
scripts/             helpers (make-sample.sh)
tests/               doctest unit suites (one binary per subsystem)
.github/workflows/   CI matrix (Linux GCC/Clang verified locally;
                     macOS Clang job included best-effort)
docs/                ARCHITECTURE, DETERMINISM, KPIS, REPLAY_STUDIO
```

## Determinism contract

See [`docs/DETERMINISM.md`](docs/DETERMINISM.md) for the full list
of rules and where each is enforced. The short version:

* No `float` / `double` in the simulation — all sim arithmetic uses
  `ironclad::Fixed` (Q32.32, 64-bit storage).
* No `std::random_device`, no `std::mt19937` distributions —
  `ironclad::Rng` is hand-written `xorshift64*` with explicit state.
* Snapshots serialize via byte-deterministic packers, never via
  raw struct memcpy.
* Component column iteration is by ascending entity id, not
  insertion order.
* The network simulator owns its own RNG; changing latency/loss
  parameters never affects sim hashes.

## License

MIT. Vendored `xxhash` is BSD-2-Clause; vendored `doctest` is MIT.
