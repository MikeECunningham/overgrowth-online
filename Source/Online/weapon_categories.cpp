#include <Online/weapon_categories.h>

#include <Internal/filesystem.h>
#include <Internal/modloading.h>
#include <Logging/logdata.h>
#include <Online/online_utility.h>
#include <tinyxml.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <set>

namespace WeaponCategories {
namespace {
struct Registry {
    std::vector<Category> categories;
    std::array<std::string, kLegacyCount> legacy;
    std::map<std::string, std::string> old_categories, old_paths;
    std::map<std::string, int> category_selection;
    std::map<std::string, std::pair<size_t, size_t>> by_path;
};
Registry registry;
bool current = false;

std::string Attr(const TiXmlElement& element, const char* key) {
    const char* value = element.Attribute(key);
    return value ? value : "";
}
bool Id(const std::string& id) {
    return !id.empty() && id.size() <= 80 && id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == std::string::npos;
}
bool Asset(const std::string& path) { return OnlineUtility::IsSafeAssetPath(path); }

struct Mechanics {
    std::string model, sharpness;
    bool tangent_model = false;
    std::vector<std::string> fields;
    bool operator==(const Mechanics& other) const {
        return model == other.model && sharpness == other.sharpness &&
               tangent_model == other.tangent_model && fields == other.fields;
    }
};
// Use structure rather than serialized XML so indentation, comments and
// attribute order cannot split reskins. Preserve the order of weapon lines.
void Field(std::string& key, const std::string& part) {
    key += std::to_string(part.size()) + ":" + part;
}
enum class Section { item, points, other };
std::string AttributeValue(const TiXmlElement& element, const TiXmlAttribute& attribute, Section parent) {
    const std::string tag = element.Value(), key = attribute.Name(), raw = attribute.Value();
    const bool hands = parent == Section::item && tag == "grip" && key == "hands";
    const bool range = parent == Section::item && tag == "range" && (key == "extend" || key == "multiply");
    const bool point = parent == Section::points && (key == "x" || key == "y" || key == "z");
    const bool mass = parent == Section::item && tag == "physics" && key == "mass";
    if (!hands && !range && !point && !mass) return raw;
    // Item::Load splits the mass at the first literal space. Keep its entire
    // suffix so different or unrecognized units are never merged.
    const auto unit = mass ? raw.find(' ') : std::string::npos;
    const std::string number = raw.substr(0, unit);
    char* end;
    const double parsed = std::strtod(number.c_str(), &end);
    if (end == number.c_str() || *end || !std::isfinite(parsed)) return "raw:" + raw;
    if (hands) {
        int value;
        if (parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max() ||
            element.QueryIntAttribute(key.c_str(), &value) != TIXML_SUCCESS || value != parsed) return "raw:" + raw;
        return std::to_string(value);
    }
    if (std::abs(parsed) > std::numeric_limits<float>::max()) return "raw:" + raw;
    float value = static_cast<float>(parsed);
    if (!mass && element.QueryFloatAttribute(key.c_str(), &value) != TIXML_SUCCESS) return "raw:" + raw;
    if (!std::isfinite(value)) return "raw:" + raw;
    if (value == 0) value = 0;  // Both signs of zero compare equal in gameplay.
    uint32_t bits;
    static_assert(sizeof(bits) == sizeof(value), "weapon mechanics require a 32-bit float");
    std::memcpy(&bits, &value, sizeof(bits));
    return std::to_string(bits) + (unit == std::string::npos ? "" : raw.substr(unit));
}
std::string ElementKey(const TiXmlElement& element, Section parent = Section::item) {
    std::string key;
    Field(key, element.Value());
    std::map<std::string, std::string> attrs;
    for (auto* a = element.FirstAttribute(); a; a = a->Next()) attrs[a->Name()] = AttributeValue(element, *a, parent);
    for (const auto& attr : attrs) { Field(key, attr.first); Field(key, attr.second); }
    if (!element.FirstChildElement() && element.GetText()) Field(key, element.GetText());
    const auto section = parent == Section::item && std::string(element.Value()) == "points" ? Section::points : Section::other;
    for (auto* child = element.FirstChildElement(); child; child = child->NextSiblingElement())
        Field(key, ElementKey(*child, section));
    return key;
}
struct AvailableAssets {
    std::map<std::string, bool> files;
    bool Has(const std::string& path) {
        auto found = files.emplace(path, false);
        if (found.second) found.first->second = Asset(path) && FindFilePath(path, kAnyPath, false).isValid();
        return found.first->second;
    }
    bool Image(const std::string& path) {
        // Any of these can supply the texture. Choosing the freshest conversion
        // via FindImagePath can read and checksum the full texture unnecessarily.
        return Has(path) || Has(path + "_converted.dds") || Has(path + "_converted.crn");
    }
    bool References(const TiXmlElement& element) {
        auto present = [this](const std::string& val) { return val.compare(0, 5, "Data/") != 0 || Has(val); };
        if (element.GetText() && !present(element.GetText())) return false;
        for (auto* attrib = element.FirstAttribute(); attrib; attrib = attrib->Next())
            if (!present(attrib->Value())) return false;
        for (auto* child = element.FirstChildElement(); child; child = child->NextSiblingElement())
            if (!References(*child)) return false;
        return true;
    }
};
bool ReadMechanics(const std::string& path, Mechanics& result, AvailableAssets& assets) {
    if (!Asset(path)) return false;
    const Path item_file = FindFilePath(path, kAnyPath, false);
    TiXmlDocument item;
    if (!item_file.isValid() || !item.LoadFile(item_file.GetFullPath())) return false;
    const auto* root = item.FirstChildElement("item");
    const auto* type = root ? root->FirstChildElement("type") : nullptr;
    const auto* look = root ? root->FirstChildElement("appearance") : nullptr;
    if (!type || !type->GetText() || std::string(type->GetText()) != "weapon" || !look ||
        type->NextSiblingElement("type") || look->NextSiblingElement("appearance") ||
        !assets.References(*root)) return false;
    // Item::Load visits fields in order. Repeated overrides may win, and
    // multiple <lines> elements append in order even across separate sections.
    for (auto* child = root->FirstChildElement(); child; child = child->NextSiblingElement())
        if (child != look) result.fields.push_back(ElementKey(*child));

    std::string appearance = Attr(*look, "obj_path");
    if (!Asset(appearance)) return false;
    const Path object_file = FindFilePath(appearance, kAnyPath, false);
    TiXmlDocument object;
    if (!object_file.isValid() || !object.LoadFile(object_file.GetFullPath())) return false;
    const auto* obj = object.FirstChildElement("Object");
    if (!obj) return false;
    const auto* model = obj->FirstChildElement("Model");
    const auto* shader = obj->FirstChildElement("ShaderName");
    const auto* sharp = obj->FirstChildElement("SharpnessMap");
    if (!model || !model->GetText() || !shader || !shader->GetText()) return false;
    for (const char* tag : {"ColorMap", "NormalMap", "TranslucencyMap"}) {
        const auto* image = obj->FirstChildElement(tag);
        if (!image && std::string(tag) == "TranslucencyMap") continue;
        if (!image || !image->GetText() || !assets.Image(image->GetText())) return false;
    }
    result.model = model->GetText();
    result.sharpness = sharp && sharp->GetText() ? sharp->GetText() : "";
    result.tangent_model = std::string(shader->GetText()) == "cubemap";
    // Sharpness is sampled on the CPU. ImageSampler can read the original
    // image or its own cache; a GPU texture conversion alone cannot supply it.
    return (result.sharpness.empty() || assets.Has(result.sharpness) ||
            assets.Has(result.sharpness + "_image_sample_cache")) && assets.Has(result.model);
}

const std::set<std::string> prices{"dagger","stick","tool","machete","staff","spear","sword","hammer","broadsword"};
struct CatalogBuilder {
    std::map<std::string, size_t> ids;
    std::set<std::string> paths;
    std::map<std::string, Mechanics> reference;
    AvailableAssets available;

