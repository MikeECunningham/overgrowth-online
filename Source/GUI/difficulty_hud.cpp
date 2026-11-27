// Host-view difficulty conditions. Apache-2.0.
#include "difficulty_hud.h"

#include <Editors/map_editor.h>
#include <Asset/Asset/texture.h>
#include <Asset/assetmanager.h>
#include <Game/level.h>
#include <Graphics/graphics.h>
#include <Graphics/local_viewport.h>
#include <Graphics/textures.h>
#include <Internal/filesystem.h>
#include <Internal/config.h>
#include <Main/engine.h>
#include <Main/scenegraph.h>
#include <Online/online.h>
#include <Online/friendly_progression.h>
#include <UserInput/input.h>

#include <imgui.h>
#include <GUI/dimgui/imgui_impl_sdl_gl3.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace DifficultyHUD {
namespace {
Json::Value layout;
unsigned shop_host;
vec2 shop_inset(0.0f);
std::string cached_payload;
Json::Value cached_snapshot;
TextureAssetRef panel_art, heading_art;

void Artwork(ImDrawList* draw, const TextureAssetRef& texture, const ImVec2& a, const ImVec2& b, ImU32 color) {
    if (!texture.valid()) return;
    Textures::Instance()->EnsureInVRAM(texture);
    draw->AddImage(reinterpret_cast<ImTextureID>(Textures::Instance()->ImGuiTextureID(texture)), a, b, ImVec2(0, 0), ImVec2(1, 1), color);
}

Json::Value Rect(float left, float top, float right, float bottom) {
    Json::Value result(Json::arrayValue);
    for (float value : {left, top, right, bottom}) result.append(value);
    return result;
}
}

void BeginFrame() {
    shop_inset = vec2(0.0f);
    layout = Json::Value(Json::objectValue);
    layout["drawn"] = false;
    layout["rows"] = Json::Value(Json::arrayValue);
}

const Json::Value& Inspect() { return layout; }
vec2 ShopInset(unsigned player) { return player == shop_host ? shop_inset : vec2(0.0f); }

void Dispose() {
    panel_art.clear();
    heading_art.clear();
    cached_payload.clear();
    cached_snapshot = Json::Value();
}

