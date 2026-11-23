#pragma once

#include <algorithm>
#include <vector>

namespace NetworkScheduling {
struct Demand {
    double desired_hz;
    double minimum_hz;
    double bytes;
    bool critical = false;
};

// Reserve combat's minimum rates before spending spare bandwidth on detail.
// In overload even the reservations must share the available connection budget.
inline std::vector<double> Rates(const std::vector<Demand>& demands, double budget) {
    double critical_cost = 0.0, routine_cost = 0.0, extra_cost = 0.0;
    for (const auto& d : demands) {
        const double floor = std::max(0.0, std::min(d.minimum_hz, d.desired_hz));
        (d.critical ? critical_cost : routine_cost) += floor * d.bytes;
        extra_cost += (d.desired_hz - floor) * d.bytes;
    }
    const double critical_scale = critical_cost > 0.0 ? std::max(0.0, std::min(1.0, budget / critical_cost)) : 1.0;
    const double remaining = std::max(0.0, budget - critical_cost);
    const double routine_scale = routine_cost > 0.0 ? std::min(1.0, remaining / routine_cost) : 1.0;
    const double extra_scale = extra_cost > 0.0 ? std::max(0.0, std::min(1.0, (remaining - routine_cost) / extra_cost)) : 0.0;
    std::vector<double> rates;
    rates.reserve(demands.size());
    for (const auto& d : demands) {
        const double floor = std::max(0.0, std::min(d.minimum_hz, d.desired_hz));
        rates.push_back(floor * (d.critical ? critical_scale : routine_scale) + (d.desired_hz - floor) * extra_scale);
    }
    return rates;
}
}  // namespace NetworkScheduling
