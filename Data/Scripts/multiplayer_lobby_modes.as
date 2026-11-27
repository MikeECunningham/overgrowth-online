// Data behind the lobby's mode cards and map selector. Picking never starts
// hosting or loads a world; the host's Start button owns that transition.
// License: Apache-2.0.

class LobbyMapChoice {
    string path;
    string title;
    string thumbnail;
    LobbyMapChoice(const string &in p, const string &in t, const string &in image) {
        path = p;
        title = t;
        thumbnail = image;
    }
}

class LobbyModeChoice {
    string id;
    string campaign;
    string title;
    string thumbnail;
    bool build = false;
    array<LobbyMapChoice@> maps;
    TextureAssetRef texture;
    bool texture_loaded = false;
}

array<LobbyModeChoice@> lobby_modes;
bool lobby_maps_from_host = false;
const string kLobbyDefaultThumbnail = "Data/Textures/ui/menus/main/custom_level_thumbnail.jpg";
const string kLobbySandboxCampaign = "com-wolfire-sandbox-levels";

bool LobbyCoopCampaign(Campaign &in camp) {
    if(camp.GetID() == "com-wolfire-drika" || camp.GetID() == "com-wolfire-timbles-therium") {
        return true;
    }
    return camp.GetMainScript().findFirst("linear_campaign") >= 0 &&
        camp.GetMenuScript().findFirst("arena") < 0 && camp.GetMenuScript().findFirst("aio_battle") < 0;
}

string LobbyTexturePath(const string &in path) {
    if(path == "") {
        return kLobbyDefaultThumbnail;
    }
    if(path.findFirst("Data/") == 0 || path.findFirst("/") == 0 || path.findFirst(":") == 1) {
        return path;
    }
    return "Data/" + path;
}

string LobbyLevelTitle(const string &in path) {
    int slash = path.findLast("/");
    string name = slash >= 0 ? path.substr(slash + 1) : path;
    int dot = name.findLast(".");
    return dot > 0 ? name.substr(0, dot) : name;
}

string LobbyLevelPath(const string &in path) {
    if(path == "" || path.findFirst("Data/") == 0 || path.findFirst("/") == 0 || path.findFirst(":") == 1) {
        return path;
    }
    return "Data/Levels/" + path;
}

void LobbyOfferMap(LobbyModeChoice@ mode, const string &in path, const string &in title, const string &in thumbnail) {
    string level_path = LobbyLevelPath(path);
    for(uint i = 0; i < mode.maps.length(); ++i) {
        if(mode.maps[i].path == level_path) {
            return;
        }
    }
    mode.maps.insertLast(LobbyMapChoice(level_path, title, LobbyTexturePath(thumbnail)));
}

