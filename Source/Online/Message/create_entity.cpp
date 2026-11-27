//-----------------------------------------------------------------------------
//           Name: create_entity.cpp
//      Developer: Wolfire Games LLC
//         Author: Max Danielsson
//    Description: Create entity message, sent by the host to clients to inform
//                 them of construction of a new entity.
//
//                 Clients will request creation of an entity to the host via a
//                 different message.
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
#include "create_entity.h"

#include <Online/online.h>
#include <Online/online_utility.h>
#include <Main/engine.h>
#include <Editors/map_editor.h>
#include <Objects/itemobject.h>

namespace OnlineMessages {
CreateEntity::CreateEntity(const string& path, const vec3& pos, ObjectID entity_object_id, bool stripped_npc, bool staged) : OnlineMessageBase(OnlineMessageCategory::LEVEL_PERSISTENT),
                                                                                             path(path),
                                                                                             pos(pos),
                                                                                             entity_object_id(entity_object_id),
                                                                                             stripped_npc(stripped_npc), staged(staged) {
}

binn* CreateEntity::Serialize(void* object) {
    CreateEntity* ce = static_cast<CreateEntity*>(object);
    binn* l = binn_object();

    binn_object_set_std_string(l, "path", ce->path);
    binn_object_set_vec3(l, "pos", ce->pos);
    binn_object_set_int32(l, "id", ce->entity_object_id);
    if (ce->stripped_npc) binn_object_set_bool(l, "stripped_npc", true);
    if (ce->staged) binn_object_set_bool(l, "staged", true);

    return l;
}

void CreateEntity::Deserialize(void* object, binn* l) {
    CreateEntity* ce = static_cast<CreateEntity*>(object);

    binn_object_get_std_string(l, "path", &ce->path);
    binn_object_get_vec3(l, "pos", &ce->pos);
    binn_object_get_int32(l, "id", &ce->entity_object_id);
    BOOL stripped = false;
    binn_object_get_bool(l, "stripped_npc", &stripped);
    ce->stripped_npc = stripped;
    BOOL staged = false;
    binn_object_get_bool(l, "staged", &staged);
    ce->staged = staged;
}

void CreateEntity::Execute(const OnlineMessageRef& ref, void* object, PeerID peer) {
    CreateEntity* ce = static_cast<CreateEntity*>(object);
    SceneGraph* sg = Engine::Instance()->GetSceneGraph();
    Online* online = Online::Instance();
    if ((ce->stripped_npc || ce->staged) && online->IsHosting()) return;  // host-only reinforcement templates
    if (!OnlineUtility::IsSafeAssetPath(ce->path) || !std::isfinite(ce->pos[0]) || !std::isfinite(ce->pos[1]) || !std::isfinite(ce->pos[2])) {
        return;  // (an object file under Data/, somewhere real)
    }

    if (sg != nullptr) {
        if (sg->map_editor->CreateObjectFromHost(ce->path, ce->pos, ce->entity_object_id, ce->stripped_npc) && ce->staged) {
            // A reliable enable message may land in a later network dispatch.
            // A staged body or weapon must already be dormant on its first frame.
            Object* created = sg->GetObjectFromID(online->GetObjectID(ce->entity_object_id));
            if (created) {
                created->SetEnabled(false);
                if (created->GetType() == _item_object) {
                    auto* item = static_cast<ItemObject*>(created);
                    item->SetPhysicsTransform(item->GetPhysicsTransform());
                }
            }
        }
    }
}

void* CreateEntity::Construct(void* mem) {
    return new (mem) CreateEntity("", vec3(), -1);
}

void CreateEntity::Destroy(void* object) {
    CreateEntity* ce = static_cast<CreateEntity*>(object);
    ce->~CreateEntity();
}
}  // namespace OnlineMessages
