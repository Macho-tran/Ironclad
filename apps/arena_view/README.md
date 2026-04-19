# `arena_view` — SDL2 visual front-end (stretch)

`arena_view` is the visual reference client for the headless
`arena_demo`. Player 0 is keyboard-driven; players 1-3 use the same
deterministic AI as the headless soak harness.

This binary is **not** required for any KPI or test — those are
covered by the headless `arena_demo` and `test_soak_smoke`. It
exists to make the rollback effect visible.

## Build

This target is **off by default**. Enable it explicitly:

```bash
cmake -S . -B build -DIRONCLAD_BUILD_SDL_DEMO=ON
cmake --build build --target arena_view
./build/apps/arena_view/arena_view --rtt-ms 150 --loss-pct 5
```

If `find_package(SDL2)` fails, the target is silently skipped at
configure time so a missing SDL2 dev install doesn't break a CI
build.

## Controls

| Key                       | Action          |
| ------------------------- | --------------- |
| Arrow keys / WASD         | Move player 0   |
| Space                     | Attack          |
| Left Shift                | Dash            |
| Esc                       | Quit            |

## Visual style ("arcade-tech")

* High-contrast neon strokes on a black background.
* Players are glowing circles with a per-player neon palette.
* Projectiles are small white pellets.
* When player 0's session reports a non-zero rollback distance for
  the most recent reconciliation, white "glass shard" particles
  spawn at the centre of the arena. Particle count scales with
  rollback magnitude. The particles are pure view-layer state and
  never affect the simulation.

## Diegetic stats overlay

Drawn as line segments — no font dependency. Shows:

* `FRAME N`            — current sim frame.
* `RTT N ms`           — round-trip time of the netsim.
* `ROLLBACK N F`       — frames rolled back on the most recent
                         reconciliation.
* `DESYNC YES/NO`      — sticky indicator from the desync detector.

## Limitations / status

This client is a *skeleton*. Items consciously deferred:

* No menus / lobby flow — it boots straight into a 4-player
  match.
* No spectator mode or replay scrubber. The `Recorder` API is
  ready (the headless demo writes `.iclr` files); plugging the
  scrubber into ImGui is a follow-up.
* No audio.
* Keyboard-only input.
