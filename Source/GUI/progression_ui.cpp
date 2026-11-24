// Per-seat experience readout and between-round shop. Apache-2.0.
#include "progression_ui.h"

#include <imgui.h>
#include <GUI/dimgui/imgui_impl_sdl_gl3.h>
#include <Asset/assetmanager.h>
#include <Asset/Asset/texture.h>
#include <Graphics/graphics.h>
#include <Graphics/textures.h>
#include <Internal/filesystem.h>
#include <Graphics/local_viewport.h>
#include <Main/engine.h>
#include <Online/friendly_progression.h>
#include <Online/armor_protection.h>
#include <Online/online.h>
#include <UserInput/input.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ProgressionUI {
namespace {
constexpr ImU32 ink = IM_COL32(24, 21, 19, 249);
constexpr ImU32 ivory = IM_COL32(244, 236, 213, 255);
constexpr ImU32 muted = IM_COL32(196, 184, 160, 255);
constexpr ImU32 gold = IM_COL32(249, 201, 111, 255);
constexpr ImU32 green = IM_COL32(133, 225, 140, 255);
constexpr ImU32 purple = IM_COL32(207, 158, 246, 255);

struct Box {
    float x, y, w, h;
    ImVec2 min() const { return {x, y}; }
    ImVec2 max() const { return {x + w, y + h}; }
    bool contains(ImVec2 p) const { return p.x >= x && p.x < x+w && p.y >= y && p.y < y+h; }
};

// Keep the interpolated amounts across level loads. A commit moves XP between
// the two bars even when the server sends both new totals in one snapshot.
struct Wallet {
    bool seen = false;
    double held = 0, pool = 0, held_from = 0, pool_from = 0;
    int held_goal = 0, pool_goal = 0;
    unsigned commit = 0;
    double started = 0, duration = 0.4, strength = 0;
    void update(const Json::Value& view, double now) {
        const double t = std::min(1.0, std::max(0.0, (now-started)/duration));
        const double ease = t*t*(3-2*t);
        held = held_from + (held_goal-held_from)*ease;
        pool = pool_from + (pool_goal-pool_from)*ease;
        const int next_held = view["held"].asInt(), next_pool = view["pooled"].asInt();
        const unsigned next_commit = view["commit_serial"].asUInt();
        if (!seen) {
            seen = true;
            held = held_from = held_goal = next_held;
            pool = pool_from = pool_goal = next_pool;
            commit = next_commit;
            return;
        }
        if (next_held == held_goal && next_pool == pool_goal && next_commit == commit) return;
        double amount = std::max(std::abs(next_held-held), std::abs(next_pool-pool));
        bool transfer = next_commit != commit && view["commit_amount"].asInt() > 0;
        if (transfer) {
            int committed = view["commit_amount"].asInt();
            held = std::max(0, next_held-committed);
            pool = static_cast<double>(next_pool) + committed;
            amount = committed;
        }
        held_from = held; pool_from = pool;
        held_goal = next_held; pool_goal = next_pool; commit = next_commit;
        const double magnitude = std::min(1.0, std::log1p(amount)/8.0);
        duration = transfer ? 0.48 + 0.90*magnitude : 0.18 + 0.38*magnitude;
        strength = 0.06 + 0.18*magnitude;
        started = now;
    }
    float pulse(double now) const {
        double t = std::min(1.0, std::max(0.0, (now-started)/duration));
        return float(1.0 + std::sin(t*3.14159265)*strength);
    }
};

enum class Page { Shop, Loadout, Team };
struct Seat {
    Wallet wallet;
    bool shopping = false, dropdown = false;
    Page page = Page::Shop;
    unsigned target = UINT32_MAX;
    std::string focus, notice;
    double notice_until = 0;
    float scroll = 0;
    int old_dx = 0, old_dy = 0;
    double next_move = 0;
    bool accept_down = false, back_down = false;
    ImVec2 previous_mouse{-10000,-10000};
    void reset_list() { focus.clear(); scroll = 0; dropdown = false; }
};
std::map<uint32_t, Seat> seats;
Json::Value layout;
TextureAssetRef brush;
std::map<std::string, TextureAssetRef, std::less<>> thumbnails;

struct Artwork {
    const char* path;
    const char* abbreviation;
    bool mirror = false, blade = false;
};

Artwork card_art(const std::string& id, int material) {
    if (id == "health") return {nullptr, "VIT"};
    if (id == "damage") return {nullptr, "DMG"};
    if (id == "speed") return {nullptr, "SPD"};
    if (id == "stealth_passive") return {nullptr, "STL"};
    if (id == "stealth_active") return {nullptr, "SNK"};
    // These are the same item and character previews as the stock editor.
    // Left/right pieces share a mesh, so mirror the preview on the right.
    if (material == ArmorProtection::Wood) return {"O2/large_wood_plate","WD"};
    // The base game ships no editor thumbnails for these families. Name the
    // real material instead of showing a dog plate as a chitin/grey purchase.
    if (material == ArmorProtection::Chitin) return {nullptr,"CHI"};
    if (material == ArmorProtection::Grey) return {nullptr,"GRY"};
    static const Artwork armor[] = {
        {"Object/dog_armor_breastplate", "CHS"}, {"Object/dog_armor_backplate","BCK"},
        {"Object/dog_armor_helmet","HLM"}, {"Object/dog_armor_lower_arm","ARM"},
        {"Object/dog_armor_lower_arm","ARM",true}, {"Object/dog_armor_shoulder_b","SHL"},
        {"Object/dog_armor_shoulder_b","SHL",true}, {"Object/dog_armor_hip_plate","THG"},
        {"Object/dog_armor_hip_plate","THG",true}
    };
    if (id.size() == 7 && id.compare(0, 6, "armor_") == 0 && id[6] >= '0' && id[6] <= '8')
        return armor[id[6] - '0'];
    if (id == "dagger") return {"Object/rabbit_knife", "DGR", false, true};
    if (id == "sword") return {"Object/DogSword", "SWD", false, true};
    if (id == "spear") return {"Object/DogSpear", "SPR", false, true};
    if (id == "recruit") return {"Character/turner", "BOT"};
    return {"Character/rabbitguard", "AI"};
}

struct Tier {
    ImU32 surface, accent;
    const char* name;
};
constexpr Tier tiers[] = {
    {IM_COL32(56, 56, 56, 255), IM_COL32(205, 205, 205, 255), "Common"},
    {IM_COL32(30, 74, 47, 255), IM_COL32(136, 221, 146, 255), "Uncommon"},
    {IM_COL32(30, 59, 99, 255), IM_COL32(144, 196, 255, 255), "Rare"},
    {IM_COL32(78, 44, 99, 255), IM_COL32(224, 166, 255, 255), "Epic"},
    {IM_COL32(108, 54, 24, 255), IM_COL32(255, 182, 112, 255), "Legendary"}
};
void paint_heading(ImDrawList* draw, const Box& box) {
    if (!brush.valid()) return;
    draw->AddImage(reinterpret_cast<ImTextureID>(Textures::Instance()->ImGuiTextureID(brush)),
                   box.min(), box.max(), {0,0}, {1,1}, IM_COL32(255,255,255,210));
}
ImFont* font(bool heading) {
    ImFont* f = ImGui_ImplSdlGL3_MenuFont(heading);
    return f ? f : ImGui::GetFont();
}
void label(ImDrawList* draw, const Box& box, const std::string& s, float size, ImU32 color, bool heading = false, bool wrap = false) {
    if (box.w <= 0 || box.h <= 0) return;
    draw->PushClipRect(box.min(), box.max(), true);
    draw->AddText(font(heading), size, box.min(), color, s.c_str(), nullptr, wrap ? box.w : 0);
    draw->PopClipRect();
}
void draw_card_art(ImDrawList* draw, const Box& area, const Artwork& art, ImU32 accent) {
    if (art.path) {
        auto item = thumbnails.find(art.path);
        if (item == thumbnails.end()) {
            item = thumbnails.emplace(art.path, TextureAssetRef()).first;
            const std::string path = std::string("Data/UI/spawner/thumbs/") + art.path + ".png";
            if (FindFilePath(path.c_str(), kAnyPath, false).isValid())
                item->second = Engine::Instance()->GetAssetManager()->LoadSync<TextureAsset>(path, PX_NOMIPMAP|PX_NOCONVERT|PX_NOREDUCE, 0);
        }
        const auto& texture = item->second;
        if (texture.valid()) {
            const ImTextureID image = Textures::Instance()->ImGuiTextureID(texture);
            if (art.blade) {
                // The spawner photographed each blade vertically with wide transparent
                // gutters. A diagonal crop makes its point and hilt readable on a card.
                const float length = std::min(area.w * 0.97f, area.h * 1.58f);
                const ImVec2 center{area.x+area.w/2,area.y+area.h/2};
                const ImVec2 up{length*0.455f,-length*0.207f};
                const ImVec2 side{length*0.052f,length*0.114f};
                auto corner = [&](float vertical, float horizontal) {
                    return ImVec2(center.x+vertical*up.x+horizontal*side.x,center.y+vertical*up.y+horizontal*side.y);
                };
                draw->AddImageQuad(image,corner(1,-1),corner(1,1),corner(-1,1),corner(-1,-1),
                                   {0.375f,0},{0.625f,0},{0.625f,1},{0.375f,1});
                return;
            }
            const float size = std::min(area.w, area.h);
            const float x = area.x + (area.w-size)/2, y = area.y + (area.h-size)/2;
            draw->AddImage(image, {x,y}, {x+size,y+size},
                           {art.mirror ? 1.f : 0.f,0}, {art.mirror ? 0.f : 1.f,1});
            return;
        }
    }
    const float size = std::min(area.h * 0.78f, area.w * 0.36f);
    const ImVec2 text_size = font(true)->CalcTextSizeA(size,10000,0,art.abbreviation);
    label(draw,{area.x+(area.w-text_size.x)/2,area.y+(area.h-text_size.y)/2,area.w,area.h},
          art.abbreviation,size,accent,true);
}
void balance(ImDrawList* draw, const Wallet& wallet, Box box, double now, float scale) {
    draw->AddRectFilled(box.min(), box.max(), IM_COL32(24,21,19,225), 3);
    const float inset = 6*scale, gap = 9*scale;
    float w = (box.w-2*inset-gap)*0.5f;
    float h = 3*scale;
    double largest = std::max({wallet.held_from, wallet.pool_from, double(wallet.held_goal), double(wallet.pool_goal), 1.0});
    double range = 100.0;
    while (range < largest) range *= 2;
    for (int i=0; i<2; ++i) {
        const float x = box.x+inset+i*(w+gap);
        const float y = box.y+box.h-inset-h;
        double amount = i ? wallet.pool : wallet.held;
        ImU32 color = i ? purple : green;
        const float text_size = std::min(18*scale, w*0.14f);
        const char* caption = i ? "Pooled XP" : "Held XP";
        label(draw, {x,box.y+2*scale,w,box.h}, caption, text_size, color);
        const float prefix = font(false)->CalcTextSizeA(text_size, 10000, 0, caption).x + 5*scale;
        label(draw, {x+prefix,box.y+2*scale,w-prefix,box.h}, std::to_string(std::llround(amount)), text_size*wallet.pulse(now), color, true);
        draw->AddRectFilled({x,y}, {x+w,y+h}, IM_COL32(83,75,77,255));
        draw->AddRectFilled({x,y}, {x+w*float(std::min(1.0,amount/range)),y+h}, color);
    }
}
float input_depth(const PlayerInput* input, const char* key) {
    if (!input || !input->enabled || input->remote_controlled) return 0;
    auto it = input->key_down.find(key);
    return it != input->key_down.end() && it->second.count > 0 ? it->second.depth : 0.0f;
}

enum class Action { TargetMenu, Target, PageShop, PageLoadout, PageTeam, Buy, Equip, Leader, Ready, Loop };
struct Button {
    std::string id, title, detail, badge;
    Action action;
    Box box;
    bool active = true, in_list = false;
    uint32_t target = UINT32_MAX;
    unsigned rarity = 0;
    int armor_material = 0;
};
struct Panel {
    Box bounds, list, wallet, heading, note;
    float scale, total = 0;
    std::vector<Button> buttons;
};

std::string target_name(const Json::Value& rows, uint32_t id, const std::string& fallback) {
    for (const auto& row : rows)
        if (row["id"].asUInt() == id) return row["name"].asString();
    return fallback;
}

Panel build_panel(Seat& seat, Box area, const PlayerState& player, const Json::Value& v) {
    Panel p{};
    p.bounds = area;
    p.scale = std::max(0.48f, std::min({1.22f, area.w/490.0f, area.h/375.0f}));
    const float s = p.scale, pad = 12*s, gap = 6*s, line = 30*s;
    const float x = area.x+pad, w = area.w-2*pad;
    float y = area.y+pad;
    p.heading = {x,y,w,line}; y += line+gap;
    p.wallet = {x,y,w,33*s}; y += p.wallet.h+gap;
    const bool can_buy = v["can_buy"].asBool();
    const bool shared = v["shared"].asBool();
    if (!can_buy && seat.page == Page::Shop) seat.page = Page::Loadout;
    const auto& targets = v["targets"];
    const std::string name = target_name(targets, seat.target, player.playername);
    p.buttons.push_back({"target", name+"   v", "", "", Action::TargetMenu,
                         {x,y,w,line}, targets.size() > 1});
    y += line+gap;
    int tabs = (can_buy ? 1 : 0) + 1 + (shared ? 1 : 0);
    float tw = (w-gap*(tabs-1))/tabs, tx = x;
    auto tab = [&](Action action, const char* key, const char* title, Page page) {
        p.buttons.push_back({key,title,"",seat.page == page ? "selected" : "",action,{tx,y,tw,line}});
        tx += tw+gap;
    };
    if (can_buy) tab(Action::PageShop, "shop", "Upgrades", Page::Shop);
    tab(Action::PageLoadout, "loadout", "Equipment", Page::Loadout);
    if (shared) tab(Action::PageTeam, "team", "Team", Page::Team);
    y += line+gap;
    const float bottom = area.y+area.h-pad;
    const float footer_width = v["final"].asBool() && Online::Instance()->IsLobbyHost(player.player_id) ? (w-gap)*0.5f : w;
    p.buttons.push_back({"ready",v["ready"].asBool() ? "Ready  [x]" : "Ready", "", "", Action::Ready,
                         {x+w-footer_width,bottom-line,footer_width,line}});
    if (footer_width < w)
        p.buttons.push_back({"loop", v["loop"].asBool() ? "Loop campaign  [x]" : "Loop campaign  [ ]", "", "", Action::Loop,
                             {x,bottom-line,footer_width,line}});
    p.note = {x,bottom-line-gap-35*s,w,35*s};
    p.list = {x,y,w,std::max(1.0f,p.note.y-gap-y)};
    float ly = 0;
    auto row = [&](Action action, const std::string& key, const std::string& title,
                   bool active, uint32_t target = UINT32_MAX, const std::string& detail = "") {
        p.buttons.push_back({key,title,detail,"",action,{x,y+ly,w,line},active,true,target});
        ly += line+gap;
    };
    if (seat.dropdown) {
        for (const auto& tgt : targets) {
            uint32_t id = tgt["id"].asUInt();
            row(Action::Target,"target_"+std::to_string(id),tgt["name"].asString()+(tgt["bot"].asBool()?"  (bot)":""),true,id);
        }
    } else if (seat.page == Page::Shop) {
        const int columns = std::max(1, static_cast<int>((w+gap)/(141*s+gap)));
        const float size = (w-(columns-1)*gap)/columns;
        int index = 0;
        for (const auto& card : v["cards"]) {
            const auto col = index%columns, r = index/columns;
            p.buttons.push_back({card["id"].asString(),card["label"].asString(),card["description"].asString(),
                                 std::to_string(card["cost"].asInt())+" XP",Action::Buy,
                                 {x+col*(size+gap),y+r*(size+gap),size,size},card["available"].asBool(),true,UINT32_MAX,
                                 static_cast<unsigned>(std::max(0,std::min(4,card["rarity"].asInt()))), card["armor_material"].asInt()});
            ++index;
        }
        ly = ((index+columns-1)/columns)*(size+gap);
    } else if (seat.page == Page::Loadout) {
        int equipped = v["big_weapon"].asInt();
        const bool edit = v["can_equip"].asBool();
        row(Action::Equip,"none",equipped == 0 ? "(*)  No main weapon" : "( )  No main weapon",edit);
        if (v["owned_sword"].asBool()) row(Action::Equip,"sword",equipped == 1 ? "(*)  Sword" : "( )  Sword",edit);
        if (v["owned_spear"].asBool()) row(Action::Equip,"spear",equipped == 2 ? "(*)  Spear" : "( )  Spear",edit);
        if (v["owned_dagger"].asBool()) row(Action::Equip,"dagger",v["dagger"].asBool() ? "[x]  Offhand dagger" : "[ ]  Offhand dagger",edit);
    } else {
        for (const auto& member : v["teammates"]) {
            const uint32_t id = member["id"].asUInt();
            bool leader = id == v["team_leader"].asUInt();
            row(Action::Leader,"leader_"+std::to_string(id),member["name"].asString()+(leader ? "   (Team leader)" : ""),
                !leader && player.player_id == v["team_leader"].asUInt(), id,
                leader ? "" : "Choose this player as team leader.");
        }
    }
    p.total = std::max(0.f,ly-gap);
    return p;
}

void dispatch(const Button& btn, Seat& s, uint32_t actor, const Json::Value& v, double now) {
    if (!btn.active) return;
    if (btn.action == Action::TargetMenu) { s.dropdown = !s.dropdown; s.scroll = 0; s.focus = s.dropdown ? "target_"+std::to_string(s.target) : "target"; return; }
    if (btn.action == Action::Target) { s.target = btn.target; s.reset_list(); s.focus = "target"; return; }
    if (btn.action == Action::PageShop || btn.action == Action::PageLoadout || btn.action == Action::PageTeam) {
        s.page = btn.action == Action::PageShop ? Page::Shop : btn.action == Action::PageTeam ? Page::Team : Page::Loadout;
        s.reset_list(); s.focus = btn.id; return;
    }
    std::string action, key = btn.id;
    uint32_t target = s.target;
    switch (btn.action) {
        case Action::Buy: action = "buy"; break;
        case Action::Equip: action = "equip"; break;
        case Action::Ready: action = "ready"; key.clear(); break;
        case Action::Loop: action = "loop"; key = v["loop"].asBool() ? "no" : "yes"; break;
        case Action::Leader: action = "leader"; target = btn.target; key.clear(); break;
        default: return;
    }
    if (!FriendlyProgression::Act(actor,target,action,key)) {
        s.notice = "This choice is no longer available.";
        s.notice_until = now+1.8;
    }
}
// Only list items scroll. Navigation through a row can reach partially visible
// cards without ever moving the focus belonging to another local player.
Box screen_box(const Button& b, const Seat& seat) {
    Box rect = b.box;
    if (b.in_list) rect.y -= seat.scroll;
    return rect;
}
bool visible(const Button& b, const Panel& p, const Seat& seat) {
    if (!b.in_list) return true;
    Box box = screen_box(b,seat);
    return box.y < p.list.y+p.list.h && box.y+box.h > p.list.y;
}
const Button* selected(const Panel& p, const std::string& id) {
    for (const auto& b : p.buttons) if (b.id == id) return &b;
    return nullptr;
}
void navigate(const Panel& p, Seat& seat, int dx, int dy) {
    const Button* current = selected(p,seat.focus);
    if (!current) return;
    const Box from = screen_box(*current,seat);
    const Button* best = nullptr;
    float best_cost = std::numeric_limits<float>::max();
    for (const auto& b : p.buttons) {
        if (&b == current) continue;
        Box r = screen_box(b,seat);
        // Treat the clipped list as a single region when entering from a tab
        // or footer. Once inside, allow movement through the entire list.
        if (!current->in_list && !visible(b,p,seat)) continue;
        float x = r.x+r.w/2 - (from.x+from.w/2);
        float y = r.y+r.h/2 - (from.y+from.h/2);
        float along = dx ? dx*x : dy*y;
        float across = std::fabs(dx ? y : x);
        if (along < 1 || across > along*3.5f) continue;
        float cost = along + across*2.25f;
        if (current->in_list && !b.in_list && dy) {
            // Prefer the next/previous list row over a fixed footer/header.
            if ((dy > 0 && b.box.y < p.list.y+p.list.h) || (dy < 0 && b.box.y > p.list.y)) continue;
            cost += 10000;
        }
        if (cost < best_cost) { best = &b; best_cost = cost; }
    }
    if (!best) return;
    seat.focus = best->id;
    if (best->in_list) {
        float top = best->box.y-p.list.y, bottom = top+best->box.h;
        if (top < seat.scroll) seat.scroll = top;
        else if (bottom > seat.scroll+p.list.h) seat.scroll = std::max(top,bottom-p.list.h);
    }
}
void shop(ImDrawList* draw, const PlayerState& player, const Json::Value& view, Box area, Seat& seat,
          const PlayerInput* input, bool mouse_owner, double now, Json::Value& audit) {
    ImGuiIO& io = ImGui::GetIO();
    Panel p = build_panel(seat,area,player,view);
    const float s = p.scale;
    if (!selected(p,seat.focus)) {
        seat.focus = "ready";
        for (const auto& b : p.buttons) if (b.in_list) { seat.focus = b.id; break; }
    }
    const bool hover_mouse = mouse_owner && area.contains(io.MousePos);
    float scroll_axis = input_depth(input,"menu_scroll_down") - input_depth(input,"menu_scroll_up");
    const float previous_scroll = seat.scroll;
    seat.scroll += scroll_axis*std::min(io.DeltaTime,0.1f)*600*s;
    if (hover_mouse && p.list.contains(io.MousePos)) seat.scroll -= io.MouseWheel*50*s;
    seat.scroll = std::max(0.f,std::min(std::max(0.f,p.total-p.list.h),seat.scroll));
    const Button* old_focus = selected(p,seat.focus);
    auto readable = [&](const Button& button) {
        const Box r = screen_box(button,seat);
        const float exposed = std::min(r.y+r.h,p.list.y+p.list.h)-std::max(r.y,p.list.y);
        return exposed >= std::min(r.h,p.list.h)*0.5f;
    };
    if (seat.scroll != previous_scroll && old_focus && old_focus->in_list && !readable(*old_focus)) {
        const float anchor = seat.scroll > previous_scroll ? p.list.y : p.list.y+p.list.h;
        float distance = std::numeric_limits<float>::max();
        for (const auto& b : p.buttons) {
            if (!b.in_list || !readable(b)) continue;
            Box r = screen_box(b,seat);
            float d = std::abs(r.y+r.h/2-anchor) + std::abs(r.x-old_focus->box.x)*2;
            if (d < distance) { distance = d; seat.focus = b.id; }
        }
    }
    auto held = [&](const char* nav, const char* stick) { return std::max(input_depth(input,nav),input_depth(input,stick)) > 0.5f; };
    int dx = int(held("menu_right","right"))-int(held("menu_left","left"));
    int dy = int(held("menu_down","down"))-int(held("menu_up","up"));
    if (dy) dx = 0;
    if ((dx || dy) && (seat.old_dx != dx || seat.old_dy != dy || now >= seat.next_move)) {
        navigate(p,seat,dx,dy);
        seat.next_move = now+(seat.old_dx == dx && seat.old_dy == dy ? 0.115 : 0.32);
    }
    seat.old_dx = dx; seat.old_dy = dy;
    const bool accept = input_depth(input,"menu_return") > 0.5f;
    const bool back = input_depth(input,"menu_cancel") > 0.5f;
    const bool accept_press = accept && !seat.accept_down;
    if (back && !seat.back_down) {
        if (seat.dropdown) { seat.reset_list(); seat.focus = "target"; }
        else seat.focus = "ready";
    }
    seat.accept_down = accept; seat.back_down = back;
    draw->AddRectFilled(area.min(), area.max(), ink);
    draw->AddLine(area.min(), {area.x+area.w,area.y}, gold, std::max(1.f,s));
    std::string title = view["shared"].asBool() && view["team_leader"].asUInt() != player.player_id ? "Outfit" : "Upgrade shop";
    paint_heading(draw,p.heading);
    const int seconds = view["seconds_left"].asInt();
    const std::string clock = seconds >= 0 ? std::to_string(seconds)+"s" : "";
    label(draw,{p.heading.x,p.heading.y,p.heading.w-(clock.empty()?0:52*s),p.heading.h},title+"  /  "+player.playername,25*s,ivory,true);
    if (!clock.empty()) label(draw,{p.heading.x+p.heading.w-49*s,p.heading.y,49*s,p.heading.h},clock,22*s,gold,true);
    balance(draw,seat.wallet,p.wallet,now,s);
    bool mouse_delta = io.MousePos.x != seat.previous_mouse.x || io.MousePos.y != seat.previous_mouse.y;
    seat.previous_mouse = io.MousePos;
    bool clicked = mouse_owner && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const Button* chosen = nullptr;
    for (const auto& b : p.buttons) {
        if (!visible(b,p,seat)) continue;
        Box box = screen_box(b,seat);
        if (b.in_list) draw->PushClipRect(p.list.min(),p.list.max(),true);
        bool hovered = hover_mouse && box.contains(io.MousePos) && (!b.in_list || p.list.contains(io.MousePos));
        if (hovered && (mouse_delta || clicked)) seat.focus = b.id;
        bool focused = seat.focus == b.id;
        const bool card = b.action == Action::Buy;
        const Tier& tier = tiers[b.rarity];
        ImU32 surface = card ? tier.surface : focused ? IM_COL32(86,67,40,255) : IM_COL32(47,41,34,255);
        ImU32 border = focused || b.badge == "selected" ? gold : card ? tier.accent : IM_COL32(97,83,65,255);
        draw->AddRectFilled(box.min(),box.max(),surface,3*s);
        draw->AddRect(box.min(),box.max(),border,3*s,0,focused ? 3*s : s);
        if (card) {
            const float edge = 9*s, content = box.w - 2*edge, art_h = box.h * 0.44f;
            draw_card_art(draw,{box.x+edge,box.y+edge,content,art_h},card_art(b.id, b.armor_material),tier.accent);
            const float title_y = box.y+edge+art_h+4*s, footer_y = box.y+box.h-32*s;
            label(draw,{box.x+edge,title_y,content,footer_y-title_y},b.title,21*s,ivory,true,true);
            draw->AddLine({box.x+edge,footer_y},{box.x+box.w-edge,footer_y},tier.accent);
            const float badge_size = 16*s;
            const float cost_w = font(true)->CalcTextSizeA(badge_size,10000,0,b.badge.c_str()).x;
            label(draw,{box.x+edge,footer_y+5*s,content,24*s},b.badge,badge_size,
                  b.active ? IM_COL32(208,255,174,255) : ivory,true);
            const float tier_size = std::min(13*s,(content-cost_w-4*s)/std::max(1.f,font(false)->CalcTextSizeA(1,10000,0,tier.name).x));
            const float tier_w = font(false)->CalcTextSizeA(tier_size,10000,0,tier.name).x;
            if (tier_size >= 10*s)
                label(draw,{box.x+box.w-edge-tier_w,footer_y+6*s,tier_w+1,24*s},tier.name,tier_size,tier.accent);
        } else {
            const float edge = 6*s;
            label(draw,{box.x+edge,box.y+edge*0.7f,box.w-2*edge,box.h-edge*0.6f},b.title,18*s,b.active ? ivory : muted);
        }
        if (b.in_list) draw->PopClipRect();
        if (b.active && ((clicked && hovered) || (accept_press && focused))) chosen = &b;
    }
    if (p.total > p.list.h) {
        const float thumb = std::max(8*s,p.list.h*p.list.h/p.total);
        const float top = p.list.y+seat.scroll/(p.total-p.list.h)*(p.list.h-thumb);
        draw->AddRectFilled({p.list.x+p.list.w-3*s,top},{p.list.x+p.list.w,top+thumb},gold);
    }
    const Button* focus = selected(p,seat.focus);
    const std::string note = seat.notice_until > now ? seat.notice : focus ? focus->detail : "";
    label(draw,p.note,note,16*s,muted,false,true);
    audit["focus"] = seat.focus;
    audit["target"] = seat.target;
    audit["scroll"] = seat.scroll;
    audit["page"] = seat.page == Page::Shop ? "shop" : seat.page == Page::Loadout ? "equipment" : "team";
    if (chosen) dispatch(*chosen,seat,player.player_id,view,now);
}
}
void Clear() {
    seats.clear();
    brush.clear();
    thumbnails.clear();
    layout = Json::Value(Json::arrayValue);
}
const Json::Value& Inspect() { return layout; }

