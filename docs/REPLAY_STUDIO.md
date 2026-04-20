# Replay Studio

A time-travel debugger and replay viewer for ironclad `.iclr`
recordings. Makes the rollback netcode's behaviour inspectable
frame-by-frame instead of hidden behind an FPS counter.

## What it shows

```
+----------------------------------------------------------------+
|  IRONCLAD REPLAY STUDIO          FRAME 900/1800   HASH 3412... |
|                                  ROLLBACK 6 F                  |
|  +------------------------+      +---------------------------+ |
|  |  arena render at the   |      | ENTITY INSPECTOR          | |
|  |  scrubbed frame —       |      |   P0 HP 100 SCORE 0       | |
|  |  player circles +       |      |     POS -1.26, -4.28      | |
|  |  projectiles            |      |     VEL  0.13,  0.13      | |
|  |  (driven by Replayer)   |      |   P1 HP 100 SCORE 0  ...  | |
|  +------------------------+      | PROJECTILES 4             | |
|                                   |                           | |
|                                   | REPLAY SUMMARY            | |
|                                   |   FRAMES 1800             | |
|                                   |   ROLLBACKS 1643          | |
|                                   |   MAX 9F  AVG 5.67        | |
|                                   |   DESYNCS 0               | |
|                                   +---------------------------+ |
|                                                                |
|  ROLLBACK SPIKES   |▆__▂_▇▃_____▆___▂_____|  <- click to scrub |
|  PLAYER INPUT LANES                                            |
|    P0 [colour cells per frame, button accents, mismatch ticks] |
|    P1 ...                                                      |
|  SPACE PLAY/PAUSE  < > STEP  SHIFT+ JUMP  [ ] PREV/NEXT EVENT  |
+----------------------------------------------------------------+
```

* **Arena render** — the world at the scrubbed frame, rebuilt
  deterministically from the recording (no live network).
* **Top bar** — frame counter, current state hash, and a
  contextual amber "ROLLBACK N F" / red "DESYNC" banner if the
  current frame had an event.
* **Entity inspector** — every player's HP, score, position,
  velocity, and dash/attack cooldowns at the selected frame, plus
  the live projectile count and a recording-wide summary panel.
* **Rollback spike timeline** — full-width scrubber. Each pixel
  corresponds to a frame (or a small range for long replays);
  amber bar height = rollback frames on that tick; red blocks =
  desync events. Click anywhere to seek.
* **Player input lanes** — one lane per player, ~120 frames
  visible centred on the playhead. Cell tint shows direction
  pressed; pink top-bar = attack; amber bottom-bar = dash; tiny
  red corner = "this peer's prediction for this player diverged
  from the canonical input" (`pred_diff` bit set).
* **Keyboard** — space play/pause; ←/→ frame step; Shift+←/→
  jump 60; `[`/`]` previous/next rollback or desync event;
  Home/End to start/end; Esc to quit.

## How to drive it

```bash
# Build (needs SDL2 dev headers)
cmake -S . -B build -DIRONCLAD_BUILD_SDL_DEMO=ON
cmake --build build --parallel

# Record a sample (writes samples/sample.iclr by default)
./scripts/make-sample.sh

# Inspect via the SDL studio (interactive)
./build/apps/arena_view/arena_view --replay samples/sample.iclr

# Or render a single frame to BMP without a display
SDL_VIDEODRIVER=offscreen \
  ./build/apps/arena_view/arena_view \
    --replay samples/sample.iclr \
    --screenshot studio.bmp \
    --frame 900

# Headless text summary (CI-friendly)
./build/apps/arena_demo/arena_demo --replay-info samples/sample.iclr
```

Sample console output from `--replay-info`:

```
== ironclad replay ==
  file              : samples/sample.iclr
  format version    : v2
  tick rate         : 60 Hz
  players           : 4
  seed              : 0x00c0ffeebeefd00d
  recorded frames   : 1800
  duration          : 30.00 s @ 60 Hz
  rollback events   : 1643 (max 9 frames, total 10208, avg 5.67)
  desync events     : 0
  rollback histogram:
       =0f : 157
       =1f : 0
       <=2f : 0
       <=4f : 2
       <=8f : 1640
      <=16f : 1
      <=32f : 0
       >32f : 0
  hash chain        : OK (re-sim matches every recorded hash)
```

## Architecture

