#pragma once
#include <cstdint>

namespace ob {

using OrderId = std::uint64_t;
using Price = std::int32_t;   // integer tick index in [0, num_ticks)
using Qty = std::uint32_t;

enum class Side : std::uint8_t { Buy, Sell };

// A single fill. `price` is the resting (maker) order's price, per convention:
// a crossing taker trades at the maker's posted price (price-time priority).
struct Trade {
    OrderId taker;
    OrderId maker;
    Price price;
    Qty qty;

    friend bool operator==(const Trade&, const Trade&) = default;
};

}  // namespace ob
