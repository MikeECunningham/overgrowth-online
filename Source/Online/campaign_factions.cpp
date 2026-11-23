// Host-derived campaign faction affiliation. Apache-2.0.
#include <Online/campaign_factions.h>
#include <Online/online.h>
#include <Main/engine.h>
#include <Main/scenegraph.h>
#include <Objects/movementobject.h>
#include <Objects/object.h>

struct CampaignFactionState {
    SceneGraph* graph = nullptr;
    std::string level;
    std::string main_tags;
    std::string prefix = "__og_campaign_lobby_";
    std::vector<std::string> enemies;
    bool discovered = false;
    bool main_tags_known = false;
    CampaignFactions::MainAnchor main;
};

namespace {
std::map<uint32_t, int> HumanTeams(const Online* online) {
    std::map<uint32_t, int> result;
    if (online->online_session) for (const auto& entry : online->online_session->player_states)
        if (!entry.second.is_bot) result[entry.first] = entry.second.team;
    return result;
}
}

int Online::CampaignMainLobbyTeam() const {
    const auto humans = HumanTeams(this);
    if (campaign_faction_state && campaign_faction_state->level == level_name)
        return campaign_faction_state->main.Resolve(humans, campaign_main_player_id_);
    const auto main = humans.find(campaign_main_player_id_);
    return main != humans.end() ? main->second : 1;
}
uint32_t Online::CampaignFactionAnchorPlayer() const {
    return campaign_faction_state && campaign_faction_state->level == level_name && campaign_faction_state->main.team > 0
        ? campaign_faction_state->main.player : campaign_main_player_id_;
}

bool Online::CampaignPlayerOnMainTeam(PlayerID player) const {
    if (!online_session) return false;
    const auto found = online_session->player_states.find(player);
    if (found == online_session->player_states.end()) return false;
    return !lobby_force_teams_ || found->second.team == CampaignMainLobbyTeam();
}

void Online::ResetCampaignFactions() { campaign_faction_state.reset(); }

void Online::RefreshCampaignFactions(const std::string& authored_main_tags, bool authored_tags_known) {
    Engine* engine = Engine::Instance();
    SceneGraph* graph = engine->GetSceneGraph();
    if (!online_session) return;
    if (IsClient()) {
        // Only the host has a complete, ordered view of authored actors. A
        // late client's partial scene must never replace its received profiles.
        if (campaign_faction_state && campaign_faction_state->level == level_name && !loading && engine->IsLevelLoaded())
            campaign_faction_state->graph = graph;
        return;
    }
    if (!IsHosting()) return;
    if (!campaign_faction_state || campaign_faction_state->graph != graph || campaign_faction_state->level != level_name) {
        campaign_faction_state = std::make_shared<CampaignFactionState>();
        campaign_faction_state->graph = graph;
        campaign_faction_state->level = level_name;
    }
    auto& state = *campaign_faction_state;
    state.main.Refresh(HumanTeams(this), campaign_main_player_id_);
    const std::string supplied = CampaignFactions::Canonical(authored_main_tags);
    // Record the authored value before an early load-time team override. Empty
    // is a valid authored value, not permission to recapture our own tags later.
    if (authored_tags_known && (!state.main_tags_known || supplied != state.main_tags)) {
        state.main_tags = supplied;
        state.main_tags_known = true;
        state.discovered = false;
    }
    if (!graph || loading || !engine->IsLevelLoaded() || !CoopCampaign()) return;
    if (!state.main_tags_known) {
        Object* main = graph->GetObjectFromID(CampaignMainAvatarID());
        if (!main || main->GetType() != _movement_object) {
            for (Object* object : graph->movement_objects_) {
                auto* actor = static_cast<MovementObject*>(object);
                if (actor->is_player && !actor->created_on_the_fly) { main = actor; break; }
            }
        }
        if (!main || main->GetType() != _movement_object) return;
        const auto* params = main->GetScriptParams();
        if (params->HasParam("Teams")) {
            std::string authored;
            for (const auto& tag : CampaignFactions::Tags(params->GetStringVal("Teams"))) {
                if (tag.compare(0, state.prefix.size(), state.prefix) == 0) continue;
                if (!authored.empty()) authored += ',';
                authored += tag;
            }
            state.main_tags = authored;
        }
        state.main_tags_known = true;
    }
    if (state.discovered) return;
    std::vector<std::string> authored;
    authored.push_back(state.main_tags);
    for (Object* object : graph->movement_objects_) {
        auto* actor = static_cast<MovementObject*>(object);
        const auto* params = actor->GetScriptParams();
        if (actor->is_player || actor->controlled || actor->created_on_the_fly || IsPlayerCharacter(actor->GetID()) ||
            params->HasParam("Campaign Scaling Source") || !params->HasParam("Teams")) continue;
        authored.push_back(params->GetStringVal("Teams"));
    }
    state.enemies = CampaignFactions::EnemyProfiles(state.main_tags, authored);
    state.prefix = CampaignFactions::UnusedPrefix(authored);
    state.discovered = true;
    // The initial roster can precede full scene discovery. Publish the now
    // complete profiles even when no further player/team edit occurs.
    BroadcastLobby();
}

