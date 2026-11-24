// Optional, duplicate-safe mode progression. Apache-2.0.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace ChallengeScaling {
enum Rate { Off = 0, Gentle = 1, Standard = 2, Brisk = 3, Nightmare = 4 };
enum PerformanceMode { PerformanceOff = 0, OnWin = 1, OnLoss = 2, Forced50 = 3 };
constexpr double kPressureCap = 1.5;
constexpr double kRetryReliefCap = 0.25;

struct Settings {
    int time_rate = Off;
    int round_rate = Off;  // level or completed match; caller gates mode support
    int performance_mode = PerformanceOff;
    int performance_rate = Standard;
};

struct EncounterRecord {
    uint32_t wipes = 0;  // full human-party defeats, retained through retries
    double relief = 0;  // Forced50 easing belongs only to this unfinished encounter
    bool won = false;
    double win_award = 0;  // frozen when credited, never repriced by late events
};

struct Snapshot {
    double active_seconds = 0;
    double pressure = 0;
    std::set<std::string> completions;
    std::set<std::string> wipes;
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
double EffectivePressure(const Snapshot& snapshot, const Settings& settings, const std::string& encounter);

// An explicitly conditional forecast: credit the current unfinished encounter
// now; assume no extra elapsed time, defeat or level/match completion.
struct WinPreview {
    double pressure = 0;
    double general_pressure = 0;
    double encounter_relief = 0;
    double next_pressure = 0;
    double award = 0;
    double time_per_second = 0;
    double completion_award = 0;
    double wipe_award = 0;
    uint32_t encounter_wipes = 0;
    bool pending = false;
};
WinPreview PreviewWin(const Snapshot& snapshot, const Settings& settings, const std::string& encounter);

// Only human counts are supplied. A partial revive must not start a new
// attempt, and a migrated latched wipe remains latched until party recovery.
struct WipeTracker {
    bool latched = false;
    double all_awake_since = 0;
    bool Observe(uint32_t humans, uint32_t awake, double now_ms);
};

class State {
public:
    // dt is active gameplay only: the caller excludes pause/loading/cutscenes.
    bool Advance(double seconds, const Settings& settings);
    bool Completion(const std::string& event_id, const Settings& settings);
    // Caller emits one ID per complete human-party defeat, never per actor.
    bool Wipe(const std::string& event_id, const std::string& encounter_id, const Settings& settings);
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
