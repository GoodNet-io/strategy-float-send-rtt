// SPDX-License-Identifier: Apache-2.0
/// @file   plugins/strategies/float_send_rtt/float_send_rtt.hpp
/// @brief  `gn.strategy.rtt-optimal` — reference strategy plugin
///         that ranks conns by smoothed RTT.
///
/// Decision logic, mirroring `docs/architecture/strategies.ru.md` §3
/// "smart routing":
///   * Per-conn smoothed RTT (EWMA, α = 1/8 per RFC 6298) tracked
///     across `GN_PATH_EVENT_RTT_UPDATE` events.
///   * Per-peer "currently selected" conn-id stored so the picker can
///     apply hysteresis — switch from the active conn to a candidate
///     only when the candidate RTT is below
///     `switch_threshold * active_rtt`. Default threshold 0.75 — the
///     candidate must be 25 % faster before we flip, preventing
///     thrash between two conns whose RTT samples oscillate around a
///     similar mean.
///   * Ties broken by capability flags: an `EncryptedPath`-capable
///     conn wins over a plain one when smoothed RTTs are within ±5 %.
///   * `GN_PATH_EVENT_CONN_DOWN` evicts the conn from local tracking;
///     `GN_PATH_EVENT_CONN_UP` resets per-conn EWMA so the very next
///     sample replaces "unknown" without averaging in the zero.

#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include <sdk/extensions/strategy.h>
#include <sdk/host_api.h>
#include <sdk/types.h>

namespace gn::strategy::float_send_rtt {

inline constexpr const char* kExtensionName    = "gn.strategy.rtt-optimal";
inline constexpr std::uint32_t kExtensionVersion = 0x00010000U;

/// Default switch threshold — the picker only flips to a new conn
/// when its smoothed RTT is below 75 % of the currently active conn.
inline constexpr float kDefaultSwitchThreshold = 0.75f;

/// "Tie band" within which RTT is treated as equal so capability
/// flags break the tie. 5 % gives a comfortable margin around the
/// 1 % typical sample-to-sample jitter on a stable link.
inline constexpr float kCapTieBand = 0.05f;

/// Strategy plugin class. The `GN_STRATEGY_PLUGIN` macro in
/// `plugin_entry.cpp` registers `gn.strategy.rtt-optimal` and wires
/// the C ABI through SFINAE thunks against this class.
class FloatSendRtt {
public:
    explicit FloatSendRtt(const host_api_t* api) noexcept;

    FloatSendRtt(const FloatSendRtt&)            = delete;
    FloatSendRtt& operator=(const FloatSendRtt&) = delete;

    /// Required extension surface — picked up by the macro.
    static constexpr const char* extension_name() noexcept {
        return kExtensionName;
    }
    static constexpr std::uint32_t extension_version() noexcept {
        return kExtensionVersion;
    }

    /// Required `pick_conn`. Walks @p candidates, applies hysteresis
    /// against the previous winner stored under @p peer_pk.
    gn_result_t pick_conn(
        const std::uint8_t peer_pk[GN_PUBLIC_KEY_BYTES],
        const gn_path_sample_t* candidates,
        std::size_t count,
        gn_conn_id_t* out_chosen) noexcept;

    /// Optional `on_path_event` — updates internal RTT EWMA + evicts
    /// dead conns. Best-effort; never blocks the kernel's dispatcher
    /// for long.
    gn_result_t on_path_event(
        const std::uint8_t peer_pk[GN_PUBLIC_KEY_BYTES],
        gn_path_event_t ev,
        const gn_path_sample_t* sample) noexcept;

    /// Test / inspection helper. Returns 0 if the conn is unknown.
    [[nodiscard]] std::uint64_t rtt_ewma_us(
        gn_conn_id_t conn) const noexcept;

    /// Test / inspection helper. Returns the previous winner for
    /// @p peer_pk, or `GN_INVALID_ID` if no `pick_conn` has fired yet.
    [[nodiscard]] gn_conn_id_t last_winner(
        const std::uint8_t peer_pk[GN_PUBLIC_KEY_BYTES]) const noexcept;

    /// Test helper — override the hysteresis threshold. In production
    /// the value is read from `strategies.rtt-optimal.switch_threshold`
    /// config key (future work; the v1 default is hard-coded).
    void set_switch_threshold(float t) noexcept {
        switch_threshold_.store(t, std::memory_order_relaxed);
    }

    /// Test helper. Clears all tracked state.
    void reset_for_test() noexcept;

private:
    struct PathState {
        /// EWMA RTT in microseconds. 0 means "no sample yet", which
        /// is treated as worst-case (won't be picked unless every
        /// candidate is at 0).
        std::uint64_t rtt_ewma_us = 0;
        /// Capability flags snapshot from the link plugin
        /// (`GN_LINK_CAP_*`). Used for tie-breaking.
        std::uint32_t caps = 0;
    };

    struct PeerWinner {
        gn_conn_id_t   conn    = GN_INVALID_ID;
        std::uint64_t  rtt_us  = 0;
    };

    /// Folds the 32-byte peer pk into a 64-bit key. Collisions are
    /// vanishingly rare for honest peers (≈ 2⁻³² across a million
    /// peers); strategy correctness degrades gracefully — a
    /// collision means two peers share winner state, the next
    /// `pick_conn` for either rebuilds the winner anyway. Using a
    /// uint64_t key sidesteps libstdc++ 15's stricter noexcept gates
    /// on hashtable keyed by `std::array<uint8_t, N>`.
    [[nodiscard]] static std::uint64_t pk_to_key(
        const std::uint8_t pk[GN_PUBLIC_KEY_BYTES]) noexcept;

    /// Compare smoothed RTT with the "rtt_ewma == 0 means unknown"
    /// rule: unknown ranks WORSE than any positive number. Returns
    /// negative when @p a is preferable, positive when @p b is.
    [[nodiscard]] static int rank_rtt(std::uint64_t a,
                                       std::uint64_t b) noexcept;

    const host_api_t* api_ = nullptr;
    std::atomic<float> switch_threshold_{kDefaultSwitchThreshold};

    /// Per-conn RTT/caps tracker. Updated under `paths_mu_` exclusive;
    /// read under `paths_mu_` shared from `pick_conn` hot path.
    mutable std::shared_mutex                                 paths_mu_;
    std::unordered_map<gn_conn_id_t, PathState>                paths_;

    /// Per-peer winner — survives between `pick_conn` calls so
    /// hysteresis can compare against the previous decision.
    mutable std::shared_mutex                                 winners_mu_;
    std::unordered_map<std::uint64_t, PeerWinner>              winners_;
};

}  // namespace gn::strategy::float_send_rtt
