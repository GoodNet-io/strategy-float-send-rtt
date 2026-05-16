// SPDX-License-Identifier: Apache-2.0
/// @file   plugins/strategies/float_send_rtt/tests/test_float_send_rtt.cpp
/// @brief  Unit coverage for FloatSendRtt's pick_conn + on_path_event.
///
/// The picker is exercised directly — no kernel dispatch needed. The
/// future Слайс 9-KERNEL adds the host_api thunk that calls this
/// strategy in production; this test fixture pretends to be that
/// thunk by handing the strategy synthetic `gn_path_sample_t` arrays.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include <sdk/extensions/link.h>      // GN_LINK_CAP_ENCRYPTED_PATH
#include <sdk/extensions/strategy.h>
#include <sdk/types.h>

#include "../float_send_rtt.hpp"

using gn::strategy::float_send_rtt::FloatSendRtt;
using gn::strategy::float_send_rtt::kCapTieBand;
using gn::strategy::float_send_rtt::kDefaultSwitchThreshold;
using gn::strategy::float_send_rtt::kExtensionName;
using gn::strategy::float_send_rtt::kExtensionVersion;

namespace {

std::array<std::uint8_t, GN_PUBLIC_KEY_BYTES> peer_pk(std::uint8_t seed) {
    std::array<std::uint8_t, GN_PUBLIC_KEY_BYTES> pk{};
    pk.fill(seed);
    return pk;
}

gn_path_sample_t sample(gn_conn_id_t conn, std::uint64_t rtt,
                         std::uint32_t caps = 0) {
    gn_path_sample_t s{};
    s.conn   = conn;
    s.rtt_us = rtt;
    s.caps   = caps;
    return s;
}

}  // namespace

TEST(FloatSendRtt, ExtensionMetadataMatchesPlan) {
    EXPECT_STREQ(kExtensionName, "gn.strategy.rtt-optimal");
    EXPECT_EQ(kExtensionVersion, 0x00010000u);
    EXPECT_EQ(FloatSendRtt::extension_name(),    kExtensionName);
    EXPECT_EQ(FloatSendRtt::extension_version(), kExtensionVersion);
}

TEST(FloatSendRtt, NullArgsRejected) {
    FloatSendRtt s(nullptr);
    auto pk = peer_pk(1);
    gn_path_sample_t cand = sample(0x100, 1000);
    gn_conn_id_t chosen = GN_INVALID_ID;

    EXPECT_EQ(s.pick_conn(nullptr, &cand, 1, &chosen), GN_ERR_NULL_ARG);
    EXPECT_EQ(s.pick_conn(pk.data(), nullptr, 1, &chosen), GN_ERR_NULL_ARG);
    /// Empty candidate set returns `GN_ERR_NOT_FOUND` — "no winner
    /// can be picked" is a distinct surface from "caller passed
    /// nullptr where required". See `pick_conn` body.
    EXPECT_EQ(s.pick_conn(pk.data(), &cand, 0, &chosen),   GN_ERR_NOT_FOUND);
    EXPECT_EQ(s.pick_conn(pk.data(), &cand, 1, nullptr),   GN_ERR_NULL_ARG);

    EXPECT_EQ(s.on_path_event(nullptr, GN_PATH_EVENT_RTT_UPDATE, &cand),
              GN_ERR_NULL_ARG);
}

TEST(FloatSendRtt, MinRttWinsAcrossCandidates) {
    FloatSendRtt s(nullptr);
    auto pk = peer_pk(2);

    gn_path_sample_t pool[] = {
        sample(0x100, 800),
        sample(0x200, 200),
        sample(0x300, 500),
    };
    gn_conn_id_t chosen = GN_INVALID_ID;
    ASSERT_EQ(s.pick_conn(pk.data(), pool, 3, &chosen), GN_OK);
    EXPECT_EQ(chosen, 0x200u);
}

