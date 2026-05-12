# goodnet-strategy-float-send-rtt

Reference multi-path strategy plugin. Picks the connection with
the lowest smoothed RTT when an outbound message has several
eligible conns to the same peer. Exposes the `gn.strategy.rtt-optimal`
extension (vtable shape: `sdk/extensions/strategy.h`).

**Kind**: strategy · **Artefact**: dynamic plugin (`.so` via dlopen)
· **License**: Apache 2.0 (see `LICENSE`)

## Build

This plugin lives in its own folder with a CMakeLists that consumes
the kernel SDK either through `find_package(GoodNet REQUIRED)`
(standalone) or directly as a sibling target in the monorepo. From
the kernel checkout:

```sh
nix run .#build                              # release build
ctest --test-dir build -R FloatSendRtt       # plugin tests
nix run .#test                               # full kernel + plugin suite
nix run .#test -- asan                       # AddressSanitizer
nix run .#test -- tsan                       # ThreadSanitizer
```

## Load

The kernel's `PluginManager` opens the `.so` from a manifest entry
that pins its SHA-256 digest. One strategy is active per node;
loading a second `gn.strategy.*` plugin (e.g. `cost-aware`)
triggers `GN_ERR_LIMIT_REACHED` from `register_extension`.

## Decision logic

Mirrors `docs/architecture/strategies.ru.md` "smart routing":

- Per-conn smoothed RTT (EWMA, α = 1/8 per RFC 6298) updated on
  every `GN_PATH_EVENT_RTT_UPDATE`.
- `pick_conn` returns the lowest-RTT candidate from the kernel's
  snapshot. Unknown RTT (`rtt_us == 0`) ranks worse than any
  measured value so a fresh conn doesn't pre-empt a slow but
  observed one.
- Hysteresis: only flip from the previous winner when the
  candidate's RTT is below `switch_threshold * previous_rtt`.
  Default `0.75` — 25 % faster required. Tunable via
  `FloatSendRtt::set_switch_threshold(float)` (future:
  `strategies.rtt-optimal.switch_threshold` config key).
- Tie band (±5 %): an `EncryptedPath`-capable conn wins over a
  plain one when RTTs are within the band.
- `GN_PATH_EVENT_CONN_DOWN` evicts the conn from local tracking
  and clears the per-peer winner if it pointed at the dead conn.

## Contract

- Extension vtable: `sdk/extensions/strategy.h`
- SDK macro: `sdk/cpp/strategy_plugin.hpp` (`GN_STRATEGY_PLUGIN`)
- Family overview + design rationale:
  [`docs/architecture/strategies.ru.md`](../../../docs/architecture/strategies.ru.md)
- Reserved namespace catalogue:
  [`docs/architecture/built-in-extensions.ru.md`](../../../docs/architecture/built-in-extensions.ru.md)

## Status

- v0.1.0: shipped 2026-05-12 as part of Слайс 9-RTT.
- v1.0.0 target: stable picker behaviour + operator config key for
  `switch_threshold` + loss-aware routing (currently the
  `GN_PATH_EVENT_LOSS_DETECTED` slot is ignored; v1.1 will weigh
  loss alongside RTT).
- Kernel-side outbound dispatch hook lands in Слайс 9-KERNEL.
  Until then this plugin compiles and unit-tests in isolation; the
  picker logic is exercised through direct `pick_conn` calls in
  `tests/test_float_send_rtt.cpp`.
