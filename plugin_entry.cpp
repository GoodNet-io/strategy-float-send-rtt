// SPDX-License-Identifier: Apache-2.0
/// @file   plugins/strategies/float_send_rtt/plugin_entry.cpp
/// @brief  `GN_STRATEGY_PLUGIN` macro expansion — registers
///         `gn.strategy.rtt-optimal` and emits the six
///         `gn_plugin_*` extern "C" entry points.

#include <sdk/cpp/strategy_plugin.hpp>

#include "float_send_rtt.hpp"

GN_STRATEGY_PLUGIN(
    ::gn::strategy::float_send_rtt::FloatSendRtt,
    "float-send-rtt",
    "0.1.0")
