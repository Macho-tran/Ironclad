# Hardening — what we test, how, and the bugs we found

This document records the methodology + findings of Phase 8 of the
multi-feature hardening pass.

## Test surface

| Suite | Where | Default? | Run via |
|---|---|---|---|
| Per-subsystem unit tests | `tests/test_*.cpp` | yes | `ctest -LE soak` |
| Replay Studio model + Replayer | `tests/test_replay_model.cpp` | yes | `ctest -R replay_model` |
| Property tests (random round-trips) | `tests/test_property.cpp` | yes | `ctest -R property` |
| Soak smoke (60 s @ 4p @ 150 ms / 5 % loss) | `tests/test_soak_smoke.cpp` | yes (label `soak`) | `ctest -L soak` |
| UDP transport (2-thread loopback) | `tests/test_udp_transport.cpp` | yes (label `network`) | `ctest -L network` |
| Stress (5 min soak, 8 players, extreme netsim) | `tests/test_stress.cpp` | yes (label `stress`) | `ctest -L stress` |
| ASan + UBSan | `IRONCLAD_SANITIZE=ON` | CI job | `cmake … -DIRONCLAD_SANITIZE=ON && ctest` |
| libFuzzer (replay/packet/range_coder/delta) | `tests/fuzz_*.cpp` | clang-only, gated | `cmake -DIRONCLAD_BUILD_FUZZERS=ON` then `ctest -L fuzz` |
| Cross-platform replay determinism | CI step | yes | every matrix cell re-runs `--replay-info samples/sample.iclr` |
| clang-tidy (bugprone/cert/cppcoreguidelines) | `.clang-tidy` | manual | `clang-tidy -p build src/*.cpp` |

## Real bugs found and fixed

### 1. Desync detector never actually detected divergence

**Found by:** wiring up real UDP between two processes. The two
peers' `last_state_hash` values diverged immediately (each side
fills in remote inputs from a different sequence of received
packets), but neither side's `desync_detected` flag fired.

**Root cause:** the `ack_hash`/`ack_frame` field in `InputPacket`
was being **bounced back** — peer A sent peer B's most-recent
attested hash back to peer B, who then compared it against its own
snapshot and obviously matched. No peer ever attested its OWN
hash; the cross-attestation loop was a no-op.

**Fix:** changed semantics so each peer attests *its own* hash
for the most recent fully-acked frame, with a 30-frame margin so
in-flight rollbacks have time to settle on the receiver side.
Evaluation order also moved to AFTER local rollback so the
comparison uses the receiver's post-correction snapshot, not its
stale prediction. See `src/session.cpp::drain_transport` +
`broadcast_input_packet`.

### 2. ByteReader UB on `nullptr` / zero-length

**Found by:** running the doctest suite under `-fsanitize=undefined`.

**Root cause:** `ByteReader::read_bytes(nullptr, 0)` called
`memset(nullptr, 0, 0)`. Per C standard, even zero-byte memcpy /
memset to a null pointer is UB.

**Fix:** early-return on `n == 0`; also nullguard `dst` before
`memset` on the error path. See `include/ironclad/byteio.hpp`.

### 3. Replay parser allocated up to 4 GB on adversarial input

**Found by:** `fuzz_replay_parse` libFuzzer harness. Input was an
otherwise-valid v2 header with `init_size` = `0xFFFFFFFF`.

**Root cause:** `parse_replay` did
`hdr.initial_snapshot.resize(init_size)` without bounds-checking
against the actual file length.

**Fix:** reject `init_size > bytes.size()`,
`num_players == 0 || num_players > 64`, `world_capacity > 65536`
in the header. Also added a similar pre-check on the lag-event
block's count field. See `src/recorder.cpp::parse_replay`.

### 4. Range coder ran for 32 M iterations on 4-byte adversarial input

**Found by:** `fuzz_range_coder` libFuzzer harness with `-timeout=2`.

**Root cause:** `BitDecoder` zero-pads past EOF. Combined with the
adaptive model's biased early state, certain 3-byte payloads
caused the decoder to keep emitting "valid" symbols indefinitely
until the 32 MB output cap.

**Fix:** track bytes consumed past EOF in `BitDecoder::read_bit`;
after 64 zero-padded bytes set the error flag so
`RangeCoder::decode` returns `false`. Real payloads finish
encoding/decoding in a few padding bytes max. See
`src/range_coder.cpp`.

### 5. Sign-conversion warning in `Bitmap::test`

**Found by:** ASan/UBSan build with `-Wsign-conversion -Werror`.

**Root cause:** integer promotion in `(bytes_[i >> 3] >> (i & 7u)) & 1u`
returned `int`, then implicit narrowing back to `bool`.

**Fix:** explicit `static_cast<unsigned>` for the shift and the
byte before `&`. See `include/ironclad/ecs.hpp::Bitmap::test`.

### 6. Vec2 `constexpr` chain broken by MSVC port

**Found by:** the Phase 7 MSVC port required `Fixed::operator*` to
be non-`constexpr` (because `_mul128` is not). `Vec2`'s
`constexpr` arithmetic transitively used it and silently went
non-`constexpr`-callable on Debug builds.

**Fix:** dropped `constexpr` from `Vec2::operator*`, `dot`,
`length_sq` to keep the API uniform across toolchains. See
`include/ironclad/vec2.hpp`.

## Methodology

* **Per-subsystem unit tests** (existing) cover the happy path and
  hand-picked edges of every public type.
* **Property tests** (Phase 8c) sample 1000–5000 random inputs per
  property and check universal invariants (commutativity,
  transitivity, round-trip).
* **Sanitizer build** (Phase 8a) runs the entire unit suite with
  ASan + UBSan + `-fno-sanitize-recover=all`. Catches UB, OOB,
  use-after-free, integer overflow, etc.
* **libFuzzer harnesses** (Phase 8b) use coverage-guided fuzzing
  to drive the four most attacker-exposed parsers (replay file,
  input packet, range coder, delta) into corner-case inputs. Each
  runs for 30s in CI; longer manual runs surface edge cases.
* **Stress tests** (Phase 8d) exercise long-running soak
  conditions, max-player count, and pathological network
  parameters in a few seconds of headless wall-clock.
* **Cross-platform replay determinism** (Phase 6) commits a small
  `samples/sample.iclr` recorded on Linux/GCC and has every CI
  matrix cell re-validate its hash chain. Catches compiler-
  specific drift in `__int128` / `_mul128` / serialization order.

## How to reproduce

```bash
# Sanitizers
cmake -S . -B build_san -DCMAKE_BUILD_TYPE=Debug -DIRONCLAD_SANITIZE=ON
cmake --build build_san --parallel
ctest --test-dir build_san --output-on-failure

# Fuzzers (clang only)
CC=clang CXX=clang++ cmake -S . -B build_fuzz \
    -DCMAKE_BUILD_TYPE=Debug -DIRONCLAD_BUILD_FUZZERS=ON \
    -DIRONCLAD_BUILD_SDL_DEMO=OFF -DIRONCLAD_USE_IMGUI=OFF \
    -DIRONCLAD_WERROR=OFF
cmake --build build_fuzz --parallel
for f in fuzz_replay_parse fuzz_packet_decode \
         fuzz_range_coder fuzz_delta_decode; do
    timeout 60 ./build_fuzz/tests/$f -max_total_time=30 -timeout=2
done

# Stress
ctest --test-dir build -L stress --output-on-failure

# clang-tidy (warnings only — not gating)
clang-tidy -p build src/*.cpp
```
