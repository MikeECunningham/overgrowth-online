// Independent pause/settings script contexts for local players.
// Copyright 2026. Licensed under the Apache License, Version 2.0.
#pragma once

#include <GUI/player_menu_context.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class Engine;

class LocalPlayerMenus {
   public:
    explicit LocalPlayerMenus(Engine& engine);
    ~LocalPlayerMenus();

    bool Toggle(int input_id, bool pause_with_menu = false);
    bool IsOpen(int input_id) const;
    bool AnyOpen() const;
    bool PausesWorld() const;
    bool Execute(unsigned local_index, const std::string& code);
    void Update();
    void Draw();
    void Clear();
    void ResolveCursor();

   private:
    struct Menu;
    Engine& engine_;
    std::map<unsigned, std::unique_ptr<Menu>> menus_;
    std::vector<std::pair<unsigned, std::string>> pending_;
    bool last_pause_ = false;
    // Arm on a close/retry, then inspect only a short period of normal play.
    // Loading, cutscenes and explicit shared pauses defer the inspection.
    std::map<unsigned, double> resume_checks_;
    double next_resume_check_ms_ = 0.0;

    PlayerMenuUI::Context ContextFor(unsigned local_index) const;
    static void Callback(void* instance, const std::string& message);
    void Close(unsigned local_index);
    void ApplyCommands();
    void RefreshPause();
    void UpdateResumeChecks();
};
