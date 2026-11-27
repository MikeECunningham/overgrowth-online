// Copyright 2026. Licensed under the Apache License, Version 2.0.
#include "local_player_menus.h"

#include <GUI/scriptable_ui.h>
#include <Editors/map_editor.h>
#include <Game/level.h>
#include <Graphics/camera.h>
#include <Graphics/local_viewport.h>
#include <Internal/filesystem.h>
#include <Main/engine.h>
#include <Objects/movementobject.h>
#include <Online/online.h>
#include <Online/online_utility.h>
#include <Logging/logdata.h>
#include <UserInput/input.h>

#include <algorithm>

struct LocalPlayerMenus::Menu {
    LocalPlayerMenus* manager;
    unsigned local_index;
    std::unique_ptr<ScriptableUI> ui;
    bool shortcut_pause = false;
};

LocalPlayerMenus::LocalPlayerMenus(Engine& engine) : engine_(engine) {}
LocalPlayerMenus::~LocalPlayerMenus() { Clear(); }

PlayerMenuUI::Context LocalPlayerMenus::ContextFor(unsigned local_index) const {
    Input* input = Input::Instance();
    Graphics* graphics = Graphics::Instance();
    int camera_id = static_cast<int>(local_index);
    int count = std::max(std::max(1, engine_.num_avatars), static_cast<int>(input->GetNumLocalPlayers()));
    if (Online::Instance()->IsActive()) {
        count = std::max(1u, Online::Instance()->LocalLobbyPlayerCount());
        for (const auto& player : Online::Instance()->GetLocalPlayerStates()) {
            if (player.local_index == local_index) camera_id = std::max(0, player.screen - 1);
        }
    } else if (engine_.GetSceneGraph() != nullptr) {
        for (auto* object : engine_.GetSceneGraph()->movement_objects_) {
            MovementObject* actor = static_cast<MovementObject*>(object);
            if (actor->controlled && actor->controller_id == input->GetLocalPlayerInputID(local_index)) {
                camera_id = actor->camera_id;
                break;
            }
        }
    }
    if (!engine_.GetSplitScreen()) {
        camera_id = 0;
        count = 1;
    }
    const LocalViewports::Rect rect = LocalViewports::ForPlayer(camera_id, count);
    PlayerMenuUI::Context result;
    result.input_id = input->GetLocalPlayerInputID(local_index);
    result.local_index = static_cast<int>(local_index);
    result.x = static_cast<int>(rect.left * graphics->window_dims[0]);
    result.y = static_cast<int>(rect.bottom * graphics->window_dims[1]);
    result.width = static_cast<int>(rect.right * graphics->window_dims[0]) - result.x;
    result.height = static_cast<int>(rect.top * graphics->window_dims[1]) - result.y;
    result.mouse_enabled = input->LocalKeyboardPlayerIndex() == static_cast<int>(local_index);
    return result;
}

bool LocalPlayerMenus::Toggle(int input_id, bool pause_with_menu) {
    Input* input = Input::Instance();
    const int logical = input->LocalPlayerIndexForInputID(input_id);
    if (logical < 0 || !engine_.IsLevelLoaded()) return false;
    const auto existing = menus_.find(static_cast<unsigned>(logical));
    if (existing != menus_.end()) {
        bool close = false;
        {
            PlayerMenuUI::Scope scope(ContextFor(static_cast<unsigned>(logical)));
            close = existing->second->ui->CanGoBack();
        }
        if (close) pending_.emplace_back(static_cast<unsigned>(logical), "close");
        ApplyCommands();
        return true;
    }
    // A missing custom menu can still use the level's original pause UI.
    const Path script = FindFilePath("Data/Scripts/local_player_menu.as", kDataPaths | kModPaths, false);
    if (!script.isValid()) return false;
    std::unique_ptr<Menu> menu(new Menu{this, static_cast<unsigned>(logical), nullptr});
    menu->ui.reset(new ScriptableUI(menu.get(), &LocalPlayerMenus::Callback));
    const PlayerMenuUI::Context context = ContextFor(menu->local_index);
    if (context.width <= 0 || context.height <= 0) return false;
    ASData data;
    data.scenegraph = engine_.GetSceneGraph();
    data.gui = &engine_.gui;
    {
        PlayerMenuUI::Scope scope(context);
        menu->ui->Initialize(script, data, false, true);
    }
    if (!menu->ui->IsInitialized()) {
        PlayerMenuUI::Scope scope(context);
        menu->ui.reset();
        return false;
    }
    const unsigned local_index = menu->local_index;
    if (pause_with_menu) menu->shortcut_pause = Online::Instance()->RequestPause(local_index, true, true);
    menus_.emplace(local_index, std::move(menu));
    RefreshPause();
    ResolveCursor();
    return true;
}

