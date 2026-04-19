# Determinism

Determinism is the load-bearing claim of this library. Every rule
below is enforced by code, by tests, or by both.

## Rule 1 — No `float`/`double` in the simulation

* All sim arithmetic uses `ironclad::Fixed` (Q32.32 stored in
  `int64_t`).
* `Fixed::to_double()` is provided only for the *view* layer and
  for tests. It is not called from any code in `src/` other than
  through tests.
* Multiplication and division use `__int128` to avoid overflow
  that would otherwise be platform-dependent.
* Division by zero is **defined** to return ±MAX (or 0 for 0/0),
  not UB.
* `INT64_MIN` negation is **defined** to saturate to `INT64_MAX`.
* `Fixed::sqrt` of a negative value returns 0 (defined).

Tested in `tests/test_fixed.cpp`.

## Rule 2 — RNG state is part of the snapshot

* `ironclad::Rng` is `xorshift64*` seeded through `SplitMix64`.
* Its only state is a `uint64_t`, included in the snapshot
  produced by `Session::save_snapshot`.
* `std::random_device`, `std::mt19937`, and `<random>` distributions
  are **forbidden** in the sim path.

Tested in `tests/test_rng.cpp` (frozen byte-sequence fixture).

## Rule 3 — ECS iteration order is by entity id

* Components live in dense column arrays indexed by entity id.
* `World::each<T>` iterates `0..capacity_-1`, skipping inactive or
  missing slots — strictly ascending order, regardless of
  insertion or removal history.
* Why this matters: EnTT's sparse-set iteration depends on
  insertion history; that's a footgun for byte-deterministic
  snapshots, so we hand-rolled the ECS.

## Rule 4 — Serialization is byte-deterministic

* The snapshot writer in `World::serialize` only emits explicitly
  packed bytes via `ByteWriter` (little-endian for every multi-byte
  integer).
* Component types provide ADL-found `pack`/`unpack` functions that
  define their byte layout. We never `memcpy` raw struct memory
  (which would expose padding bytes and alignment artifacts).
* The hash is computed over the serialized bytes, never raw
  in-memory state.

Tested in `tests/test_ecs.cpp` (round-trip + cross-instance hash
equality).

## Rule 5 — Two RNG streams: sim and netsim are isolated

* `Session` owns one `Rng` for the sim.
* `LoopbackHub` owns its own `Rng` for the network simulator.
* The two are seeded with different keys derived from the
  `SessionConfig::seed` so they cannot accidentally share state.
* Consequence: changing `--rtt-ms`, `--loss-pct`, `--jitter-ms`
  changes the *delivery schedule* but does not change any sim
  hash for a given seed. This is what makes the soak test
  reliable.

## Rule 6 — Local input delay is part of the deterministic record

* `SessionConfig::local_input_delay` (default 2 ticks) shifts every
  local input forward in the input ring by N ticks.
* Replays preserve the same shift because both inputs and snapshots
  are identical.

## Rule 7 — Strict order of operations per tick

`Session::tick`:

1. `transport_->poll()`
2. record local input (at `frame + delay`)
3. drain transport, possibly setting `rollback_to`
4. perform rollback (`load_snapshot` + re-step + re-snapshot)
5. step the *current* frame
6. save + hash the new snapshot
7. broadcast `InputPacket`

Every system in the demo step function (`apps/arena_demo/arena.cpp:
step_arena`) runs in a fixed order: player movement → projectile
update → projectile lifetime → RNG advance.

## Rule 8 — `__int128` is the only platform-specific dep

* Available on GCC and Clang via `__int128`. We assert at compile
  time and gracefully `#error` on unsupported toolchains.
* MSVC support requires the `_mul128` / `_div128` intrinsics; this
  is documented as a follow-up in `include/ironclad/fixed.hpp`.

## Verification

| Test                         | What it verifies                          |
| ---------------------------- | ----------------------------------------- |
| `test_fixed`                 | Fixed-point arithmetic is exact / within  |
|                              | 1 ulp; division-by-zero is defined; sqrt  |
|                              | accuracy on perfect squares + irrationals |
| `test_rng`                   | Frozen byte-sequence fixture              |
| `test_ecs`                   | save → load → save round-trip             |
| `test_snapshot`              | Ring buffer wrap + frame-tagged retrieval |
| `test_session_rollback`      | Two peers byte-identical for 200 frames   |
|                              | with zero latency; identical after drain  |
|                              | with 150 ms RTT + 5 % loss + 30 ms jitter |
| `test_netsim`                | Two runs with same seed → identical       |
|                              | delivery schedule                         |
| `test_soak_smoke`            | 60 s, 4 AI clients, aggressive netsim →   |
|                              | 0 desyncs                                 |
