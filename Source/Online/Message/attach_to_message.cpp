//-----------------------------------------------------------------------------
//           Name: attach_to_message.cpp
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
#include "attach_to_message.h"

#include <Main/engine.h>
#include <Online/online.h>
#include <Utility/binn_util.h>

namespace OnlineMessages {
AttachToMessage::AttachToMessage(ObjectID parent_id, ObjectID child_id, uint32_t bone_id, bool attach, bool mirrored,
                               const AttachedEnvObject* exact_attachment, const vec3& scale, int armor_region, bool runtime_item) : OnlineMessageBase(OnlineMessageCategory::LEVEL_PERSISTENT),
                                                                                                                        bone_id(bone_id),
                                                                                                                        attach(attach),
                                                                                                                        mirrored(mirrored) {
    this->parent_id = Online::Instance()->GetOriginalID(parent_id);
    this->child_id = Online::Instance()->GetOriginalID(child_id);
    exact_env = exact_attachment != nullptr;
    env_scale = scale;
    env_region = armor_region;
    this->runtime_item = runtime_item;
    if (exact_attachment) std::copy(std::begin(exact_attachment->bone_connects), std::end(exact_attachment->bone_connects), env_bones.begin());
}

binn* AttachToMessage::Serialize(void* object) {
    AttachToMessage* t = static_cast<AttachToMessage*>(object);
    binn* l = binn_object();

    binn_object_set_int32(l, "parent_id", t->parent_id);
    binn_object_set_int32(l, "child_id", t->child_id);
    binn_object_set_uint32(l, "bone_id", t->bone_id);
    binn_object_set_bool(l, "attach", t->attach);
    binn_object_set_bool(l, "mirrored", t->mirrored);
    if (t->runtime_item) binn_object_set_bool(l, "runtime_item", true);
    if (t->exact_env) {
        binn_object_set_vec3(l, "env_scale", t->env_scale);
        binn_object_set_int32(l, "env_region", t->env_region);
        for (unsigned i = 0; i < kMaxBoneConnects; ++i) {
            const std::string key = "env" + std::to_string(i);
            binn_object_set_int32(l, (key + "b").c_str(), t->env_bones[i].bone_id);
            binn_object_set_int32(l, (key + "n").c_str(), t->env_bones[i].num_connections);
            binn_object_set_mat4(l, (key + "m").c_str(), t->env_bones[i].rel_mat);
        }
    }

    return l;
}

void AttachToMessage::Deserialize(void* object, binn* l) {
    AttachToMessage* t = static_cast<AttachToMessage*>(object);

    binn_object_get_int32(l, "parent_id", &t->parent_id);
    binn_object_get_int32(l, "child_id", &t->child_id);
    binn_object_get_uint32(l, "bone_id", &t->bone_id);

    BOOL attach, mirrored;
    binn_object_get_bool(l, "attach", &attach);
    binn_object_get_bool(l, "mirrored", &mirrored);
    t->attach = attach;
    t->mirrored = mirrored;
    BOOL runtime = false;
    binn_object_get_bool(l, "runtime_item", &runtime);
    t->runtime_item = runtime;
    t->exact_env = binn_object_get_vec3(l, "env_scale", &t->env_scale);
    t->env_region = 0;
    if (t->exact_env) binn_object_get_int32(l, "env_region", &t->env_region);
    if (t->exact_env) for (unsigned i = 0; i < kMaxBoneConnects; ++i) {
        const std::string key = "env" + std::to_string(i);
        if (!binn_object_get_int32(l, (key + "b").c_str(), &t->env_bones[i].bone_id) ||
            !binn_object_get_int32(l, (key + "n").c_str(), &t->env_bones[i].num_connections) ||
            !binn_object_get_mat4(l, (key + "m").c_str(), &t->env_bones[i].rel_mat)) t->env_bones[i].num_connections = -1;
    }
}

void AttachToMessage::Execute(const OnlineMessageRef& ref, void* object, PeerID from) {
    AttachToMessage* t = static_cast<AttachToMessage*>(object);
    ObjectID parent_object_id = Online::Instance()->GetObjectID(t->parent_id);
    ObjectID child_object_id = Online::Instance()->GetObjectID(t->child_id);
    if (!Online::Instance()->PeerMayChange(from, parent_object_id)) {
        return;  // (a guest attaches to and takes from its own character only)
    }

    SceneGraph* graph = Engine::Instance()->GetSceneGraph();
    if (graph == nullptr) return;

    // We need to make sure that we actually know what objects
    // the sender is referencing
    Object* parent = graph->GetObjectFromID(parent_object_id);
    Object* child = graph->GetObjectFromID(child_object_id);

    Online* online = Online::Instance();
    if ((t->exact_env || t->runtime_item) && online->IsHosting()) return;  // reserved host-authored gear; never relay a forged client attachment

    // Overgrowth Online: the host decides who holds an item. A client grabbing
    // one that is already in someone else's hand here gets told who has it.
    if (online->IsHosting() && parent != nullptr && child != nullptr && t->attach &&
        child->GetType() == EntityType::_item_object && parent->GetType() == EntityType::_movement_object) {
        int holder = static_cast<ItemObject*>(child)->HeldByWhom();
        Peer* peer = online->GetPeerFromID(from);
        if (holder != -1 && holder != parent_object_id && peer != nullptr) {
            online->SendTo<OnlineMessages::AttachToMessage>(peer->conn_id, parent_object_id, child_object_id, 0, false, false);
            online->SendTo<OnlineMessages::AttachToMessage>(peer->conn_id, holder, child_object_id, t->bone_id, true, t->mirrored);
            return;
        }
    }

    if (online->IsHosting()) {
        online->RelayFromPeer(ref, from);  // everyone else sees the pickup or the drop too
    }

    // Overgrowth Online: an item goes on a character only. An id this machine
    // doesn't know falls back to a local object with the same number (often
    // not a character): casting that crashed in AvailableItemSlots
    if (parent != nullptr && child != nullptr && child->GetType() == EntityType::_item_object &&
        parent->GetType() != EntityType::_movement_object) {
        LOGW << "Overgrowth Online: item " << child_object_id << " attached to object " << parent_object_id
             << ", not a character here (an unknown id): ignored" << std::endl;
        return;
    }

    if (parent != nullptr && child != nullptr) {
        if (t->exact_env) {
            if (parent->GetType() != _movement_object || child->GetType() != _env_object || online->IsHosting()) return;
            auto* rigged = static_cast<MovementObject*>(parent)->rigged_object();
            int connections = 0;
            if (t->env_region < 0 || t->env_region > 511) return;
            for (int k = 0; k < 3; ++k) if (!std::isfinite(t->env_scale[k]) || std::fabs(t->env_scale[k]) < 0.001f || std::fabs(t->env_scale[k]) > 10) return;
            for (const auto& bone : t->env_bones) {
                if (bone.num_connections < 0 || bone.num_connections > 16 ||
                    (bone.num_connections && (bone.bone_id < 0 || unsigned(bone.bone_id) >= rigged->skeleton_.physics_bones.size()))) return;
                for (int k = 0; k < 16; ++k) if (!std::isfinite(bone.rel_mat[k])) return;
                connections += bone.num_connections;
            }
            if (!connections) return;
            auto found = std::find_if(rigged->children.begin(), rigged->children.end(), [child](const AttachedEnvObject& a) { return a.direct_ptr == child; });
            if (found == rigged->children.end()) { rigged->children.emplace_back(); found = rigged->children.end() - 1; }
            found->direct_ptr = child; found->legacy_obj_id = child->GetID(); found->bone_connection_dirty = false;
            std::copy(t->env_bones.begin(), t->env_bones.end(), std::begin(found->bone_connects));
            child->SetScale(t->env_scale); child->SetParent(parent); child->SetCollisionEnabled(false);
            if (t->env_region) { ScriptParam provenance; provenance.SetInt(t->env_region); child->GetScriptParams()->InsertScriptParam("Campaign Armor Region", provenance); }
            child->exclude_from_save = true; child->exclude_from_undo = true;
            return;
        }
        if (t->attach) {
            child->SetParent(parent);
            if (child->GetType() == EntityType::_item_object) {
                ItemObject* item_object = (ItemObject*)child;
                MovementObject* movement_object = (MovementObject*)parent;
                if (t->runtime_item) {
                    // Runtime loot is not an editor starting-loadout link.
                    // Resetting/rehosting must not take it back from a human.
                    if (t->bone_id != _at_grip && t->bone_id != _at_sheathe) return;
                    movement_object->AttachItemToSlot(item_object->GetID(), static_cast<AttachmentType>(t->bone_id), t->mirrored);
                    movement_object->Execute("HandleEditorAttachment(" + std::to_string(item_object->GetID()) + "," +
                        std::to_string(t->bone_id) + "," + (t->mirrored ? "true" : "false") + ");");
                    return;
                }
                AttachmentSlotList attachment_slots;
                movement_object->rigged_object()->AvailableItemSlots(item_object->item_ref(), &attachment_slots);
                for (auto& slot : attachment_slots) {
                    if (slot.type == (AttachmentType)t->bone_id && slot.mirrored == t->mirrored) {
                        movement_object->AttachItemToSlotEditor(item_object->GetID(), slot.type, slot.mirrored, slot.attachment_ref, true);
                        break;
                    }
                }

            } else if (parent->GetType() == EntityType::_movement_object) {
                MovementObject* mov = (MovementObject*)parent;
                RiggedObject* rigged = mov->rigged_object();
                // this is copied and pasted from the "original code" -rewrite so that it does not default construct inside the vector
                rigged->children.resize(rigged->children.size() + 1);

                // dummy data
                AttachedEnvObject& attached_env_object = rigged->children.back();
                attached_env_object.bone_connection_dirty = true;
                attached_env_object.direct_ptr = child;
                attached_env_object.bone_connects[0].bone_id = t->bone_id;
                attached_env_object.bone_connects[0].num_connections = 1;
            }
        } else {
            if (child->GetType() == EntityType::_item_object) {
                parent->Disconnect(*child, true, false);
            } else {
                parent->ChildLost(child);
            }
        }
    }
}

void* AttachToMessage::Construct(void* mem) {
    return new (mem) AttachToMessage(0, 0, 0, false, false);
}

void AttachToMessage::Destroy(void* object) {
    AttachToMessage* t = static_cast<AttachToMessage*>(object);
    t->~AttachToMessage();
}
}  // namespace OnlineMessages
