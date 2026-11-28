#include <Online/lobby_authority.h>
#include <tut/tut.hpp>

namespace tut {
struct lobby_authority_data {
    std::vector<LobbyAuthority::Seat> seats{{255,255,0},{256,255,1},{1,1,0},{257,1,1}};
    LobbyAuthority::State roles;
    lobby_authority_data() { roles.Refresh(seats); }
};
typedef test_group<lobby_authority_data> lobby_authority_group;
lobby_authority_group lobby_authority_tests("LobbyAuthority");
typedef lobby_authority_group::object lobby_authority_test;

template<> template<> void lobby_authority_test::test<1>() {
    set_test_name("promoted local leader keeps identity and permits removing the old primary");
    ensure(roles.CanPromoteLeader(seats, 255, 256));
    roles.leaders[255] = 256;
    ensure(!roles.CanRemove(seats, 256, 255)); // the decision host must transfer too
    ensure(roles.CanPromoteHost(seats, 255, 256));
    roles.host = 256;
    ensure(roles.CanRemove(seats, 256, 255));
    seats.erase(seats.begin()); seats.front().local_index = 0;
    roles.Refresh(seats);
    ensure_equals(roles.host, 256u);
    ensure(roles.IsLeader(seats, 256));
}
template<> template<> void lobby_authority_test::test<2>() {
    set_test_name("decision host may be remote without changing local leaders");
    ensure(roles.CanPromoteHost(seats, 255, 257)); roles.host = 257;
    ensure(!roles.CanPromoteHost(seats, 255, 256));
    ensure(roles.CanPromoteHost(seats, 257, 255));
    ensure(!roles.IsLeader(seats, 257));
    ensure(!roles.CanPromoteLeader(seats, 255, 257));
    ensure(roles.CanPromoteLeader(seats, 1, 257));
    ensure(roles.CanRemove(seats, 257, 255));
    ensure(!roles.CanRemove(seats, 255, 257));
}
template<> template<> void lobby_authority_test::test<3>() {
    set_test_name("own-team permission cannot edit another player or override natural mode teams");
    ensure(!roles.CanSetTeam(seats, 256, 256, true, true));
    ensure(!roles.CanSetTeam(seats, 256, 256, false, true));
    ensure(roles.CanSetTeam(seats, 1, 257, false, true));
    ensure(!roles.CanSetTeam(seats, 256, 1, false, true));
    ensure(!roles.CanSetTeam(seats, 256, 256, false, false));
    ensure(roles.CanSetTeam(seats, 255, 1, true, false));
}
template<> template<> void lobby_authority_test::test<4>() {
    set_test_name("disconnect removes stale roles and deterministically returns authority to a surviving leader");
    roles.host = 257; roles.leaders[255] = 256;
    seats.erase(seats.begin()+2, seats.end()); roles.Refresh(seats);
    ensure_equals(roles.host, 256u);
    ensure_equals(roles.leaders.size(), size_t(1));
    ensure(!roles.CanSetTeam(seats, 257, 256, false, true));
}
template<> template<> void lobby_authority_test::test<5>() {
    set_test_name("a new peer cannot overwrite a preserved logical player after host migration");
    std::vector<LobbyAuthority::Seat> migrated{{1,255,0},{3,1,0},{257,255,1}};
    LobbyAuthority::Player next = 257;
    ensure_equals(LobbyAuthority::AllocatePlayerID(migrated, 3, 0, next), 258u);
    ensure_equals(next, 259u);
    ensure_equals(LobbyAuthority::AllocatePlayerID(migrated, 4, 0, next), 4u);
    ensure_equals(next, 259u);
}
}
