#include <Online/challenge_scaling.h>

#include <algorithm>
#include <cmath>

namespace ChallengeScaling {
namespace {
constexpr double kTimePerSecond = 0.05 / 300.0;
constexpr double kRoundAward = 0.08;
constexpr double kWinAward = 0.04;
constexpr double kLossAward = 0.05;
constexpr double kForcedWinAward = 0.08;
constexpr double kRetryRelief = 0.025;
constexpr size_t kMaximumEvents = 8192;
constexpr size_t kMaximumKeyBytes = 1024;

bool ValidID(const std::string& id) { return !id.empty() && id.size() <= kMaximumKeyBytes; }
double Bounded(double pressure) { return std::max(0.0, std::min(kPressureCap, pressure)); }
double WinAward(const Settings& settings) {
    return (settings.performance_mode == OnWin ? kWinAward : settings.performance_mode == Forced50 ? kForcedWinAward : 0.0) *
        RateMultiplier(settings.performance_rate);
}
}

double RateMultiplier(int rate) {
    switch (rate) {
        case Gentle: return 0.5;
        case Standard: return 1.0;
        case Brisk: return 1.5;
        case Nightmare: return 2.0;
        default: return 0.0;
    }
}

Factors Evaluate(double pressure) {
    const double amount = std::isfinite(pressure) ? std::max(-kRetryReliefCap, std::min(kPressureCap, pressure)) : 0.0;
    Factors result;
    result.vitality = static_cast<float>(1.0 + amount);
    result.damage = static_cast<float>(1.0 + amount * 0.15);
    // Armor is actual equipment, never an invisible statistical multiplier.
    return result;
}

double EffectivePressure(const Snapshot& snapshot, const Settings& settings, const std::string& encounter) {
    const double general = std::isfinite(snapshot.pressure) ? Bounded(snapshot.pressure) : 0.0;
    const auto record = snapshot.encounters.find(encounter);
    if (settings.performance_mode != Forced50 || record == snapshot.encounters.end() || record->second.won) return general;
    return std::max(-kRetryReliefCap, general - std::min(kRetryReliefCap, record->second.relief));
}

WinPreview PreviewWin(const Snapshot& snapshot, const Settings& settings, const std::string& encounter) {
    WinPreview result;
    result.general_pressure = std::isfinite(snapshot.pressure) ? Bounded(snapshot.pressure) : 0.0;
    result.pressure = EffectivePressure(snapshot, settings, encounter);
    result.encounter_relief = result.general_pressure - result.pressure;
    result.next_pressure = result.pressure;
    result.time_per_second = kTimePerSecond * RateMultiplier(settings.time_rate);
    result.completion_award = kRoundAward * RateMultiplier(settings.round_rate);
    const double rate = RateMultiplier(settings.performance_rate);
    result.wipe_award = settings.performance_mode == OnLoss ? kLossAward * rate : settings.performance_mode == Forced50 ? -kRetryRelief * rate : 0;
    const auto record = snapshot.encounters.find(encounter);
    result.encounter_wipes = record == snapshot.encounters.end() ? 0 : record->second.wipes;
    result.pending = ValidID(encounter) && (record == snapshot.encounters.end() || !record->second.won) &&
        (record != snapshot.encounters.end() || snapshot.encounters.size() < kMaximumEvents);
    if (result.pending) {
        result.award = WinAward(settings);
        // Winning retires this encounter's relief, even if its award is zero.
        result.next_pressure = Bounded(result.general_pressure + result.award);
    }
    return result;
}

bool WipeTracker::Observe(uint32_t humans, uint32_t awake, double now_ms) {
    if (!humans || awake > humans || !std::isfinite(now_ms) || now_ms <= 0) return false;
    if (awake == humans) {
        if (!all_awake_since) all_awake_since = now_ms;
        if (now_ms - all_awake_since >= 350.0) latched = false;
        return false;
    }
    all_awake_since = 0;
    if (awake || latched) return false;
    latched = true;
    return true;
}

bool State::Advance(double seconds, const Settings& settings) {
    if (!std::isfinite(seconds) || seconds <= 0 || !std::isfinite(snapshot_.active_seconds + seconds)) return false;
    snapshot_.active_seconds += seconds;
    snapshot_.pressure = Bounded(snapshot_.pressure + seconds * kTimePerSecond * RateMultiplier(settings.time_rate));
    return true;
}

bool State::Completion(const std::string& event_id, const Settings& settings) {
    if (!ValidID(event_id) || snapshot_.completions.count(event_id) || snapshot_.completions.size() >= kMaximumEvents) return false;
    snapshot_.completions.insert(event_id);
    snapshot_.pressure = Bounded(snapshot_.pressure + kRoundAward * RateMultiplier(settings.round_rate));
    return true;
}

bool State::Wipe(const std::string& event_id, const std::string& encounter_id, const Settings& settings) {
    if (!ValidID(event_id) || (!encounter_id.empty() && !ValidID(encounter_id)) ||
        snapshot_.wipes.count(event_id) || snapshot_.wipes.size() >= kMaximumEvents) return false;
    if (!encounter_id.empty()) {
        const auto previous = snapshot_.encounters.find(encounter_id);
        if (previous == snapshot_.encounters.end() && snapshot_.encounters.size() >= kMaximumEvents) return false;
        EncounterRecord& record = snapshot_.encounters[encounter_id];
        ++record.wipes;
        if (settings.performance_mode == Forced50 && !record.won)
            record.relief = std::min(kRetryReliefCap, record.relief + kRetryRelief * RateMultiplier(settings.performance_rate));
    }
    snapshot_.wipes.insert(event_id);
    if (settings.performance_mode == OnLoss)
        snapshot_.pressure = Bounded(snapshot_.pressure + kLossAward * RateMultiplier(settings.performance_rate));
    return true;
}

bool State::Win(const std::string& encounter_id, const Settings& settings) {
    if (!ValidID(encounter_id)) return false;
    const auto previous = snapshot_.encounters.find(encounter_id);
    if (previous != snapshot_.encounters.end() && previous->second.won) return false;
    if (previous == snapshot_.encounters.end() && snapshot_.encounters.size() >= kMaximumEvents) return false;
    EncounterRecord& record = snapshot_.encounters[encounter_id];
    record.won = true;
    record.relief = 0;
    record.win_award = WinAward(settings);
    snapshot_.pressure = Bounded(snapshot_.pressure + record.win_award);
    return true;
}

void State::Clear() { snapshot_ = Snapshot(); }

bool State::Restore(const Snapshot& snapshot) {
    if (!std::isfinite(snapshot.active_seconds) || snapshot.active_seconds < 0 ||
        !std::isfinite(snapshot.pressure) || snapshot.pressure < 0 || snapshot.pressure > kPressureCap ||
        snapshot.completions.size() > kMaximumEvents || snapshot.wipes.size() > kMaximumEvents ||
        snapshot.encounters.size() > kMaximumEvents) return false;
    for (const auto& id : snapshot.completions) if (!ValidID(id)) return false;
    for (const auto& id : snapshot.wipes) if (!ValidID(id)) return false;
    uint64_t recorded_wipes = 0;
    for (const auto& entry : snapshot.encounters) {
        const EncounterRecord& encounter = entry.second;
        if (!ValidID(entry.first) || !std::isfinite(encounter.win_award) || encounter.win_award < 0 ||
            encounter.win_award > kForcedWinAward * RateMultiplier(Nightmare) + 1e-9 || (!encounter.won && encounter.win_award != 0) ||
            !std::isfinite(encounter.relief) || encounter.relief < 0 || encounter.relief > kRetryReliefCap ||
            encounter.relief > encounter.wipes * kRetryRelief * RateMultiplier(Nightmare) + 1e-9 ||
            (encounter.won && encounter.relief != 0) || (encounter.wipes == 0 && encounter.relief != 0)) return false;
        recorded_wipes += encounter.wipes;
    }
    if (recorded_wipes > snapshot.wipes.size()) return false;
    snapshot_ = snapshot;
    return true;
}
}
