// Optional, duplicate-safe mode progression. Apache-2.0.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace ChallengeScaling {
enum Rate { Off = 0, Gentle = 1, Standard = 2, Brisk = 3 };
constexpr double kPressureCap = 1.5;

struct Settings {
    int time_rate = Off;
    int round_rate = Off;  // level or completed match; caller gates mode support
    int win_rate = Off;
    int death_rate = Off;  // enemy pressure added per qualifying player down/death
    bool adaptive = false;  // replaces death pressure with a discount on the next win
};

struct EncounterRecord {
    uint32_t deaths = 0;  // retained through failed attempts of this encounter
    bool won = false;
    double win_award = 0;  // frozen when credited, never repriced by late events
};

struct Snapshot {
    double active_seconds = 0;
    double pressure = 0;
    std::set<std::string> completions;
    std::set<std::string> deaths;
    std::map<std::string, EncounterRecord> encounters;
};

struct Factors {
    float vitality = 1;
    float damage = 1;
    float armor = 1;
    float ai = 0;
};

double RateMultiplier(int rate);
Factors Evaluate(double pressure);

class State {
public:
    // dt is active gameplay only: the caller excludes pause/loading/cutscenes.
    bool Advance(double seconds, const Settings& settings);
    bool Completion(const std::string& event_id, const Settings& settings);
    // Caller emits one ID per human down/death, not another for KO -> death.
    bool Death(const std::string& event_id, const std::string& encounter_id, const Settings& settings);
    bool Win(const std::string& encounter_id, const Settings& settings);
    void Clear();
    Snapshot Capture() const { return snapshot_; }
    const Snapshot& Data() const { return snapshot_; }
    bool Restore(const Snapshot& snapshot);
    Factors Evaluate() const { return ChallengeScaling::Evaluate(snapshot_.pressure); }

private:
    Snapshot snapshot_;
};
}
