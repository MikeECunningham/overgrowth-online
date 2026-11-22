//-----------------------------------------------------------------------------
//           Name: pcs_client_parameters_message.cpp
//      Developer: Wolfire Games LLC
//    Description:
//        License: Read below
//-----------------------------------------------------------------------------
//
//   Copyright 2022 Wolfire Games LLC
//
//   Licensed under the Apache License, Version 2.0 (the "License");
//   you may not use this file except in compliance with the License.
//   You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License.
//
//-----------------------------------------------------------------------------
#include "pcs_client_parameters_message.h"

#include <Online/online.h>
#include <Utility/binn_util.h>
#include <JSON/json.h>

namespace OnlineMessages {
PCSClientParametersMessage::PCSClientParametersMessage(const std::string& player_name, const std::string& enabled_mods_string) : OnlineMessageBase(OnlineMessageCategory::TRANSIENT),
                                                                                                                                 player_name(player_name),
                                                                                                                                 enabled_mods_string(enabled_mods_string) {
    local_names = Online::Instance()->LocalLobbyNames();
    local_screens = Online::Instance()->LocalLobbyScreens();
}

binn* PCSClientParametersMessage::Serialize(void* object) {
    PCSClientParametersMessage* t = static_cast<PCSClientParametersMessage*>(object);
    binn* l = binn_object();

    binn_object_set_std_string(l, "player_name", t->player_name);
    binn_object_set_std_string(l, "enabled_mods_string", t->enabled_mods_string);
    Json::Value names(Json::arrayValue);
    for (const auto& name : t->local_names) names.append(name);
    binn_object_set_std_string(l, "local_names", Json::FastWriter().write(names));
    Json::Value screens(Json::arrayValue);
    for (int screen : t->local_screens) screens.append(screen);
    binn_object_set_std_string(l, "local_screens", Json::FastWriter().write(screens));

    return l;
}

void PCSClientParametersMessage::Deserialize(void* object, binn* l) {
    PCSClientParametersMessage* t = static_cast<PCSClientParametersMessage*>(object);

    binn_object_get_std_string(l, "player_name", &t->player_name);
    binn_object_get_std_string(l, "enabled_mods_string", &t->enabled_mods_string);
    std::string encoded;
    binn_object_get_std_string(l, "local_names", &encoded);
    Json::Value names; Json::Reader reader;
    t->local_names.clear();
    if (reader.parse(encoded, names, false) && names.isArray()) {
        for (const auto& name : names) if (name.isString()) t->local_names.push_back(name.asString());
    }
    encoded.clear();
    binn_object_get_std_string(l, "local_screens", &encoded);
    Json::Value screens;
    t->local_screens.clear();
    if (reader.parse(encoded, screens, false) && screens.isArray()) {
        for (const auto& screen : screens) if (screen.isInt()) t->local_screens.push_back(screen.asInt());
    }
}

void PCSClientParametersMessage::Execute(const OnlineMessageRef& ref, void* object, PeerID from) {
    PCSClientParametersMessage* t = static_cast<PCSClientParametersMessage*>(object);

    assert(Online::Instance()->IsHosting());
    Online::Instance()->online_session->client_connection_manager.ApplyPackage(from, t);
}

void* PCSClientParametersMessage::Construct(void* mem) {
    return new (mem) PCSClientParametersMessage("", "");
}

void PCSClientParametersMessage::Destroy(void* object) {
    PCSClientParametersMessage* t = static_cast<PCSClientParametersMessage*>(object);
    t->~PCSClientParametersMessage();
}
}  // namespace OnlineMessages