bool LocalPlayerMenus::IsOpen(int input_id) const {
    const int local = Input::Instance()->LocalPlayerIndexForInputID(input_id);
    return local >= 0 && menus_.count(static_cast<unsigned>(local)) != 0;
}

bool LocalPlayerMenus::AnyOpen() const { return !menus_.empty(); }

bool LocalPlayerMenus::PausesWorld() const {
    const Online* online = Online::Instance();
    return AnyOpen() && (!online->IsActive() || (online->IsHosting() && !online->HasMultipleLobbyHumans())) &&
        Input::Instance()->GetNumLocalPlayers() <= 1;
}

void LocalPlayerMenus::Callback(void* instance, const std::string& message) {
    Menu& menu = *static_cast<Menu*>(instance);
    menu.manager->pending_.emplace_back(menu.local_index, message);
}

void LocalPlayerMenus::RefreshPause() {
    const bool pause = PausesWorld();
    if (pause != last_pause_) {
        last_pause_ = pause;
        engine_.CommitPause();
    }
}

void LocalPlayerMenus::Close(unsigned local_index) {
    auto found = menus_.find(local_index);
    if (found == menus_.end()) return;
    {
        PlayerMenuUI::Scope scope(ContextFor(local_index));
        found->second->ui.reset();
    }
    if (found->second->shortcut_pause) Online::Instance()->RequestPause(local_index, false, true);
    menus_.erase(found);
    resume_checks_[local_index] = 0.0;
    next_resume_check_ms_ = 0.0;
    RefreshPause();
    ResolveCursor();
}

void LocalPlayerMenus::Clear() {
    // Dispose scripts while their owning context and live level still exist.
    pending_.clear();
    while (!menus_.empty()) Close(menus_.begin()->first);
    pending_.clear();
    for (auto& check : resume_checks_) check.second = 0.0;
    next_resume_check_ms_ = 0.0;
}

void LocalPlayerMenus::ApplyCommands() {
    std::vector<std::pair<unsigned, std::string>> commands;
    commands.swap(pending_);
    for (const auto& command : commands) {
        const unsigned local = command.first;
        const std::string& action = command.second;
        if (menus_.count(local) == 0) continue;
        Online* online = Online::Instance();
        const PlayerID player = online->LobbyPlayerIDForLocalIndex(local);
        const bool leader = online->IsLobbyLeader(player);
        const bool host = online->IsLobbyHost(player);
        if (action == "close") {
            Close(local);
        } else if (action == "toggle_pause") {
            if (online->IsActive()) {
                online->RequestPause(local, !engine_.paused);
            } else if (leader) {
                engine_.user_paused = !engine_.paused;
                engine_.CommitPause();
            }
        } else if (host && action == "end_game" && online->IsHosting()) {
            if (online->EndLobbyGame(player)) Clear();
        } else if (leader && action == "main_menu") {
            Clear();
            engine_.ScriptableUICallback("back_to_main_menu");
        } else if (leader && action == "media_mode") {
            Graphics::Instance()->SetMediaMode(true);
            Close(local);
        } else if (host && action == "retry" && !online->IsClient()) {
            Clear();
            // Retry is the host starting a new attempt. Continue deliberately
            // preserves an explicit world pause; Retry resumes the new one.
            if (online->IsHosting()) online->SetTimeFrozen(false);
            engine_.user_paused = engine_.menu_paused = false;
            engine_.CommitPause();
            Level* level = engine_.GetSceneGraph()->level;
            if (online->IsHosting() && level->HasFunction("void OnlineRetry()")) {
                level->Execute("OnlineRetry();");
            } else if (online->IsHosting()) {
                engine_.ScriptableUICallback(engine_.GetLatestLevelPath().GetOriginalPath());
            } else {
                level->Message("reset");
            }
        }
    }
}