void Draw(Engine& engine) {
    layout = Json::Value(Json::arrayValue);
    Online* online = Online::Instance();
    if (!FriendlyProgression::Enabled() || !online->IsActive() || online->LobbyWaiting()) { Clear(); return; }
    if (!engine.IsLevelLoaded() || !ImGui::GetCurrentContext() || Graphics::Instance()->media_mode()) return;
    if (FriendlyProgression::ShopOpen()) {
        if (!brush.valid() && FindFilePath("Data/Textures/ui/menus/main/brushStroke.png", kAnyPath, false).isValid())
            brush = engine.GetAssetManager()->LoadSync<TextureAsset>("Data/Textures/ui/menus/main/brushStroke.png", PX_NOMIPMAP|PX_NOCONVERT|PX_NOREDUCE, 0);
        if (brush.valid()) Textures::Instance()->EnsureInVRAM(brush);
    }
    const auto display = ImGui::GetIO().DisplaySize;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const double now = ImGui::GetTime();
    Input* devices = Input::Instance();
    const unsigned count = std::max(1u,online->LocalLobbyPlayerCount());
    std::set<uint32_t> present;
    for (const auto& player : online->GetLocalPlayerStates()) {
        if (player.is_bot) continue;
        present.insert(player.player_id);
        Seat& seat = seats[player.player_id];
        if (seat.target == UINT32_MAX) seat.target = player.player_id;
        auto view = FriendlyProgression::View(player.player_id,seat.target);
        if (!view["enabled"].asBool()) continue;
        bool open = view["shop_open"].asBool();
        const auto vp = LocalViewports::ForPlayer(std::max(0,player.screen-1),count);
        const Box area{vp.left*display.x,(1-vp.top)*display.y,(vp.right-vp.left)*display.x,(vp.top-vp.bottom)*display.y};
        if (area.w < 1 || area.h < 1) continue;
        const int actual = devices->GetLocalPlayerInputID(player.local_index);
        const PlayerInput* input = devices->GetController(actual);
        seat.wallet.update(view,now);
        if (open && !seat.shopping) {
            seat.target = player.player_id;
            seat.page = view["can_buy"].asBool() ? Page::Shop : Page::Loadout;
            seat.reset_list();
            seat.accept_down = input_depth(input,"menu_return") > 0.5f;
            seat.back_down = input_depth(input,"menu_cancel") > 0.5f;
        }
        seat.shopping = open;
        Json::Value audit;
        audit["actor"] = player.player_id;
        audit["screen"] = player.screen;
        audit["shop"] = open;
        audit["display_held"] = seat.wallet.held;
        audit["display_pooled"] = seat.wallet.pool;
        if (open) {
            bool valid = false;
            for (const auto& target : view["targets"]) valid |= target["id"].asUInt() == seat.target;
            if (!valid) seat.target = player.player_id;
        }
        draw->PushClipRect(area.min(),area.max(),true);
        if (open) shop(draw,player,view,area,seat,input,int(player.local_index) == devices->LocalKeyboardPlayerIndex(),now,audit);
        else if (!engine.waiting_for_input_ && !engine.PlayerHasMenu(actual)) {
            const float s = std::max(0.55f,std::min({1.16f,area.w/420.f,area.h/480.f}));
            const float width = std::min(308*s,std::max(1.f,area.w-24*s));
            balance(draw,seat.wallet,{area.x+area.w-width-12*s,area.y+area.h-46*s,width,34*s},now,s);
        }
        draw->PopClipRect();
        layout.append(audit);
    }
    for (auto it=seats.begin(); it!=seats.end(); ) {
        if (!present.count(it->first)) it = seats.erase(it);
        else ++it;
    }
}
}
