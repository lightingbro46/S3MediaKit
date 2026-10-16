#ifndef S3MANAGERKIT_HEALTHEVALUATOR_H
#define S3MANAGERKIT_HEALTHEVALUATOR_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "json/json.h"

namespace managerkit {

/**
 * Health levels of the monitor API (docs/api-health-monitor.md).
 * The order of OK < WARNING < CRITICAL is the severity order; UNKNOWN means "not enough samples
 * yet" or "no data" and never counts towards the overall level.
 */
enum class HealthLevel : int {
    OK = 0,
    WARNING = 1,
    CRITICAL = 2,
    UNKNOWN = 3,
};

const char *healthLevelString(HealthLevel level);
// The more severe of two levels; UNKNOWN loses to any known level.
HealthLevel worseLevel(HealthLevel a, HealthLevel b);

struct HealthThreshold {
    double warn = 0;
    double crit = 0;
    // true: value >= warn is bad (CPU %); false: value <= warn is bad (fps ratio).
    bool higher_is_worse = true;
};

// Level of `value` against `th`. NaN gives UNKNOWN.
HealthLevel classifyValue(double value, const HealthThreshold &th);

/**
 * Hysteresis of one metric: the confirmed level changes only after `sustain` consecutive samples
 * at the same new level (both up and down), so the API does not flip green/red per sample.
 * A new metric stays UNKNOWN until its first level is confirmed. An UNKNOWN sample (no data)
 * resets the metric to UNKNOWN at once.
 */
class HealthHysteresis {
public:
    struct State {
        HealthLevel level = HealthLevel::UNKNOWN;
        int64_t since = 0;            // unix seconds the confirmed level started
        HealthLevel candidate = HealthLevel::UNKNOWN;
        int64_t candidate_since = 0;
        int count = 0;
        uint64_t seen_cycle = 0;
    };

    // Confirmed level of `key` after observing `observed` at `now`.
    HealthLevel update(const std::string &key, HealthLevel observed, int sustain, int64_t now,
                       int64_t *since = nullptr);
    // Drops metrics not updated since `cycle` (cameras removed, NICs gone).
    void sweep(uint64_t cycle);
    void setCycle(uint64_t cycle) { _cycle = cycle; }
    size_t size() const { return _states.size(); }

private:
    std::map<std::string, State> _states;
    uint64_t _cycle = 0;
};

struct HealthReason {
    std::string component;
    std::string metric;
    HealthLevel level = HealthLevel::OK;
    double value = 0;
    double threshold = 0;     // the threshold that was crossed (warn or crit)
    int64_t since = 0;
    std::string detail;       // e.g. mount point or NIC name; empty when the metric is global

    Json::Value toJson() const;
};

struct HealthComponent {
    bool enabled = true;      // false: compiled out (N5), not counted
    HealthLevel level = HealthLevel::UNKNOWN;
};

struct HealthEvaluation {
    HealthLevel overall = HealthLevel::UNKNOWN;
    int score = 100;
    std::map<std::string, HealthComponent> components;
    std::vector<HealthReason> reasons; // most severe first

    // Stable signature of levels and reasons (no values), used for `rev`.
    std::string signature() const;
};

/**
 * One evaluation cycle. Collectors call add*() for every metric, then finish() gives the
 * component levels, the overall level and the reasons. Pure logic: no singleton, no clock,
 * so unit tests drive it directly. The hysteresis state lives in the HealthHysteresis passed in
 * and survives between cycles.
 */
class HealthEvaluator {
public:
    // Components of section 5.2, in display order.
    static const std::vector<std::string> &componentNames();
    // Components whose problems sit outside the server (cameras, viewers): they raise the
    // overall level to WARNING at most.
    static bool isExternalComponent(const std::string &component);

    HealthEvaluator(HealthHysteresis &hysteresis, int sustain, int64_t now);

    // Numeric metric against a threshold; `detail` separates instances (mount, NIC).
    HealthLevel addValue(const std::string &component, const std::string &metric, double value,
                         const HealthThreshold &th, const std::string &detail = std::string());
    // Metric with a level decided elsewhere (already sustained, or a boolean failure).
    // `sustained` = true skips the hysteresis because the source already has its own.
    HealthLevel addLevel(const std::string &component, const std::string &metric, HealthLevel observed,
                         double value, double threshold, const std::string &detail = std::string(),
                         bool sustained = false);
    void setEnabled(const std::string &component, bool enabled);

    HealthEvaluation finish();

private:
    HealthLevel record(const std::string &component, const std::string &metric, HealthLevel observed,
                       double value, double warn, double crit, const std::string &detail, bool sustained);

private:
    HealthHysteresis &_hysteresis;
    int _sustain;
    int64_t _now;
    HealthEvaluation _eval;
    std::map<std::string, bool> _has_known; // component -> got at least one known metric
};

} // namespace managerkit

#endif // S3MANAGERKIT_HEALTHEVALUATOR_H