void LocalPlayerMenus::Update() {
    std::vector<unsigned> gone;
    for (const auto& menu : menus_) {
        if (menu.first >= Input::Instance()->GetNumLocalPlayers()) {
            gone.push_back(menu.first);
            continue;
        }
        PlayerMenuUI::Scope scope(ContextFor(menu.first));
        menu.second->ui->Update();
        if (menu.second->ui->IsDeleteScheduled()) pending_.emplace_back(menu.first, "close");
    }
    for (unsigned local : gone) Close(local);
    ApplyCommands();
    RefreshPause();
    ResolveCursor();
    UpdateResumeChecks();
}

void LocalPlayerMenus::UpdateResumeChecks() {
    if (resume_checks_.empty()) return;
    Online* online = Online::Instance();
    SceneGraph* scene = engine_.GetSceneGraph();
    if (!engine_.IsLevelLoaded() || engine_.waiting_for_input_ || engine_.paused ||
        !scene || !scene->level || !scene->map_editor || scene->map_editor->state_ != MapEditor::kInGame ||
        (online->IsActive() && (online->LobbyWaiting() || online->LobbyBuild() || online->BuildSession() || online->TimeFrozen())) ||
        online->CoopCutscene() || online->RemoteCutsceneActive() || scene->level->DialogueCameraControl() || scene->level->HasScriptFocus()) {
        for (auto& check : resume_checks_) check.second = 0.0;
        return;
    }
    const double now = OnlineClockMs();
    if (now < next_resume_check_ms_) return;
    next_resume_check_ms_ = now + 100.0;
    Input* input = Input::Instance();
    for (auto it = resume_checks_.begin(); it != resume_checks_.end();) {
        const unsigned local = it->first;
        if (local >= input->GetNumLocalPlayers()) { it = resume_checks_.erase(it); continue; }
        if (menus_.count(local)) { it->second = 0.0; ++it; continue; }
        if (it->second == 0.0) it->second = now + 3000.0;
        if (now > it->second) { it = resume_checks_.erase(it); continue; }
        const int actual = input->GetLocalPlayerInputID(local);
        const PlayerInput* controller = input->GetController(actual);
        if (!controller || !controller->enabled || controller->remote_controlled) { ++it; continue; }
        MovementObject* actor = nullptr;
        for (auto* object : scene->movement_objects_) {
            auto* candidate = static_cast<MovementObject*>(object);
            if (candidate->controlled && candidate->controller_id == actual &&
                (!online->IsActive() || online->OwnPlayerCharacter(candidate->GetID()))) {
                actor = candidate;
                break;
            }
        }
        // Temporary ragdoll ownership, staged opponents and authored static or
        // dialogue poses are intentionally untouched, as are healthy animations.
        if (!actor || !actor->enabled_ || !actor->visible || actor->static_char || !actor->as_context || !actor->rigged_object() ||
            !actor->rigged_object()->animated || !online->IsLocalAvatar(actor->GetID()) ||
            !actor->ASHasVar("knocked_out") || actor->ASGetIntVar("knocked_out") != MovementObject::_awake ||
            !actor->ASHasVar("state") || actor->ASGetIntVar("state") != 0 ||
            (actor->ASHasVar("dialogue_control") && actor->ASGetBoolVar("dialogue_control")) ||
            (actor->ASHasVar("coop_cutscene_hold") && actor->ASGetBoolVar("coop_cutscene_hold")) ||
            !actor->rigged_object()->anim_client.GetCurrAnim().empty()) { ++it; continue; }
        const bool update = actor->HasFunction("void Update(int)");
        const bool idle = actor->HasFunction("void ApplyIdle(float, bool)") && actor->HasFunction("void FixDiscontinuity()");
        const int as_state = actor->as_context->ctx ? actor->as_context->ctx->GetState() : -1;
        LOGW << "Overgrowth Online: empty local player pose after menu/retry: actor " << actor->GetID()
             << ", local " << local << ", input " << actual << ", camera " << actor->camera_id
             << ", owner " << static_cast<int>(online->GetAvatarOwner(actor->GetID()))
             << ", local peer " << static_cast<int>(online->LocalPeerID())
             << ", proxy " << actor->network_proxy << ", controlled " << actor->controlled
             << ", remote " << actor->remote << ", enabled " << actor->enabled_ << ", visible " << actor->visible
             << ", static " << actor->static_char << ", knocked out " << actor->ASGetIntVar("knocked_out")
             << ", state " << actor->ASGetIntVar("state")
             << ", updated " << (actor->ASHasVar("updated") ? actor->ASGetIntVar("updated") : -1)
             << ", script period/counter " << actor->update_script_period << "/" << actor->update_script_counter
             << ", AS state " << as_state
             << ", Update/idle functions " << update << "/" << idle
             << ", control script " << actor->GetCurrentControlScript()
             << ", paused/ready " << engine_.paused << "/" << engine_.waiting_for_input_
             << ", dialogue/hold " << (actor->ASHasVar("dialogue_control") && actor->ASGetBoolVar("dialogue_control"))
             << "/" << (actor->ASHasVar("coop_cutscene_hold") && actor->ASGetBoolVar("coop_cutscene_hold"))
             << ", cutscene/focus " << online->CoopCutscene() << "/" << scene->level->HasScriptFocus()
             << ", menu owner " << engine_.current_menu_player << ", open menus " << menus_.size() << std::endl;
        if (update && idle && as_state != asEXECUTION_SUSPENDED && as_state != asEXECUTION_ACTIVE) {
            // The same non-simulating pose seed used for newly created bots.
            // Ownership is checked above; a peer's carried body cannot be taken.
            if (actor->network_proxy) actor->SetNetworkProxy(false);
            actor->Execute("ApplyIdle(5.0f, true); FixDiscontinuity();");
            LOGI << "Overgrowth Online: local player " << actor->GetID() << " resume pose "
                 << (actor->rigged_object()->anim_client.GetCurrAnim().empty() ? "still uninitialized" : "initialized") << std::endl;
        }
        it = resume_checks_.erase(it);  // one diagnostic/recovery attempt per menu close
    }
}

void LocalPlayerMenus::Draw() {
    Graphics* graphics = Graphics::Instance();
    for (const auto& menu : menus_) {
        const PlayerMenuUI::Context context = ContextFor(menu.first);
        if (context.width <= 0 || context.height <= 0) continue;
        graphics->PushViewport();
        graphics->setViewport(context.x, context.y, context.x + context.width, context.y + context.height);
        {
            PlayerMenuUI::Scope scope(context);
            menu.second->ui->Draw();
        }
        graphics->PopViewport();
    }
    ApplyCommands();
}

bool LocalPlayerMenus::Execute(unsigned local_index, const std::string& code) {
    auto found = menus_.find(local_index);
    if (found == menus_.end()) return false;
    {
        PlayerMenuUI::Scope scope(ContextFor(local_index));
        found->second->ui->as_context->Execute(code);
    }
    ApplyCommands();
    return true;
}

void LocalPlayerMenus::ResolveCursor() {
    if (!engine_.IsLevelLoaded()) return;
    Input::Instance()->SetGrabMouse(engine_.GameplayOwnsMouse());
}
