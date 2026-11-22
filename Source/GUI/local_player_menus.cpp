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
#include <UserInput/input.h>

#include <algorithm>

struct LocalPlayerMenus::Menu {
    LocalPlayerMenus* manager;
    unsigned local_index;
    std::unique_ptr<ScriptableUI> ui;
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

bool LocalPlayerMenus::Toggle(int input_id) {
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
    return AnyOpen() && !Online::Instance()->IsActive() && Input::Instance()->GetNumLocalPlayers() <= 1;
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
    menus_.erase(found);
    RefreshPause();
    ResolveCursor();
}

void LocalPlayerMenus::Clear() {
    // Dispose scripts while their owning context and live level still exist.
    pending_.clear();
    while (!menus_.empty()) Close(menus_.begin()->first);
    pending_.clear();
}

void LocalPlayerMenus::ApplyCommands() {
    std::vector<std::pair<unsigned, std::string>> commands;
    commands.swap(pending_);
    for (const auto& command : commands) {
        const unsigned local = command.first;
        const std::string& action = command.second;
        if (menus_.count(local) == 0) continue;
        Online* online = Online::Instance();
        if (action == "close") {
            Close(local);
        } else if (action == "toggle_pause") {
            if (online->IsActive()) {
                online->RequestPause(local, !engine_.paused);
            } else if (local == 0) {
                engine_.user_paused = !engine_.paused;
                engine_.CommitPause();
            }
        } else if (local == 0 && action == "end_game" && online->IsHosting()) {
            if (online->EndLobbyGame()) Clear();
        } else if (local == 0 && action == "main_menu") {
            Clear();
            engine_.ScriptableUICallback("back_to_main_menu");
        } else if (local == 0 && action == "media_mode") {
            Graphics::Instance()->SetMediaMode(true);
            Close(local);
        } else if (local == 0 && action == "retry" && !online->IsClient()) {
            Clear();
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
    SceneGraph* scene = engine_.GetSceneGraph();
    if (scene != nullptr && scene->map_editor != nullptr && scene->map_editor->state_ != MapEditor::kInGame) {
        Input::Instance()->SetGrabMouse(false);
        return;
    }
    const int keyboard = Input::Instance()->LocalKeyboardPlayerIndex();
    const bool keyboard_menu = keyboard >= 0 && menus_.count(static_cast<unsigned>(keyboard)) != 0;
    const bool level_ui = scene != nullptr && scene->level != nullptr && scene->level->HasScriptFocus();
    if (keyboard_menu || level_ui) {
        Input::Instance()->SetGrabMouse(false);
    } else if (engine_.IsLevelLoaded()) {
        Input::Instance()->SetGrabMouse(true);
    }
}
