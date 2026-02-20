#include "RecordPolicy.h"
#include "Util/util.h"
#include "Common/StrUtil.h"
#include "Camera/GenericRtspCameraImp.h"
#include "Local/TimeMaker.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

string getRecordModeString(RecordMode mode) {
    switch (mode) {
        case RecordMode::NoRecord:             return "NoRecord";
        case RecordMode::RecordOnlyMotion:     return "RecordOnlyMotion";
        case RecordMode::RecordLowResAndMotion:return "RecordLowResAndMotion";
        case RecordMode::RecordAlways:         return "RecordAlways";
        default:                               return "Unknown";
    }
}

static unordered_map<std::string, RecordScheduleItem::Ptr> parseRecordScheduleStr(const string &str) {
    unordered_map<std::string, RecordScheduleItem::Ptr> ret;

    Json::Value root;
    if (!StrJsonUtils::readJsonString(str, root)) {
        WarnL << "Failed to parse record schedule string";
        return ret;
    }

    if (!root.isArray()) {
        WarnL << "Record schedule json is not an array";
        return ret;
    }

    for (const auto &item : root) {
        RecordScheduleItem::Ptr s = std::make_shared<RecordScheduleItem>();
        string key;
        if (item.isMember("dh") && item["dh"].isString()) {
            // format: "d,h", e.g. "0,13" = Sunday 13:00
            string dh_str = item["dh"].asString();
            auto tmp = split(dh_str, ",");
            if (tmp.size() == 2) {
                string day_str = tmp[0];
                string hour_str = tmp[1];
                s->day = stoi(day_str);
                s->hour = stoi(hour_str);
            }
            key = dh_str;
        }

        if (item.isMember("fps") && item["fps"].isInt()) {
            s->fps = item["fps"].asInt();
        }

        if (item.isMember("q") && item["q"].isString()) {
            s->q = item["q"].asString();
        }

        if (item.isMember("ty") && item["ty"].isInt()) {
            auto ty = item["ty"].asInt();
            s->mode = static_cast<RecordMode>(ty);
        }

        ret.emplace(key, s);
    }

    return ret;
}

RecordingController::RecordingController(const toolkit::EventPoller::Ptr &poller) {
    _poller = poller ? poller : EventPollerPool::Instance().getPoller();
}

RecordingController::~RecordingController() {
    _poller.reset();
    _schedules.clear();
}

void RecordingController::start() {
    std::weak_ptr<RecordingController> weak_self = shared_from_this();
    _timer = std::make_shared<Timer>(
        5000,
        [weak_self]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return false;
            }
            auto it = strong_self->getRecordScheduledActive();
            if (it->mode != strong_self->_current_mode) {
                strong_self->onSchedulerChange(it);
            }
            return true;
        },
        _poller);
}

void RecordingController::setScheduleStr(const std::string &schedule_str) {
    if (schedule_str.empty()) {
        WarnL << "Empty schedule string. Ignored.";
        return;
    }
    _schedules = parseRecordScheduleStr(schedule_str);
    auto it = getRecordScheduledActive();
    if (it->mode != _current_mode) {
        onSchedulerChange(it);
    }
}

void RecordingController::onSchedulerChange(RecordScheduleItem::Ptr &item) {
    _current_mode = item->mode;
    InfoL << "Recording mode changed to " << getRecordModeString(_current_mode);

    // Apply recording mode to camera
    switch (_current_mode) {
        case RecordMode::NoRecord:
            onNoRecordMode();
            break;
        case RecordMode::RecordOnlyMotion:
            onRecordOnlyMotionMode();
            break;
        case RecordMode::RecordLowResAndMotion:
            onRecordLowResAndMotionMode();
            break;
        case RecordMode::RecordAlways:
            onRecordAlwaysMode();
            break;
        default:
            WarnL << "Unknown recording mode. Ignored";
            break;
    }
}

