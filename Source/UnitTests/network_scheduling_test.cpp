#include <Online/network_scheduling.h>
#include <tut/tut.hpp>

namespace tut {
struct network_scheduling_data {};
typedef test_group<network_scheduling_data> network_scheduling_group;
network_scheduling_group network_scheduling_tests("NetworkScheduling");
typedef network_scheduling_group::object scheduling_test;

template<> template<> void scheduling_test::test<1>() {
    const auto rates = NetworkScheduling::Rates({{30, 20, 100}, {15, 1, 100}}, 10000);
    ensure_equals(rates[0], 30.0);
    ensure_equals(rates[1], 15.0);
}
template<> template<> void scheduling_test::test<2>() {
    std::vector<NetworkScheduling::Demand> demand(20, {30, 1, 200});
    demand[0].minimum_hz = 20;
    demand[0].critical = true;
    const auto rates = NetworkScheduling::Rates(demand, 10000);
    ensure("nearby fight retains its reserved update rate", rates[0] >= 20.0);
    ensure("background yields before the fight", rates[1] < rates[0]);
    double used = 0;
    for (size_t i = 0; i < rates.size(); ++i) used += rates[i] * demand[i].bytes;
    ensure("allocation respects total budget", used <= 10000.0001);
}
template<> template<> void scheduling_test::test<3>() {
    const auto rates = NetworkScheduling::Rates({{30, 20, 100, true}, {30, 20, 100, true}}, 1000);
    ensure_equals(rates[0], 5.0);
    ensure_equals(rates[1], 5.0);
    const auto zero = NetworkScheduling::Rates({{30, 20, 100}}, 0);
    ensure_equals(zero[0], 0.0);
}
template<> template<> void scheduling_test::test<4>() {
    std::vector<NetworkScheduling::Demand> demand(100, {30, 1, 300});
    demand.push_back({30, 20, 1000, true});
    demand.push_back({30, 20, 1000, true});
    const auto rates = NetworkScheduling::Rates(demand, 40000);
    ensure_equals("background does not dilute combat reservations", rates[100], 20.0);
    ensure_equals(rates[101], 20.0);
    for (int i = 0; i < 100; ++i) ensure_equals(rates[i], 0.0);
}
}