void Draw(Engine& engine) {
    Online* online = Online::Instance();
    Graphics* graphics = Graphics::Instance();
    SceneGraph* scene = engine.GetSceneGraph();
    if (!config["show_difficulty_readout"].toBool() || !ImGui::GetCurrentContext() || !engine.IsLevelLoaded() || !scene || !scene->level || !scene->map_editor ||
        scene->map_editor->state_ != MapEditor::kInGame || graphics->media_mode() || engine.waiting_for_input_ ||
        (online->IsActive() && (online->LobbyWaiting() || online->LobbyBuild() || online->BuildSession())) ||
        online->CoopCutscene() || online->RemoteCutsceneActive() || scene->level->DialogueCameraControl()) return;

    int local_index = 0, camera = 0, screen = 1;
    PlayerID host = 255;
    int count = 1;
    if (online->IsActive()) {
        bool local_host = false;
        for (const auto& player : online->GetLocalPlayerStates()) {
            if (!online->IsLobbyHost(player.player_id)) continue;
            local_host = true;
            host = player.player_id;
            local_index = static_cast<int>(player.local_index);
            screen = player.screen;
            camera = screen - 1;
            break;
        }
        if (!local_host) return;
        count = static_cast<int>(std::max(1u, online->LocalLobbyPlayerCount()));
    }
    // PlayerHasMenu also reports the progression shop, which replaces the personal pause menus.
    const bool survival_shop = online->IsSurvivalMode() && FriendlyProgression::ShopOpen();
    if ((!survival_shop && engine.PlayerHasMenu(Input::Instance()->GetLocalPlayerInputID(local_index))) ||
        scene->level->HasScriptFocus()) return;
    if (!engine.GetSplitScreen()) { camera = 0; count = 1; }
    const auto view = LocalViewports::ForPlayer(camera, count);
    // ImGui positions use logical window coordinates, including on HiDPI displays.
    const float width = ImGui::GetIO().DisplaySize.x;
    const float height = ImGui::GetIO().DisplaySize.y;
    const ImVec2 top_left(std::round(view.left * width), std::round((1.0f - view.top) * height));
    const ImVec2 bottom_right(std::round(view.right * width), std::round((1.0f - view.bottom) * height));
    const float view_width = bottom_right.x - top_left.x;
    const float view_height = bottom_right.y - top_left.y;
    if (view_width < 180.0f || view_height < 140.0f) return;

    const std::string payload = online->DifficultyHUDSnapshot();
    if (payload != cached_payload) {
        Json::Reader reader;
        Json::Value incoming;
        if (!reader.parse(payload, incoming, false) || !incoming.isObject()) return;
        cached_payload = payload;
        cached_snapshot = incoming;
    }
    if (!cached_snapshot.get("visible", false).asBool() || !cached_snapshot["lines"].isArray()) return;
    std::vector<std::string> rows;
    for (const auto& row : cached_snapshot["lines"])
        if (row.isString() && !row.asString().empty() && rows.size() < 7) rows.push_back(row.asString());
    if (rows.empty()) return;

    ImFont* body = ImGui_ImplSdlGL3_MenuFont(false);
    ImFont* heading = ImGui_ImplSdlGL3_MenuFont(true);
    if (!body) body = ImGui::GetFont();
    if (!heading) heading = body;
    const bool compact = survival_shop && (view_width < 1180.0f || view_height < 320.0f);
    const float scale = std::max(0.9f, std::min(1.25f, view_height / 720.0f));
    const float margin = (compact ? 4.0f : 16.0f) * scale;
    const float padding = (compact ? 5.0f : 14.0f) * scale, gap = (compact ? 2.0f : 6.0f) * scale;
    const float font_size = (compact ? 17.0f : 22.0f) * scale, title_size = (compact ? 19.0f : 30.0f) * scale;
    float panel_width = std::min(540.0f * scale, view_width - 2.0f * margin);
    if (compact) {
        // Leave the equipment lists and controller actions room in small or split viewports.
        char next[96];
        std::snprintf(next, sizeof(next), "Next wave ~%.1fx  |  Pressure %+.1f%%",
                      cached_snapshot["rating"]["next"].asDouble(), 100.0 * cached_snapshot["progression"]["pressure"].asDouble());
        const auto title = rows.front();
        panel_width = view_width - 2 * margin;
        const auto summary = title + "  |  " + next;
        if (heading->CalcTextSizeA(title_size, FLT_MAX, 0, summary.c_str()).x <= panel_width - 2 * padding)
            rows = {summary};
        else
            rows = {title, next};
    } else if (survival_shop) {
        panel_width = std::min(panel_width, view_width * 0.32f);
    }
    const float text_width = panel_width - 2.0f * padding;
    std::vector<float> row_heights;
    float panel_height = 2.0f * padding;
    for (size_t i = 0; i < rows.size(); ++i) {
        ImFont* font = i == 0 ? heading : body;
        const float size = i == 0 ? title_size : font_size;
        const float row_height = font->CalcTextSizeA(size, FLT_MAX, text_width, rows[i].c_str()).y;
        row_heights.push_back(row_height);
        panel_height += row_height + (i + 1 < rows.size() ? gap : 0.0f);
    }
    panel_height = std::min(panel_height, view_height - 2.0f * margin);
    const ImVec2 at(bottom_right.x - margin - panel_width,
                    compact ? bottom_right.y - margin - panel_height : top_left.y + margin);
    const ImVec2 end(at.x + panel_width, at.y + panel_height);

    ImGui::SetNextWindowPos(at, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panel_width, panel_height), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground;
    if (ImGui::Begin("##host_difficulty_conditions", nullptr, flags)) {
        if (survival_shop) {
            shop_host = host;
            shop_inset = compact ? vec2(0.0f, panel_height + 2 * margin) : vec2(panel_width + 2 * margin, 0.0f);
            const ImVec2 dock_min(compact ? top_left.x : bottom_right.x - shop_inset.x(),
                                  compact ? bottom_right.y - shop_inset.y() : top_left.y);
            ImGui::GetBackgroundDrawList()->AddRectFilled(dock_min, bottom_right, IM_COL32(24, 21, 19, 249));
            layout["shop_inset"] = Json::Value(Json::arrayValue);
            layout["shop_inset"].append(shop_inset.x());
            layout["shop_inset"].append(shop_inset.y());
        }
        const uint32_t art_flags = PX_NOMIPMAP | PX_NOCONVERT | PX_NOREDUCE;
        if (!panel_art.valid() && FindFilePath("Data/Textures/ui/menus/main/black_square.png", kAnyPath, false).isValid())
            panel_art = engine.GetAssetManager()->LoadSync<TextureAsset>("Data/Textures/ui/menus/main/black_square.png", art_flags, 0);
        if (!heading_art.valid() && FindFilePath("Data/Textures/ui/menus/main/brushStroke.png", kAnyPath, false).isValid())
            heading_art = engine.GetAssetManager()->LoadSync<TextureAsset>("Data/Textures/ui/menus/main/brushStroke.png", art_flags, 0);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(top_left, bottom_right, true);
        if (panel_art.valid()) Artwork(draw, panel_art, at, end, IM_COL32(255, 255, 255, 215));
        else draw->AddRectFilled(at, end, IM_COL32(0, 0, 0, 215));
        Artwork(draw, heading_art, at, ImVec2(end.x, at.y + row_heights.front() + 2 * padding), IM_COL32_WHITE);
        float y = at.y + padding;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (y + row_heights[i] > end.y - padding + 0.5f) break;
            ImFont* font = i == 0 ? heading : body;
            const float size = i == 0 ? title_size : font_size;
            const ImU32 color = i == 0 ? IM_COL32(235, 235, 235, 255) : IM_COL32(225, 225, 225, 255);
            draw->AddText(font, size, ImVec2(at.x + padding, y), color, rows[i].c_str(), nullptr, text_width);
            layout["rows"].append(rows[i]);
            y += row_heights[i] + gap;
        }
        draw->PopClipRect();
        layout["drawn"] = true;
        layout["host_player"] = host;
        layout["local_index"] = local_index;
        layout["camera"] = camera;
        layout["screen"] = screen;
        layout["viewport"] = Rect(top_left.x, top_left.y, bottom_right.x, bottom_right.y);
        layout["rect"] = Rect(at.x, at.y, end.x, end.y);
        layout["body_font_size"] = font_size;
        layout["heading_font_size"] = title_size;
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}
}
