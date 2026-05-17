// SPDX-License-Identifier: Apache-2.0
/// @file   plugins/strategies/float_send_rtt/float_send_rtt.cpp

#include "float_send_rtt.hpp"

#include <sdk/extensions/link.h>      // GN_LINK_CAP_ENCRYPTED_PATH

#include <algorithm>
#include <cstring>

namespace gn::strategy::float_send_rtt {


std::uint64_t FloatSendRtt::pk_to_key(
    const std::uint8_t pk[GN_PUBLIC_KEY_BYTES]) noexcept {
    /// FNV-1a 64-bit fold of the 32-byte public key. Collisions
    /// across honest peers are negligible (≈ 2⁻³² for a million
    /// peers); a collision degrades a single peer's hysteresis
    /// memory to the colliding peer's last winner, not a crash.
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (std::size_t i = 0; i < GN_PUBLIC_KEY_BYTES; ++i) {
        h ^= static_cast<std::uint64_t>(pk[i]);
        h *= 0x100000001b3ULL;
    }
    return h;
}

FloatSendRtt::FloatSendRtt(const host_api_t* api) noexcept
    : api_(api) {}

int FloatSendRtt::rank_rtt(std::uint64_t a, std::uint64_t b) noexcept {
    /// "Unknown" (zero) ranks worse than any positive number so a
    /// fresh conn doesn't accidentally win against a measured one.
    if (a == 0 && b == 0) return 0;
    if (a == 0) return  1;   // a worse
    if (b == 0) return -1;   // b worse
    if (a < b)  return -1;
    if (a > b)  return  1;
    return 0;
}

gn_result_t FloatSendRtt::pick_conn(
    const std::uint8_t peer_pk[GN_PUBLIC_KEY_BYTES],
    const gn_path_sample_t* candidates,
    std::size_t count,
    gn_conn_id_t* out_chosen) noexcept {
    if (!peer_pk || !candidates || !out_chosen) {
        return GN_ERR_NULL_ARG;
    }
    /// Empty candidate set is "no winner exists" rather than
    /// "missing argument" — fold it into `GN_ERR_NOT_FOUND` so the
    /// kernel can surface a "no usable conn" diagnostic instead of
    /// the misleading null-arg complaint the previous shape returned.
    if (count == 0) {
        return GN_ERR_NOT_FOUND;
    }

    /// Snapshot every candidate's tracked state under a single shared
    /// lock so the loop sees a coherent view.
    struct Ranked {
        gn_conn_id_t  conn;
        std::uint64_t rtt_us;
        std::uint32_t caps;
    };
    Ranked best{candidates[0].conn, 0, 0};
    bool   have_best = false;
    {
        std::shared_lock lk(paths_mu_);
        for (std::size_t i = 0; i < count; ++i) {
            const auto& c = candidates[i];
            std::uint64_t rtt = c.rtt_us;
            std::uint32_t caps = c.caps;
            /// `gn_path_sample_t::rtt_us` carries the kernel's
            /// smoothed EWMA (per `sdk/extensions/strategy.h`). We
            /// keep an additional per-conn EWMA over those values
            /// so the picker has the most recent winner-stable
            /// signal even when `pick_conn` runs faster than
            /// `on_path_event` snapshots arrive. Fall back to the
            /// kernel value if we have no history for the conn yet.
            if (auto it = paths_.find(c.conn); it != paths_.end()) {
                if (it->second.rtt_ewma_us != 0) rtt  = it->second.rtt_ewma_us;
                if (it->second.caps != 0)         caps = it->second.caps;
            }
            const Ranked cur{c.conn, rtt, caps};
            if (!have_best) {
                best      = cur;
                have_best = true;
                continue;
            }
            const int cmp = rank_rtt(cur.rtt_us, best.rtt_us);
            if (cmp < 0) {
                best = cur;
            } else if (cmp == 0) {
                /// RTT tie — prefer the encrypted-path candidate.
                const bool cur_enc =
                    (cur.caps  & GN_LINK_CAP_ENCRYPTED_PATH) != 0;
                const bool best_enc =
                    (best.caps & GN_LINK_CAP_ENCRYPTED_PATH) != 0;
                if (cur_enc && !best_enc) best = cur;
            } else {
                /// `cur` worse on raw RTT — but check the tie band:
                /// when within ±`kCapTieBand` and `cur` is encrypted
                /// while `best` is not, still flip.
                if (best.rtt_us != 0) {
                    const float ratio =
                        static_cast<float>(cur.rtt_us) /
                        static_cast<float>(best.rtt_us);
                    if (ratio < (1.0f + kCapTieBand)) {
                        const bool cur_enc =
                            (cur.caps & GN_LINK_CAP_ENCRYPTED_PATH) != 0;
                        const bool best_enc =
                            (best.caps & GN_LINK_CAP_ENCRYPTED_PATH) != 0;
                        if (cur_enc && !best_enc) best = cur;
                    }
                }
            }
        }
    }

    /// Hysteresis: if the previous winner for this peer is still in
    /// the candidate set, only flip when the new candidate is
    /// `switch_threshold` faster than the old one.
    const std::uint64_t pk_key = pk_to_key(peer_pk);
    PeerWinner prev{};
    {
        std::shared_lock wl(winners_mu_);
        if (auto it = winners_.find(pk_key); it != winners_.end()) {
            prev = it->second;
        }
    }
    if (prev.conn != GN_INVALID_ID && prev.conn != best.conn) {
        /// Find the previous winner's current sample (if still alive).
        for (std::size_t i = 0; i < count; ++i) {
            if (candidates[i].conn != prev.conn) continue;
            std::uint64_t prev_rtt = candidates[i].rtt_us;
            {
                std::shared_lock pl(paths_mu_);
                if (auto it = paths_.find(prev.conn); it != paths_.end()) {
                    if (it->second.rtt_ewma_us != 0) {
                        prev_rtt = it->second.rtt_ewma_us;
                    }
                }
            }
            if (prev_rtt == 0 || best.rtt_us == 0) break;
            const float ratio =
                static_cast<float>(best.rtt_us) /
                static_cast<float>(prev_rtt);
            const float thresh =
                switch_threshold_.load(std::memory_order_relaxed);
            if (ratio >= thresh) {
                /// New candidate not fast enough — stick with previous.
                best = Ranked{prev.conn, prev_rtt, 0};
            }
            break;
        }
    }

    {
        std::unique_lock wl(winners_mu_);
        winners_[pk_key] = PeerWinner{best.conn, best.rtt_us};
    }
    *out_chosen = best.conn;
    return GN_OK;
}

gn_result_t FloatSendRtt::on_path_event(
    const std::uint8_t peer_pk[GN_PUBLIC_KEY_BYTES],
    gn_path_event_t ev,
    const gn_path_sample_t* sample) noexcept {
    if (!peer_pk) return GN_ERR_NULL_ARG;

    switch (ev) {
        case GN_PATH_EVENT_CONN_UP: {
            if (!sample) return GN_OK;
            std::unique_lock lk(paths_mu_);
            auto& st = paths_[sample->conn];
            /// Fresh conn — reset EWMA so the first sample replaces
            /// "unknown" cleanly rather than averaging with zero.
            st.rtt_ewma_us = sample->rtt_us;
            st.caps        = sample->caps;
            break;
        }
        case GN_PATH_EVENT_CONN_DOWN: {
            /// `sample` carries the specific conn id when the kernel
            /// knows which one went down. Without it the strategy
            /// cannot prune `paths_` (keyed by conn) but it MUST
            /// still invalidate the winner cache for this peer —
            /// otherwise a subsequent `pick_conn` returns the cached
            /// winner that just dropped, and the failover sequence
            /// stalls. The earlier `return GN_OK` on a null sample
            /// left the cache populated and made the strategy
            /// transparently sticky to a dead conn.
            const std::uint64_t pk_key = pk_to_key(peer_pk);
            if (sample) {
                std::unique_lock lk(paths_mu_);
                paths_.erase(sample->conn);
            }
            std::unique_lock wl(winners_mu_);
            if (auto it = winners_.find(pk_key); it != winners_.end()) {
                if (sample == nullptr || it->second.conn == sample->conn) {
                    winners_.erase(it);
                }
            }
            break;
        }
        case GN_PATH_EVENT_RTT_UPDATE: {
            if (!sample || sample->rtt_us == 0) return GN_OK;
            std::unique_lock lk(paths_mu_);
            auto& st = paths_[sample->conn];
            /// `sample->rtt_us` is already the kernel-smoothed
            /// EWMA(α = 1/8) per `host_api->notify_rtt_sample`.
            /// We compose a second EWMA over those values so a
            /// burst of late `pick_conn` calls between event
            /// snapshots still reflects the trend; the effective
            /// time constant is heavier, which the picker's
            /// hysteresis threshold accounts for. Cheap shift/
            /// add keeps the on_path_event hot path under a
            /// microsecond.
            st.rtt_ewma_us = (st.rtt_ewma_us == 0)
                ? sample->rtt_us
                : (st.rtt_ewma_us * 7 + sample->rtt_us) / 8;
            break;
        }
        case GN_PATH_EVENT_LOSS_DETECTED:
            /// Loss-aware routing is a v1.1 feature — the picker
            /// ignores loss in v1 because the loss signal is noisier
            /// than RTT and the wire shape isn't yet stable.
            break;
        case GN_PATH_EVENT_CAPABILITY_REFRESH:
            if (sample) {
                std::unique_lock lk(paths_mu_);
                paths_[sample->conn].caps = sample->caps;
            }
            break;
    }
    return GN_OK;
}

std::uint64_t FloatSendRtt::rtt_ewma_us(
    gn_conn_id_t conn) const noexcept {
    std::shared_lock lk(paths_mu_);
    auto it = paths_.find(conn);
    return (it == paths_.end()) ? 0 : it->second.rtt_ewma_us;
}

gn_conn_id_t FloatSendRtt::last_winner(
    const std::uint8_t peer_pk[GN_PUBLIC_KEY_BYTES]) const noexcept {
    const std::uint64_t pk_key = pk_to_key(peer_pk);
    std::shared_lock wl(winners_mu_);
    auto it = winners_.find(pk_key);
    return (it == winners_.end()) ? GN_INVALID_ID : it->second.conn;
}

void FloatSendRtt::reset_for_test() noexcept {
    std::unique_lock pl(paths_mu_);
    paths_.clear();
    std::unique_lock wl(winners_mu_);
    winners_.clear();
}

}  // namespace gn::strategy::float_send_rtt
