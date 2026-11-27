// Shared by the host-backed lobby and the local system settings.
vec4 ProfileRarityColor(int rank) {
    if(rank == 1) return vec4(0.35f, 0.82f, 0.39f, 1);
    if(rank == 2) return vec4(0.32f, 0.65f, 1.0f, 1);
    if(rank == 3) return vec4(0.77f, 0.47f, 1.0f, 1);
    if(rank == 4) return vec4(1.0f, 0.68f, 0.23f, 1);
    return vec4(0.75f, 0.75f, 0.75f, 1);
}

bool ProfileOptionUnlocked(JSONValue &in view, const string &in category, const string &in id, int index) {
    if(!view["meta"].asBool() || index == 0) return true;
    JSONValue allowed = view["unlocked"][category];
    for(int i = 0; i < int(allowed.size()); ++i)
        if(allowed[i].asString() == id) return true;
    return false;
}

int ProfileOptionIndex(JSONValue &in options, const string &in id) {
    for(int i = 0; i < int(options.size()); ++i)
        if(options[i]["id"].asString() == id) return i;
    return 0;
}

bool ProfileNameCharacter(int c) {
    return (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || (c >= 48 && c <= 57) || c == 32;
}

bool ProfileNameValid(const string &in name) {
    if(name.length() < 3 || name.length() > 24 || name[0] == 32 || name[name.length()-1] == 32) return false;
    for(uint i=0; i<name.length(); ++i) if(!ProfileNameCharacter(name[i])) return false;
    return true;
}
const string profile_name_hint = "Use 3–24 letters, digits or spaces. Start and end with a letter or digit.";
