# Changelog — goodnet-strategy-float-send-rtt

All notable changes to this plugin are listed here. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versions track the kernel ABI through `gn_strategy_vtable_t`
(`sdk/extensions/strategy.h`).

## [Unreleased]

### Winner-cache + empty-set hardening

`pick_conn` now returns `GN_STATUS_NOT_FOUND` when the
candidate set is empty rather than dereferencing a stale winner
pointer. The per-peer winner cache is invalidated on every
`CONN_DOWN` event whose sample is null (the kernel signals
"this conn is gone, no final RTT") so subsequent picks rebuild
from the live snapshot.

### EWMA contract alignment

The smoothed-RTT comment block in `float_send_rtt.cpp` is
aligned with the kernel-smoothed sample contract: the kernel
hands a per-event sample, the plugin applies the α = 1/8 EWMA
locally per RFC 6298, and the result feeds `pick_conn`. The
behaviour itself is unchanged; the comment block previously
described the smoothing as kernel-side.

### Multi-strategy admission landed kernel-side

The README reflects the kernel's current dispatch behaviour:
multiple `gn.strategy.*` plugins are admitted simultaneously,
and the chain is walked in registration order on every
`send_to` — the first strategy that returns a real conn wins.
Operators can stack `rtt-optimal` against a fallback such as
`cost-aware` without further gating.

## [1.0.0-rc1] — 2026-05-12

Initial release. Reference multi-path picker — selects the
connection with the lowest smoothed RTT when an outbound
message has several eligible conns to the same peer.

### Added

- `gn.strategy.rtt-optimal` extension exposing the `pick_conn`
  surface from `sdk/extensions/strategy.h`. The kernel's
  `send_to` thunk consults this entry on every multi-conn
  destination.
- Per-conn smoothed RTT — EWMA with α = 1/8 (RFC 6298),
  updated on every `GN_PATH_EVENT_RTT_UPDATE`. Unknown RTT
  (`rtt_us == 0`) ranks worse than any measured value so a
  fresh conn does not pre-empt a slow but observed one.
- Hysteresis on winner switch — flip away from the previous
  winner only when the candidate's RTT is below
  `switch_threshold * previous_rtt` (default 0.75 — 25 % faster
  required). Tunable via
  `FloatSendRtt::set_switch_threshold(float)`.
- Tie band (±5 %) — an `EncryptedPath`-capable conn wins over a
  plain one when RTTs are within the band.
- `GN_PATH_EVENT_CONN_DOWN` evicts the conn from local
  tracking and clears the per-peer winner if it pointed at the
  dead conn.
- Tests covering RTT updates, winner switching with hysteresis,
  the tie band, and CONN_DOWN eviction under
  `tests/test_float_send_rtt.cpp`.
- Plugin entry built through `sdk/cpp/strategy_plugin.hpp`'s
  `GN_STRATEGY_PLUGIN` macro.

### Out of scope for v1

- Loss-aware routing — `GN_PATH_EVENT_LOSS_DETECTED` is
  currently ignored. A future revision will weigh loss
  alongside RTT.
- Operator config key — `switch_threshold` is currently only
  reachable through the API setter; a
  `strategies.rtt-optimal.switch_threshold` key will follow.
