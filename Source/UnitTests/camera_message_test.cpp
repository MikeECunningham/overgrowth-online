// License: Apache-2.0.
#include <Online/Message/camera_transform_message.h>
#include <Online/Message/pcs_session_parameters_message.h>
#include <tut/tut.hpp>

#include <cstring>

namespace tut {
struct camera_message_data {};
typedef test_group<camera_message_data> camera_message_group;
camera_message_group camera_message_tests("CameraMessage");
typedef camera_message_group::object camera_test;

template<> template<> void camera_test::test<1>() {
    using OnlineMessages::CameraTransformMessage;
    const vec3 pos(-128.25f, 3.5f, 7.125f), flat(0.75f, 0, -0.5f);
    const vec3 facing(0.625f, -0.25f, -0.875f), up(0.125f, 0.9375f, -0.375f);
    CameraTransformMessage original(pos, flat, facing, up, -23.5f, 269.75f, 513);
    CameraTransformMessage compact(pos, flat, facing, up, -23.5f, 269.75f, 513, true);
    binn* legacy_wire = CameraTransformMessage::Serialize(&original);
    binn* compact_wire = CameraTransformMessage::Serialize(&compact);
    ensure("compact camera is smaller", binn_size(compact_wire) < binn_size(legacy_wire));
    for (binn* wire : {legacy_wire, compact_wire}) {
        CameraTransformMessage decoded(vec3(0), vec3(0), vec3(0), vec3(0), 0, 0);
        CameraTransformMessage::Deserialize(&decoded, wire);
        binn* canonical = CameraTransformMessage::Serialize(&decoded);
        const bool identical = binn_size(canonical) == binn_size(legacy_wire) &&
            std::memcmp(binn_ptr(canonical), binn_ptr(legacy_wire), binn_size(legacy_wire)) == 0;
        binn_free(canonical);
        ensure("all camera fields and split-screen player ID survive exactly", identical);
    }
    binn_free(compact_wire);
    binn_free(legacy_wire);
}

template<> template<> void camera_test::test<2>() {
    using OnlineMessages::PCSSessionParametersMessage;
    PCSSessionParametersMessage parameters({});
    binn* advertised = PCSSessionParametersMessage::Serialize(&parameters);
    ensure("new host advertises compact camera", binn_object_bool(advertised, "compact_camera"));
    binn_free(advertised);
    binn* old_host = binn_object();
    binn* bindings = binn_list();
    binn_object_set_list(old_host, "bindings", bindings);
    binn_free(bindings);
    PCSSessionParametersMessage::Deserialize(&parameters, old_host);
    binn_free(old_host);
    binn* parsed = PCSSessionParametersMessage::Serialize(&parameters);
    ensure("old host retains legacy camera encoding", !binn_object_bool(parsed, "compact_camera"));
    binn_free(parsed);
}
}  // namespace tut
