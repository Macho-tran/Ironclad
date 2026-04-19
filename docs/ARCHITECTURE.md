# Architecture

```
+-----------------------------------------------------------+
|                      arena_demo (apps/)                  |
|   - 4 AI sessions sharing a LoopbackHub                  |
|   - per-second diegetic stats overlay                    |
|   - .iclr replay recording                               |
+----------------------------+-----------------------------+
                             |
                             v
+-----------------------------------------------------------+
|                  ironclad library (include/, src/)       |
|                                                           |
|   +-----------------+   +-----------------+               |
|   | Session         |-->| World (ECS)     |               |
|   |  - input ring   |   |  - SoA columns  |               |
|   |  - snapshot ring|   |  - serialize    |               |
|   |  - rollback     |   +-----------------+               |
|   |  - reconcile    |                                     |
|   |  - desync detect|   +-----------------+               |
|   |  - input delay  |   | Rng             |               |
|   +--------+--------+   |  - xorshift64*  |               |
|            |            |  - state in snap|               |
|            |            +-----------------+               |
|            v                                              |
|   +-----------------+   +-----------------+               |
|   | ITransport      |<->| LoopbackHub     |               |
|   +-----------------+   |  - per-link sim |               |
|                         +-----------------+               |
|                                                           |
|   +-----------------+   +-----------------+               |
|   | Delta + RangeCoder ||  Recorder       |               |
|   |  - XOR + RLE    |   |  - .iclr writer |               |
|   |  - bit-arith    |   |  - parser       |               |
|   +-----------------+   +-----------------+               |
+-----------------------------------------------------------+
```

## Sim / View / Transport split

* **Simulation** lives in `ironclad`: pure, deterministic, with no
  side-effects beyond mutating its own `World`. Inputs in, state
  hash out.
* **View** lives in the *application* (e.g. `arena_demo` for
  headless, `arena_view` for SDL2): reads `Session::world()` each
  frame and renders. The view never writes to the simulation.
* **Transport** is an abstraction inside `ironclad`. The default
  `LoopbackHub` runs everything in-process so tests + soak don't
  touch the network. A real UDP transport conforms to the same
  `ITransport` interface (interface stable; impl is an explicit
  follow-up — see README for status).

## Per-tick flow

```
loop:
    transport.poll()                           # advance netsim time
    for player in peers:
        session.tick(ai_input(frame, player))
            |
            +-- 1. record local input at frame + delay
            +-- 2. drain transport
            |     - apply remote inputs to ring
            |     - if any remote input differs from prediction,
            |       record min(rollback_to)
            |     - check peer ack hashes vs local; flag desync
            +-- 3. if rollback_to set:
            |     - load_snapshot(rollback_to)
            |     - re-step frames [rollback_to .. current)
            |     - re-snapshot each
            +-- 4. assemble inputs (real or predicted) for current
            +-- 5. run user step()
            +-- 6. snapshot + hash
            +-- 7. broadcast input packet to peers
    hub.advance_tick()
```

## Snapshot byte layout

```
[u32 magic 'IRCL']
[u16 version]
[u32 capacity]
[u32 next_id]
[u32 alive_count]
[u32 alive_bitmap_size]
[alive_bitmap_size bytes]
[u32 num_columns]
for each component column:
    [has_bitmap_size bytes]
    [packed component bytes for each set bit]
```

The Session appends an extra `u64 rng_state` after this and hashes
the whole thing with xxHash3 — that hash *is* the per-frame
authoritative checksum.

## Packet schema (`InputPacket`)

```
[u8  version = 1]
[u8  sender]
[u32 frame]            # newest frame in this packet
[u8  count]            # 1..16
[u32 ack_frame]        # most recent peer-state-hash we've verified
[u64 ack_hash]
PlayerInput * count    # 4 bytes each
```

Total per packet for the demo: 19 + 4*N bytes (e.g. ~83 bytes for
N=16). At 60 Hz that's ~5 KB/s per peer per direction even before
delta-compressing snapshots — comfortably under the 150 KB/s KPI.