void LobbyReadModes() {
    lobby_modes.resize(0);
    lobby_maps_from_host = Online_IsHosting();
    LobbyModeChoice@ freeplay = LobbyModeChoice();
    freeplay.id = "freeplay";
    freeplay.title = "Freeplay";
    freeplay.thumbnail = "Data/UI/mainmenu/images/scrubby_hills_fall.png";
    LobbyOfferMap(freeplay, "starters/scrubby_hills_fall.xml", "Scrubby Hills (starter)", freeplay.thumbnail);
    array<Campaign>@ campaigns = GetCampaigns();
    for(uint i = 0; i < campaigns.length(); ++i) {
        Campaign camp = campaigns[i];
        bool arena = camp.GetID() == "com-wolfire-arena";
        bool sandbox = camp.GetID() == kLobbySandboxCampaign;
        if(!LobbyCoopCampaign(camp) && !arena && !sandbox) {
            continue;
        }
        LobbyModeChoice@ mode = LobbyModeChoice();
        mode.id = sandbox ? "sandbox" : camp.GetID();
        mode.campaign = camp.GetID();
        mode.title = arena ? "Arena (PvP)" : camp.GetTitle();
        mode.thumbnail = LobbyTexturePath(camp.GetThumbnail());
        array<ModLevel>@ levels = camp.GetLevels();
        for(uint j = 0; j < levels.length(); ++j) {
            // Stock 1.4 does not mark its arena maps online; this fork supplies
            // their PvP rules, just as it supplies the campaigns' co-op rules.
            LobbyOfferMap(mode, levels[j].GetPath(), levels[j].GetTitle(), levels[j].GetThumbnail());
            if(sandbox) {
                LobbyOfferMap(freeplay, levels[j].GetPath(), levels[j].GetTitle(), levels[j].GetThumbnail());
            }
        }
        if(mode.maps.length() > 0) {
            lobby_modes.insertLast(mode);
        }
    }

    LobbyModeChoice@ build = LobbyModeChoice();
    build.id = "build";
    build.title = "Build a level";
    build.build = true;
    build.thumbnail = kLobbyDefaultThumbnail;
    LobbyOfferMap(build, "Data/Levels/nothing.xml", "Empty level", kLobbyDefaultThumbnail);
    if(lobby_maps_from_host) {
        array<string>@ saved = Online_SavedLevels();
        for(uint i = 0; i < saved.length(); ++i) {
            string suffix = saved[i].findFirst("/autosave/") >= 0 ? " (autosave)" : " (host save)";
            string title = LobbyLevelTitle(saved[i]) + suffix;
            LobbyOfferMap(build, saved[i], title, kLobbyDefaultThumbnail);
            LobbyOfferMap(freeplay, saved[i], title, kLobbyDefaultThumbnail);
        }
    }
    array<ModID>@ active_sids = GetActiveModSids();
    for(uint i = 0; i < active_sids.length(); ++i) {
        array<ModLevel>@ levels = ModGetSingleLevels(active_sids[i]);
        for(uint j = 0; j < levels.length(); ++j) {
            if(levels[j].GetPath().findFirst("overhaul_battles/") < 0) {
                LobbyOfferMap(build, levels[j].GetPath(), levels[j].GetTitle(), levels[j].GetThumbnail());
                LobbyOfferMap(freeplay, levels[j].GetPath(), levels[j].GetTitle(), levels[j].GetThumbnail());
            }
        }
    }
    for(uint i = 0; i < campaigns.length(); ++i) {
        if(campaigns[i].GetMenuScript().findFirst("aio_battle") >= 0) {
            continue;
        }
        array<ModLevel>@ levels = campaigns[i].GetLevels();
        for(uint j = 0; j < levels.length(); ++j) {
            if(levels[j].GetPath().findFirst("overhaul_battles/") < 0) {
                LobbyOfferMap(build, levels[j].GetPath(), campaigns[i].GetTitle() + ": " + levels[j].GetTitle(),
                              levels[j].GetThumbnail());
            }
        }
    }
    if(ModSessionFeature("survival")) {
        LobbyModeChoice@ survival = LobbyModeChoice();
        survival.id = "survival";
        survival.title = "Survival";
        survival.thumbnail = "Data/Images/og-online-survival.jpg";
        survival.maps = freeplay.maps;
        lobby_modes.insertLast(survival);
    }
    lobby_modes.insertLast(freeplay);
    lobby_modes.insertLast(build);
}

int LobbySelectedMode() {
    string current = LobbyGetMode();
    // Older saves of a local standalone game with multiple players have this id.
    if(current == "local" && LobbyGetCampaign() == "") current = "freeplay";
    for(uint i = 0; i < lobby_modes.length(); ++i) {
        if(lobby_modes[i].id == current ||
           (lobby_modes[i].campaign != "" && (lobby_modes[i].campaign == current ||
            (current == "" && lobby_modes[i].campaign == LobbyGetCampaign())))) {
            return int(i);
        }
    }
    return -1;
}

int LobbySelectedMap(LobbyModeChoice@ mode) {
    string current = LobbyLevelPath(LobbyGetMap());
    for(uint i = 0; i < mode.maps.length(); ++i) {
        if(mode.maps[i].path == current) {
            return int(i);
        }
    }
    return -1;
}

bool LobbyChooseMode(int index) {
    if(index < 0 || index >= int(lobby_modes.length()) || lobby_modes[index].maps.length() == 0) {
        return false;
    }
    LobbyModeChoice@ mode = lobby_modes[index];
    return LobbySetSelection(mode.id, mode.campaign, mode.maps[0].path, mode.build);
}

bool LobbyChooseMap(int mode_index, int map_index) {
    if(mode_index < 0 || mode_index >= int(lobby_modes.length())) {
        return false;
    }
    LobbyModeChoice@ mode = lobby_modes[mode_index];
    if(map_index < 0 || map_index >= int(mode.maps.length())) {
        return false;
    }
    return LobbySetSelection(mode.id, mode.campaign, mode.maps[map_index].path, mode.build);
}