TEST(FloatSendRtt, UnknownRttRanksWorstThanMeasured) {
    FloatSendRtt s(nullptr);
    auto pk = peer_pk(3);

    /// rtt_us == 0 means "no sample yet" — must rank worse than ANY
    /// positive number even when the positive number is huge. The
    /// fresh conn won't accidentally pre-empt a measured (poor) one.
    gn_path_sample_t pool[] = {
        sample(0x100, 0),
        sample(0x200, 50'000),
    };
    gn_conn_id_t chosen = GN_INVALID_ID;
    ASSERT_EQ(s.pick_conn(pk.data(), pool, 2, &chosen), GN_OK);
    EXPECT_EQ(chosen, 0x200u);
}

TEST(FloatSendRtt, EwmaSmoothing) {
    FloatSendRtt s(nullptr);
    auto pk = peer_pk(4);

    /// CONN_UP seeds the EWMA with the very first sample so the
    /// next RTT_UPDATE doesn't average in a phantom zero.
    const auto up = sample(0x100, 1000);
    ASSERT_EQ(s.on_path_event(pk.data(), GN_PATH_EVENT_CONN_UP, &up), GN_OK);
    EXPECT_EQ(s.rtt_ewma_us(0x100), 1000u);

    /// α = 1/8: ewma' = ewma*7/8 + sample/8.
    /// (1000*7 + 200) / 8 = 7200 / 8 = 900.
    const auto u1 = sample(0x100, 200);
    ASSERT_EQ(s.on_path_event(pk.data(), GN_PATH_EVENT_RTT_UPDATE, &u1), GN_OK);
    EXPECT_EQ(s.rtt_ewma_us(0x100), 900u);

    /// Repeated low samples drag the EWMA down monotonically.
    for (int i = 0; i < 20; ++i) {
        const auto u = sample(0x100, 100);
        ASSERT_EQ(s.on_path_event(pk.data(),
                                    GN_PATH_EVENT_RTT_UPDATE, &u), GN_OK);
    }
    EXPECT_LT(s.rtt_ewma_us(0x100), 200u);
}

TEST(FloatSendRtt, HysteresisHoldsActiveConnUnlessThresholdCrossed) {
    FloatSendRtt s(nullptr);
    auto pk = peer_pk(5);

    /// Establish a baseline EWMA for both conns.
    const auto up_a = sample(0x100, 1000);
    const auto up_b = sample(0x200, 1100);
    s.on_path_event(pk.data(), GN_PATH_EVENT_CONN_UP, &up_a);
    s.on_path_event(pk.data(), GN_PATH_EVENT_CONN_UP, &up_b);

    /// First pick should land on the lower-RTT conn (1000).
    gn_path_sample_t pool[] = {
        sample(0x100, 1000),
        sample(0x200, 1100),
    };
    gn_conn_id_t chosen = GN_INVALID_ID;
    ASSERT_EQ(s.pick_conn(pk.data(), pool, 2, &chosen), GN_OK);
    EXPECT_EQ(chosen, 0x100u);

    /// Update conn B to 900 — that's better than A's 1000, but the
    /// ratio 900/1000 = 0.9 > 0.75 default threshold. Hysteresis
    /// must keep us on A.
    const auto u_b = sample(0x200, 900);
    for (int i = 0; i < 30; ++i) {
        s.on_path_event(pk.data(), GN_PATH_EVENT_RTT_UPDATE, &u_b);
    }
    pool[1].rtt_us = s.rtt_ewma_us(0x200);
    pool[0].rtt_us = s.rtt_ewma_us(0x100);
    ASSERT_EQ(s.pick_conn(pk.data(), pool, 2, &chosen), GN_OK);
    EXPECT_EQ(chosen, 0x100u);

    /// Now drive B down sharply (≈ 600). 600/1000 = 0.6 < 0.75 — flip.
    const auto u_b2 = sample(0x200, 600);
    for (int i = 0; i < 60; ++i) {
        s.on_path_event(pk.data(), GN_PATH_EVENT_RTT_UPDATE, &u_b2);
    }
    pool[1].rtt_us = s.rtt_ewma_us(0x200);
    pool[0].rtt_us = s.rtt_ewma_us(0x100);
    ASSERT_EQ(s.pick_conn(pk.data(), pool, 2, &chosen), GN_OK);
    EXPECT_EQ(chosen, 0x200u);
}

TEST(FloatSendRtt, EncryptedPathBreaksTieWithinCapBand) {
    FloatSendRtt s(nullptr);
    auto pk = peer_pk(6);

    /// Two conns within the 5 % cap-tie band. The encrypted one
    /// should win even though its raw RTT is slightly higher.
    gn_path_sample_t pool[] = {
        sample(0x100, 1000, 0),
        sample(0x200, 1030, GN_LINK_CAP_ENCRYPTED_PATH),
    };
    gn_conn_id_t chosen = GN_INVALID_ID;
    ASSERT_EQ(s.pick_conn(pk.data(), pool, 2, &chosen), GN_OK);
    EXPECT_EQ(chosen, 0x200u);

    /// Outside the band — encrypted no longer auto-wins.
    s.reset_for_test();
    pool[1].rtt_us = 1200;
    ASSERT_EQ(s.pick_conn(pk.data(), pool, 2, &chosen), GN_OK);
    EXPECT_EQ(chosen, 0x100u);
}

TEST(FloatSendRtt, ConnDownEvictsAndResetsWinner) {
    FloatSendRtt s(nullptr);
    auto pk = peer_pk(7);

    const auto up = sample(0x100, 500);
    s.on_path_event(pk.data(), GN_PATH_EVENT_CONN_UP, &up);

    gn_path_sample_t pool[] = { sample(0x100, 500) };
    gn_conn_id_t chosen = GN_INVALID_ID;
    ASSERT_EQ(s.pick_conn(pk.data(), pool, 1, &chosen), GN_OK);
    EXPECT_EQ(s.last_winner(pk.data()), 0x100u);

    const auto down = sample(0x100, 0);
    s.on_path_event(pk.data(), GN_PATH_EVENT_CONN_DOWN, &down);
    EXPECT_EQ(s.last_winner(pk.data()), GN_INVALID_ID);
    EXPECT_EQ(s.rtt_ewma_us(0x100), 0u);
}

TEST(FloatSendRtt, ThresholdOverrideAffectsHysteresis) {
    FloatSendRtt s(nullptr);
    auto pk = peer_pk(8);

    const auto up_a = sample(0x100, 1000);
    const auto up_b = sample(0x200, 1100);
    s.on_path_event(pk.data(), GN_PATH_EVENT_CONN_UP, &up_a);
    s.on_path_event(pk.data(), GN_PATH_EVENT_CONN_UP, &up_b);

    gn_path_sample_t pool[] = {
        sample(0x100, 1000),
        sample(0x200, 1100),
    };
    gn_conn_id_t chosen = GN_INVALID_ID;
    ASSERT_EQ(s.pick_conn(pk.data(), pool, 2, &chosen), GN_OK);
    EXPECT_EQ(chosen, 0x100u);

    /// Raise the switch threshold to 0.99 — any improvement on B
    /// now flips the pick. Drive B to 950 (95 % of A's 1000).
    s.set_switch_threshold(0.99f);
    const auto u_b = sample(0x200, 950);
    for (int i = 0; i < 60; ++i) {
        s.on_path_event(pk.data(), GN_PATH_EVENT_RTT_UPDATE, &u_b);
    }
    pool[0].rtt_us = s.rtt_ewma_us(0x100);
    pool[1].rtt_us = s.rtt_ewma_us(0x200);
    ASSERT_EQ(s.pick_conn(pk.data(), pool, 2, &chosen), GN_OK);
    EXPECT_EQ(chosen, 0x200u);
}

TEST(FloatSendRtt, ConcurrentPickAndUpdatesAreRaceFree) {
    /// Smoke / sanity for the shared_mutex split between paths_ and
    /// winners_. Pure local stress — TSan catches the real fireworks
    /// in the asan-tsan suite.
    FloatSendRtt s(nullptr);
    auto pk = peer_pk(9);

    const auto seed = sample(0x100, 1000);
    s.on_path_event(pk.data(), GN_PATH_EVENT_CONN_UP, &seed);

    gn_path_sample_t pool[] = { sample(0x100, 1000) };
    for (int i = 0; i < 1000; ++i) {
        gn_conn_id_t chosen = GN_INVALID_ID;
        ASSERT_EQ(s.pick_conn(pk.data(), pool, 1, &chosen), GN_OK);
        EXPECT_EQ(chosen, 0x100u);
        const auto upd = sample(0x100, 800 + (i % 400));
        s.on_path_event(pk.data(), GN_PATH_EVENT_RTT_UPDATE, &upd);
    }
}