```
+--------------------+    +-----------------+
| .iclr file (v2)    | -> | parse_replay()  |  src/recorder.cpp
+--------------------+    +--------+--------+
                                   |
                                   v
                          +----------------+
                          |  ReplayModel   |  src/replay.cpp
                          |  - records[]   |
                          |  - events[]    |
                          |  - stats       |
                          +-------+--------+
                                  |
                       +----------+-----------+
                       |                      |
                       v                      v
              +----------------+      +--------------+
              |   Replayer     |      |    Studio    |
              |  (deterministic |      |  SDL2 UI:    |
              |   re-simulator) |      | timeline,    |
              +----------------+       | lanes,       |
                       |                | inspector   |
                       v                +-------+----+
                +-----------+                   |
                |  World    |  <----------------+
                |  at frame |  world_at(playhead)
                +-----------+
```

* **Library code (`include/ironclad/replay.hpp`, `src/replay.cpp`)**
  has zero rendering / SDL deps. It can be linked into a
  command-line tool, an editor extension, or another renderer.
* **App code (`apps/arena_view/studio.{hpp,cpp}`)** holds the SDL
  rendering and event loop. It uses only the public Replay
  Studio API plus the existing `arena_demo` step / init
  functions. The Studio doesn't know what game is running; it
  just asks `Replayer::world_at(frame)` for the entity store.

## Replay format change (v1 → v2)

The Replay Studio adds three trailing bytes to every per-frame
record so we can surface rollback / desync / prediction-mismatch
events without re-simulating each scrub.

```
header:
  magic     : "IRCL_REPLAY2"  (12 bytes incl. NUL)  — was "...REPLAY1" in v1
  version   : u16             (== 2)
  tick_hz   : u16
  num_players : u8
  reserved  : u8
  seed      : u64
  world_cap : u32
  init_size : u32
  init_bytes: u8[init_size]

per record (v2):
  frame      : u32
  inputs     : PlayerInput[num_players]   (4 bytes each)
  hash       : u64
  rollback   : u8                          # NEW — frames rolled back
  flags      : u8                          # NEW — bit 0 = desync
  pred_diff  : u8                          # NEW — bitmask: bit P set
                                           #   if at least one peer's
                                           #   prediction for player P
                                           #   differed from the
                                           #   canonical input

trailer    : "ENDR" + u32 frame_count + u64 final_hash
```

`parse_replay` auto-detects the magic and loads v1 files with
`rollback = flags = pred_diff = 0` for every record, so old
recordings still open in the Studio (just with the event lanes
empty).

## Determinism

The Studio re-simulates from the recorded canonical inputs using
the same step + init functions used at record time. Because:

1. Initial snapshot bytes are stored in the header.
2. Inputs are recorded as the canonical AI inputs (offline,
   post-rollback) — see `apps/arena_demo/arena.cpp`'s offline
   re-recording pass.
3. The seed travels with the recording.

…we can prove the recording is intact by re-simulating it and
comparing every recorded hash against the re-sim hash. That's
what `--replay-info` does and what `Replayer::validate_hash_chain`
exposes via the library API.

## Demo script (~2 minutes)

1. `./scripts/make-sample.sh` — produces `samples/sample.iclr`
   from a 30 s / 4-player / 150 ms-RTT / 5 %-loss session.
2. `./build/apps/arena_demo/arena_demo --replay-info samples/sample.iclr`
   — show the text summary: frame count, rollback histogram,
   `hash chain : OK`. Talking point: the chain validating end-to-end
   is the hard proof of determinism.
3. `./build/apps/arena_view/arena_view --replay samples/sample.iclr`
   — open the studio. Press `]` to jump to the first rollback
   event. Watch the amber spike on the timeline; the entity
   inspector shows the world at that exact frame; arrow keys
   scrub by single frames so you can step through what happened.
4. Click the timeline at random spots to scrub anywhere in the
   30 s recording — the arena re-simulates instantly thanks to
   the checkpoint cache.

## Limitations / follow-ups

The Replay Studio is meant to be a **focused** debugger, not a
general-purpose ECS reflection inspector. The following are
explicitly deferred:

* Per-frame *predicted-input vs authoritative-input* visualization
  is a single bitmask today (`pred_diff`); a richer per-peer view
  would need recording each peer's prediction for each remote
  player on each tick (~`num_players² × frames` bytes — modest
  but not free).
* Lag-comp shot rewind visualization. We have the data path
  (`include/ironclad/lag_comp.hpp`) but no recording hook yet.
* Saving studio screenshots as a sequence for animated GIFs
  (today the CLI takes one BMP per invocation).
* ImGui-based control panel — deferred deliberately to avoid
  adding a heavy dep; the line-segment renderer in
  `apps/arena_view/render.cpp` is sufficient and dependency-free.
