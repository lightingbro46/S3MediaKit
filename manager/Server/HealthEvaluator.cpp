#include "HealthEvaluator.h"
#include <algorithm>
#include <cmath>
#include <sstream>

using namespace std;

namespace managerkit {

const char *healthLevelString(HealthLevel level) {
    switch (level) {
        case HealthLevel::OK: return "ok";
        case HealthLevel::WARNING: return "warning";
        case HealthLevel::CRITICAL: return "critical";
        default: return "unknown";
    }
}

HealthLevel worseLevel(HealthLevel a, HealthLevel b) {
    if (a == HealthLevel::UNKNOWN) {
        return b;
    }
    if (b == HealthLevel::UNKNOWN) {
        return a;
    }
    return (int)a >= (int)b ? a : b;
}

HealthLevel classifyValue(double value, const HealthThreshold &th) {
    if (std::isnan(value)) {
        return HealthLevel::UNKNOWN;
    }
    if (th.higher_is_worse) {
        if (value >= th.crit) {
            return HealthLevel::CRITICAL;
        }
        return value >= th.warn ? HealthLevel::WARNING : HealthLevel::OK;
    }
    if (value <= th.crit) {
        return HealthLevel::CRITICAL;
    }
    return value <= th.warn ? HealthLevel::WARNING : HealthLevel::OK;
}

// ── HealthHysteresis ─────────────────────────────────────────────

HealthLevel HealthHysteresis::update(const string &key, HealthLevel observed, int sustain, int64_t now, int64_t *since) {
    State &st = _states[key];
    st.seen_cycle = _cycle;
    if (observed == HealthLevel::UNKNOWN) {
        st.level = HealthLevel::UNKNOWN;
        st.since = now;
        st.candidate = HealthLevel::UNKNOWN;
        st.count = 0;
    } else if (observed == st.level) {
        st.candidate = HealthLevel::UNKNOWN;
        st.count = 0;
    } else {
        if (st.candidate == observed) {
            ++st.count;
        } else {
            st.candidate = observed;
            st.candidate_since = now;
            st.count = 1;
        }
        if (st.count >= std::max(1, sustain)) {
            st.level = st.candidate;
            st.since = st.candidate_since;
            st.candidate = HealthLevel::UNKNOWN;
            st.count = 0;
        }
    }
    if (since) {
        *since = st.since;
    }
    return st.level;
}

void HealthHysteresis::sweep(uint64_t cycle) {
    for (auto it = _states.begin(); it != _states.end();) {
        if (it->second.seen_cycle < cycle) {
            it = _states.erase(it);
        } else {
            ++it;
        }
    }
}

// ── HealthReason / HealthEvaluation ──────────────────────────────

static Json::Value jsonNumber(double v) {
    if (std::isnan(v) || std::isinf(v)) {
        return Json::Value(Json::nullValue);
    }
    // Two decimals are enough for every metric and keep the response small.
    return Json::Value(std::round(v * 100.0) / 100.0);
}

Json::Value HealthReason::toJson() const {
    Json::Value v;
    v["component"] = component;
    v["metric"] = metric;
    v["level"] = healthLevelString(level);
    v["value"] = jsonNumber(value);
    v["threshold"] = jsonNumber(threshold);
    v["since"] = (Json::Int64)since;
    if (!detail.empty()) {
        v["detail"] = detail;
    }
    return v;
}

string HealthEvaluation::signature() const {
    stringstream ss;
    ss << (int)overall << '|';
    for (const auto &kv : components) {
        ss << kv.first << '=' << (kv.second.enabled ? (int)kv.second.level : -1) << ';';
    }
    ss << '|';
    for (const auto &r : reasons) {
        ss << r.component << '/' << r.metric << '/' << r.detail << '=' << (int)r.level << ';';
    }
    return ss.str();
}

// ── HealthEvaluator ──────────────────────────────────────────────

const vector<string> &HealthEvaluator::componentNames() {
    static const vector<string> names = { "host", "process", "ingest", "recording", "delivery", "storage", "cluster" };
    return names;
}

bool HealthEvaluator::isExternalComponent(const string &component) {
    return component == "ingest" || component == "delivery";
}

static int componentOrder(const string &component) {
    const auto &names = HealthEvaluator::componentNames();
    auto it = std::find(names.begin(), names.end(), component);
    return (int)(it - names.begin());
}

HealthEvaluator::HealthEvaluator(HealthHysteresis &hysteresis, int sustain, int64_t now)
    : _hysteresis(hysteresis), _sustain(sustain), _now(now) {
    for (const auto &name : componentNames()) {
        _eval.components[name] = HealthComponent();
    }
}

HealthLevel HealthEvaluator::addValue(const string &component, const string &metric, double value,
                                      const HealthThreshold &th, const string &detail) {
    return record(component, metric, classifyValue(value, th), value, th.warn, th.crit, detail, false);
}

HealthLevel HealthEvaluator::addLevel(const string &component, const string &metric, HealthLevel observed,
                                      double value, double threshold, const string &detail, bool sustained) {
    return record(component, metric, observed, value, threshold, threshold, detail, sustained);
}

HealthLevel HealthEvaluator::record(const string &component, const string &metric, HealthLevel observed,
                                    double value, double warn, double crit, const string &detail, bool sustained) {
    int64_t since = _now;
    string key = component + "/" + metric + "/" + detail;
    // A source with its own hysteresis (DiskIoMonitor) still goes through update() with sustain 1,
    // which keeps "since" stable across cycles while its level holds.
    HealthLevel level = _hysteresis.update(key, observed, sustained ? 1 : _sustain, _now, &since);
    if (level != HealthLevel::UNKNOWN) {
        _has_known[component] = true;
        auto &comp = _eval.components[component];
        comp.level = comp.level == HealthLevel::UNKNOWN ? level : worseLevel(comp.level, level);
    }
    if (level == HealthLevel::WARNING || level == HealthLevel::CRITICAL) {
        HealthReason r;
        r.component = component;
        r.metric = metric;
        r.level = level;
        r.value = value;
        r.threshold = level == HealthLevel::CRITICAL ? crit : warn;
        r.since = since;
        r.detail = detail;
        _eval.reasons.push_back(std::move(r));
    }
    return level;
}

void HealthEvaluator::setEnabled(const string &component, bool enabled) {
    _eval.components[component].enabled = enabled;
}

HealthEvaluation HealthEvaluator::finish() {
    HealthLevel overall = HealthLevel::UNKNOWN;
    int score = 100;
    for (auto &kv : _eval.components) {
        auto &comp = kv.second;
        if (!comp.enabled || !_has_known[kv.first]) {
            comp.level = HealthLevel::UNKNOWN;
            continue;
        }
        if (comp.level == HealthLevel::WARNING) {
            score -= 10;
        } else if (comp.level == HealthLevel::CRITICAL) {
            score -= 30;
        }
        HealthLevel contrib = comp.level;
        if (isExternalComponent(kv.first) && contrib == HealthLevel::CRITICAL) {
            contrib = HealthLevel::WARNING;
        }
        overall = overall == HealthLevel::UNKNOWN ? contrib : worseLevel(overall, contrib);
    }
    _eval.overall = overall;
    _eval.score = std::max(0, score);
    std::stable_sort(_eval.reasons.begin(), _eval.reasons.end(), [](const HealthReason &a, const HealthReason &b) {
        if (a.level != b.level) {
            return (int)a.level > (int)b.level;
        }
        int oa = componentOrder(a.component), ob = componentOrder(b.component);
        if (oa != ob) {
            return oa < ob;
        }
        if (a.metric != b.metric) {
            return a.metric < b.metric;
        }
        return a.detail < b.detail;
    });
    return _eval;
}

} // namespace managerkit