    void Load(const std::string& filename, const std::string& mod, bool extensions) {
        TiXmlDocument doc(filename.c_str());
        if (!doc.LoadFile()) return;
        const auto* root = doc.FirstChildElement("WeaponCategories");
        if (!root) return;
        const bool stock = mod.empty();
        for (auto* node = root->FirstChildElement(); node; node = node->NextSiblingElement()) {
            const std::string tag = node->Value();
            const bool extend = tag == "Extend";
            if (extend != extensions || (!extend && tag != "Category")) continue;
            const std::string id = Attr(*node, "id");
            if (!Id(id) || (!stock && !extend && id.compare(0, mod.size()+1, mod+".") != 0)) continue;
            Category fresh;
            auto known = ids.find(id);
            if (extend && known == ids.end()) continue;
            if (!extend) {
                if (known != ids.end()) continue;
                fresh.id = id;
                fresh.label = Attr(*node, "label");
                fresh.price_family = Attr(*node, "price");
                const std::string hand = Attr(*node, "hand");
                if (fresh.label.empty() || !prices.count(fresh.price_family) || (hand != "off" && hand != "main")) continue;
                fresh.offhand = hand == "off";
                node->QueryIntAttribute("rarity", &fresh.rarity);
                if (fresh.rarity < 0 || fresh.rarity > 4) continue;
            }
            Category& cat = extend ? registry.categories[known->second] : fresh;
            for (auto* entry = node->FirstChildElement("Skin"); entry; entry = entry->NextSiblingElement("Skin")) {
                Skin skin;
                skin.id = Attr(*entry, "id"); skin.label = Attr(*entry, "label");
                skin.path = Attr(*entry, "path"); skin.artwork = Attr(*entry, "artwork");
                entry->QueryIntAttribute("rarity", &skin.rarity);
                if (!Id(skin.id) || skin.label.empty() || !Asset(skin.path) ||
                    (!skin.artwork.empty() && !Asset(skin.artwork)) || skin.rarity < 0 || skin.rarity > 4) continue;
                if (stock) {
                    int old = -1;
                    entry->QueryIntAttribute("legacy", &old);
                    if (old >= 0 && old < kLegacyCount) {
                        registry.legacy[old] = id;
                        registry.old_categories[skin.id] = id;
                        registry.old_paths[skin.id] = skin.path;
                        registry.category_selection.emplace(id, old);
                    }
                }
                if (paths.count(skin.path) || std::any_of(cat.skins.begin(), cat.skins.end(),
                        [&](const Skin& existing) { return existing.id == skin.id; })) continue;
                Mechanics mechanics;
                if (!ReadMechanics(skin.path, mechanics, available)) {
                    LOGW << "Weapon catalog: incomplete item " << skin.path << std::endl;
                    if (cat.skins.empty()) break;
                    continue;
                }
                if (cat.skins.empty()) {
                    skin.rarity = 0;
                    reference[id] = mechanics;
                } else if (!(reference[id] == mechanics)) {
                    LOGW << "Weapon catalog: " << skin.path << " differs in gameplay from " << cat.skins.front().path << std::endl;
                    continue;
                }
                paths.insert(skin.path);
                cat.skins.push_back(std::move(skin));
            }
            if (!extend && !fresh.skins.empty()) {
                ids[id] = registry.categories.size();
                registry.categories.push_back(std::move(fresh));
            }
        }
    }
};

void Ensure() {
    if (current) return;
    registry = {};
    CatalogBuilder builder;
    auto base = FindFilePath("Data/weapon_categories.xml", kDataPaths, false);
    if (base.isValid()) builder.Load(base.GetFullPathStr(), "", false);
    std::map<std::string,std::string> mods;
    for (const auto* mod : ModLoading::Instance().GetMods()) {
        if (mod->IsActive()) mods[mod->id.str()] = AssemblePath(mod->path, "weapon_categories.xml");
    }
    for (bool extensions : {false, true})
        for (const auto& mod : mods) builder.Load(mod.second, mod.first, extensions);
    for (size_t i = 0; i < registry.categories.size(); ++i) {
        auto& skins = registry.categories[i].skins;
        std::stable_sort(skins.begin()+1, skins.end(), [](const Skin& a, const Skin& b) { return a.rarity < b.rarity; });
        for (size_t s = 0; s < skins.size(); ++s) registry.by_path.emplace(skins[s].path, std::make_pair(i,s));
    }
    current = true;
}
}
const std::vector<Category>& Catalog() { Ensure(); return registry.categories; }
const Category* Find(const std::string& id) {
    for (const auto& category : Catalog()) if (category.id == id) return &category;
    return nullptr;
}
const Category* ForItem(const std::string& path, const Skin** skin) {
    Ensure();
    auto it = registry.by_path.find(path);
    if (it == registry.by_path.end()) return nullptr;
    const auto& category = registry.categories[it->second.first];
    if (skin) *skin = &category.skins[it->second.second];
    return &category;
}
const Skin& Appearance(const Category& category, const std::string& skin) {
    for (const Skin& option : category.skins) if (option.id == skin) return option;
    return category.skins.front();
}
void Invalidate() { current = false; }
std::string FromLegacy(int selection) {
    Ensure();
    return selection >= 0 && selection < kLegacyCount ? registry.legacy[selection] : "";
}
int ToLegacy(const std::string& category) {
    Ensure();
    auto it = registry.category_selection.find(category);
    return it == registry.category_selection.end() ? -1 : it->second;
}
std::string OldPurchase(const std::string& key) {
    Ensure();
    auto it = registry.old_categories.find(key);
    return it == registry.old_categories.end() ? key : it->second;
}
std::string OldItemPath(const std::string& key) {
    Ensure();
    auto it = registry.old_paths.find(key);
    return it == registry.old_paths.end() ? "" : it->second;
}
}