RecordScheduleItem::Ptr RecordingController::getRecordScheduledActive() {
    auto week_time = StrTimeUtils::getWeekTime(time(nullptr));
    string time_str = (StrPrinter << week_time.day_of_week << "," << week_time.hour);
    auto it = _schedules.find(time_str);
    if (it != _schedules.end()) {
        return it->second;
    }
    // default
    WarnL << "No matching schedule found for " << time_str << ", return default schedule item";
    return std::make_shared<RecordScheduleItem>();
}


void RecordingController::onRecordAlwaysMode() {
    for (auto &it : _state_map) {
        if (it.second != RecordState::Recording) {
            // stream is not recording, start recording
            if (_on_change) {
                _on_change(it.first, true, false, 0);
            }
            it.second = RecordState::Recording;
        } else {
            // stream has already recording, keep recording
        }
    }
}

void RecordingController::onRecordOnlyMotionMode() {
    for (auto &it : _state_map) {
        if (_event_active) {
            if (it.second != RecordState::Recording) {
                // auto option = strong_camera->getCameraOption();
                // auto start_of_hour = getStartOfHour(time(nullptr));
                // int backtime_sec = MIN(option.motionPreRecordSec, static_cast<int>((time(nullptr) - start_of_hour)));
                int backtime_sec = 0;
                // stream is not recording, start recording
                if (_on_change) {
                    _on_change(it.first, true, false, backtime_sec * 1000);
                }
                it.second = RecordState::Recording;
            } else {
                // stream has already recording, keep recording
            }
        } else {
            if (it.second == RecordState::Recording) {
                // stream is already recording, stop recording
                if (_on_change) {
                    _on_change(it.first, false, false, 0);
                }
                it.second = RecordState::Idle;
            } else {
                // stream is already idle, keep idle
            }
        }
    }
}

void RecordingController::onNoRecordMode() {
    for (auto &it : _state_map) {
        if (it.second == RecordState::Recording) {
            // stream is already recording, stop recording
            if (_on_change) {
                _on_change(it.first, false, false, 0);
            }
            it.second = RecordState::Idle;
        } else {
            // stream is already idle, keep idle
        }
    }
}

void RecordingController::onRecordLowResAndMotionMode() {
    for (auto &it : _state_map) {
        if (it.first == PrimaryStream) {
            if (it.second == RecordState::Recording) {
                if (_event_active) {
                    // primary stream has already recording, event is active => keep recording
                } else {
                    // primary stream is recording, event is not active => stop recording
                    if (_on_change) {
                        _on_change(it.first, false, false, 0);
                    }
                    it.second = RecordState::Idle;
                }
            } else {
                if (_event_active) {
                    // primary stream is already idle, event is active => start recording
                    // auto option = strong_camera->getCameraOption();
                    // auto start_of_hour = getStartOfHour(time(nullptr));
                    // int backtime_sec = MIN(option.motionPreRecordSec, static_cast<int>((time(nullptr) - start_of_hour)));
                    int backtime_sec = 0;
                    if (_on_change) {
                        _on_change(it.first, true, false, backtime_sec * 1000);
                    }
                    it.second = RecordState::Recording;
                } else {
                    // primary stream is already idle, event is not active => keep idle
                }
            }
        } else { // secondary stream
            if (it.second == RecordState::Recording) {
                if (_event_active) {
                    // secondary stream has already recording, event is active => stop recording
                    if (_on_change) {
                        _on_change(it.first, false, true, 0);
                    }
                    it.second = RecordState::Idle;
                } else {
                    // secondary stream is recording, event is not active => keep recording
                }
            } else {
                if (_event_active) {
                    // secondary stream is already idle, event is active => keep idle
                } else {
                    // secondary stream is already idle, event is not active => start recording
                    if (_on_change) {
                        _on_change(it.first, true, true, 0);
                    }
                    it.second = RecordState::Recording;
                }
            }
        }
    }
}

void RecordingController::onRecordEvent(bool bActive, uint64_t pre_ms) {
    DebugL << "Record event active: " << bActive << ", pre_ms: " << pre_ms;
    _event_active = bActive;
}

} // namespace managerkit