std::string Online::CampaignTeamTags(int lobby_team) const {
    std::set<int> occupied;
    if (online_session) for (const auto& entry : online_session->player_states) occupied.insert(entry.second.team);
    const auto& state = campaign_faction_state;
    const bool current = state && state->level == level_name;
    const auto assignment = CampaignFactions::Assign(CampaignMainLobbyTeam(), occupied,
        current ? state->main_tags : "", current ? state->enemies : std::vector<std::string>(),
        current ? state->prefix : "__og_campaign_lobby_");
    const auto found = assignment.find(lobby_team);
    return found != assignment.end() ? found->second : "__og_campaign_lobby_" + std::to_string(lobby_team);
}

std::string Online::CampaignFactionMainTags() const {
    return campaign_faction_state && campaign_faction_state->level == level_name ? campaign_faction_state->main_tags : "";
}
std::vector<std::string> Online::CampaignEnemyFactionProfiles() const {
    return campaign_faction_state && campaign_faction_state->level == level_name ? campaign_faction_state->enemies : std::vector<std::string>();
}
std::string Online::CampaignFactionPrefix() const {
    return campaign_faction_state && campaign_faction_state->level == level_name ? campaign_faction_state->prefix : "__og_campaign_lobby_";
}
std::string Online::CampaignTeamAffiliation(int lobby_team) const {
    const auto& state = campaign_faction_state;
    if (!lobby_force_teams_ || !state || !state->discovered || state->level != level_name ||
        state->graph != Engine::Instance()->GetSceneGraph() || !Engine::Instance()->IsLevelLoaded()) return "";
    if (lobby_team == CampaignMainLobbyTeam()) return "Main character team";
    std::set<int> occupied;
    if (online_session) for (const auto& entry : online_session->player_states) occupied.insert(entry.second.team);
    size_t profile = 0;
    for (int team : occupied) {
        if (team == CampaignMainLobbyTeam()) continue;
        if (team == lobby_team) {
            if (profile >= state->enemies.size()) return "Wildcard";
            const std::string& tags = state->enemies[profile];
            return "NPC faction " + (tags.size() > 48 ? tags.substr(0, 45) + "..." : tags);
        }
        ++profile;
    }
    return "";
}
void Online::ReceiveCampaignFactions(const std::string& level, const std::string& main_tags,
                                    const std::vector<std::string>& profiles, const std::string& prefix, int main_team, uint32_t anchor_player) {
    if (!IsClient() || level.size() > 4096 || main_tags.size() > 4096 || profiles.size() > 256 ||
        prefix.empty() || prefix.size() > 256 || prefix.find(',') != std::string::npos) return;
    for (const auto& profile : profiles) if (profile.size() > 4096) return;
    auto state = std::make_shared<CampaignFactionState>();
    state->graph = Engine::Instance()->GetSceneGraph();
    state->level = level;
    state->main_tags = CampaignFactions::Canonical(main_tags);
    state->main_tags_known = true;
    state->enemies = CampaignFactions::EnemyProfiles(state->main_tags, profiles);
    state->prefix = prefix;
    state->discovered = true;
    if (main_team > 0) { state->main.team = main_team; state->main.player = anchor_player; }
    campaign_faction_state = state;
}
