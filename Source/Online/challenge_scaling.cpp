#include <Online/challenge_scaling.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace ChallengeScaling {
namespace {
constexpr double kTimePerSecond = 0.05 / 300.0;
constexpr double kRoundAward = 0.08;
constexpr double kWinAward = 0.04;
constexpr double kDeathAward = 0.05;
constexpr size_t kMaximumEvents = 8192;
constexpr size_t kMaximumKeyBytes = 1024;

bool ValidID(const std::string& id) {
    return !id.empty() && id.size() <= kMaximumKeyBytes;
}
double Bounded(double pressure) {
    return std::max(0.0, std::min(kPressureCap, pressure));
}
}

double RateMultiplier(int rate) {
    switch (rate) {
        case Gentle: return 0.5;
        case Standard: return 1.0;
        case Brisk: return 1.5;
        default: return 0.0;
    }
}

Factors Evaluate(double pressure) {
    const double amount = std::isfinite(pressure) ? Bounded(pressure) : 0.0;
    Factors result;
    result.vitality = static_cast<float>(1.0 + amount);
    result.damage = static_cast<float>(1.0 + amount * 0.15);
    result.armor = static_cast<float>(1.0 + amount * 0.10);
    return result;
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

bool State::Death(const std::string& event_id, const std::string& encounter_id, const Settings& settings) {
    if (!ValidID(event_id) || (!encounter_id.empty() && !ValidID(encounter_id)) ||
        snapshot_.deaths.count(event_id) || snapshot_.deaths.size() >= kMaximumEvents) return false;
    if (!encounter_id.empty()) {
        const auto encounter = snapshot_.encounters.find(encounter_id);
        if (encounter == snapshot_.encounters.end() && snapshot_.encounters.size() >= kMaximumEvents) return false;
        ++snapshot_.encounters[encounter_id].deaths;
    }
    snapshot_.deaths.insert(event_id);
    // Adaptive replaces the death pressure driver with a future win discount.
    // A down/death never both strengthens enemies now and discounts that win.
    if (!settings.adaptive)
        snapshot_.pressure = Bounded(snapshot_.pressure + kDeathAward * RateMultiplier(settings.death_rate));
    return true;
}

bool State::Win(const std::string& encounter_id, const Settings& settings) {
    if (!ValidID(encounter_id)) return false;
    const auto previous = snapshot_.encounters.find(encounter_id);
    if (previous != snapshot_.encounters.end() && previous->second.won) return false;
    if (previous == snapshot_.encounters.end() && snapshot_.encounters.size() >= kMaximumEvents) return false;
    EncounterRecord& record = snapshot_.encounters[encounter_id];
    record.won = true;
    const double credit = settings.adaptive ? std::max(0.0, 1.0 - 0.25 * record.deaths) : 1.0;
    record.win_award = kWinAward * RateMultiplier(settings.win_rate) * credit;
    snapshot_.pressure = Bounded(snapshot_.pressure + record.win_award);
    return true;
}

void State::Clear() {
    snapshot_ = Snapshot();
}

bool State::Restore(const Snapshot& snapshot) {
    if (!std::isfinite(snapshot.active_seconds) || snapshot.active_seconds < 0 ||
        !std::isfinite(snapshot.pressure) || snapshot.pressure < 0 || snapshot.pressure > kPressureCap ||
        snapshot.completions.size() > kMaximumEvents || snapshot.deaths.size() > kMaximumEvents ||
        snapshot.encounters.size() > kMaximumEvents) return false;
    for (const auto& id : snapshot.completions) if (!ValidID(id)) return false;
    for (const auto& id : snapshot.deaths) if (!ValidID(id)) return false;
    uint64_t recorded_deaths = 0;
    for (const auto& entry : snapshot.encounters) {
        const EncounterRecord& encounter = entry.second;
        if (!ValidID(entry.first) || !std::isfinite(encounter.win_award) || encounter.win_award < 0 ||
            encounter.win_award > kWinAward * RateMultiplier(Brisk) + 1e-9 || (!encounter.won && encounter.win_award != 0)) return false;
        recorded_deaths += encounter.deaths;
    }
    if (recorded_deaths > snapshot.deaths.size()) return false;
    snapshot_ = snapshot;
    return true;
}
}
