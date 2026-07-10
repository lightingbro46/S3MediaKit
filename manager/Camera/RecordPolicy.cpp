#include "RecordPolicy.h"
#include "Util/util.h"
#include "Common/StrUtil.h"
#include "Camera/GenericRtspCameraImp.h"

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

std::string getImageQualityString(ImageQuality quality) {
    switch (quality) {
        case ImageQuality::Low:     return "Low";
        case ImageQuality::Medium:  return "Medium";
        case ImageQuality::High:    return "High";
        default:                   return "Unknown";
    }
}

std::string getRecordEventTypeString(RecordEventType type) {
    switch (type) {
        case RecordEventType::Unknown: return "Unknown";
        case RecordEventType::Motion:  return "Motion";
        default:                      return "Unknown";
    }
}

static int toRecordScheduleDay(int tm_wday) {
    // Scheduler dh day index starts at Monday 00:00: Monday=0, ..., Sunday=6.
    return (tm_wday + 6) % 7;
}

static unordered_map<std::string, RecordScheduleItem> parseRecordScheduleStr(const string &str) {
    unordered_map<std::string, RecordScheduleItem> ret;

    if (!str.empty()) {
        Json::Value root;
        if (!StrJsonUtils::readJsonString(str, root)) {
            WarnL << "Failed to parse record schedule string";
            return ret;
        }

        if (!root.isArray()) {
            WarnL << "Record schedule json is not an array";
            return ret;
        }

        // The json array is expected to contain items with the following format:
        // [
        //   {
        //     "dh": "0,13", // day and hour, e.g. "0,13" = Monday 13:00
        //     "ty": 1, // record mode, e.g. 1 = RecordOnlyMotion
        //     "fps": 15, // optional, fps for recording
        //     "q": "M" // optional, image quality for recording, e.g. "M" = Medium, "L" = Low, "H" = High
        //   },
        //   ...
        // ]
        for (const auto &item : root) {
            RecordScheduleItem s;
            string key;
            if (item.isMember("dh") && item["dh"].isString()) {
                // format: "d,h", e.g. "0,13" = Monday 13:00
                string dh_str = item["dh"].asString();
                auto tmp = split(dh_str, ",");
                if (tmp.size() == 2) {
                    string day_str = tmp[0];
                    string hour_str = tmp[1];
                    s.day = stoi(day_str);
                    s.hour = stoi(hour_str);
                    if (s.day < 0 || s.day > 6 || s.hour < 0 || s.hour > 23) {
                        WarnL << "Invalid record schedule dh value: " << dh_str;
                        continue;
                    }
                    key = to_string(s.day) + "," + to_string(s.hour);
                }
            }
            if (key.empty())
                continue;

            if (item.isMember("fps") && item["fps"].isInt()) {
                // optional, default to 0 if not specified
                s.fps = item["fps"].asInt();
            }

            if (item.isMember("q") && item["q"].isString()) {
                // optional, default to Low if not specified
                auto q_str = item["q"].asString();
                if (q_str == "L") {
                    s.q = ImageQuality::Low;
                } else if (q_str == "M") {
                    s.q = ImageQuality::Medium;
                } else if (q_str == "H") {
                    s.q = ImageQuality::High;
                } else {
                    s.q = ImageQuality::Low;
                }
            }

            if (item.isMember("ty") && item["ty"].isInt()) {
                // required, default to NoRecord if not specified
                auto ty = item["ty"].asInt();
                s.mode = static_cast<RecordMode>(ty);
            }

            ret.emplace(key, s);
        }
    }

    // Fill in missing schedule with default value (NoRecord, fps=0, q=Low)
    for (int i = 0; i < 7; ++i) {
        for (int j = 0; j < 24; ++j) {
            string key = to_string(i) + "," + to_string(j);
            if (ret.find(key) == ret.end()) {
                RecordScheduleItem s;
                s.day = i;
                s.hour = j;
                ret.emplace(key, s);
            }
        }
    }

    return ret;
}

RecordScheduler::Ptr RecordScheduler::create(const DeviceTuple &tuple, const std::string &schedule_str, const toolkit::EventPoller::Ptr &poller) {
    auto scheduler = std::make_shared<RecordScheduler>(tuple, schedule_str, poller);
    scheduler->createTimer();
    DebugL << "Created record scheduler for device: " << tuple.shortUrl() << ". Trigger after 1 second, then every 1 hour. Schedule profile: " << (schedule_str.empty() ? "empty" : "*******");
    return scheduler;
}

RecordScheduler::RecordScheduler(const DeviceTuple &tuple, const std::string &profile, const toolkit::EventPoller::Ptr &poller) : _tuple(tuple), _profile(profile), _poller(poller) {
    _items = parseRecordScheduleStr(profile);
}

RecordScheduler::~RecordScheduler() {
    stopTimer();
    _poller.reset();
}

void RecordScheduler::setListener(const std::shared_ptr<DeviceSourceEvent> &delegate) {
    setDelegate(delegate);
}

void RecordScheduler::createTimer() {
    weak_ptr<RecordScheduler> weak_self = shared_from_this();
    _timer = std::make_shared<Timer>(
        1.0f,
        [weak_self]() {
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return false;
            }
            auto time_now = time(nullptr);
            auto it = strong_self->getRecordScheduledActive(time_now);
            if (it != strong_self->_it) {
                strong_self->onSchedulerChange(it->second);
                strong_self->_it = it;
            }
            // todo: trigger onPollStreamStatus, onPollDeviceStatus 
            return true;
        },
        _poller);
}

void RecordScheduler::stopTimer() {
    _timer.reset();
    _items.clear();
    _it = _items.end();
}

void RecordScheduler::onSchedulerChange(RecordScheduleItem &item) {
    auto _current_mode = _it == _items.end() ? RecordMode::NoRecord : _it->second.mode;
    if (_current_mode != item.mode) {
        DebugL << "Recording mode of device: " << _tuple.shortUrl() << " changed to " << getRecordModeString(item.mode) << " (day=" << item.day << ", hour=" << item.hour << ")";
        auto event_active = _event_active;
        onRecordModeChange(DeviceSource::NullDeviceSource(), static_cast<int>(item.mode), event_active);
    }

    auto _current_fps = _it == _items.end() ? 0 : _it->second.fps;
    auto _current_q = _it == _items.end() ? ImageQuality::Low : _it->second.q;
    if (_current_fps != item.fps || _current_q != item.q) {
        // DebugL << "Recording quality of device: " << _tuple.shortUrl() << " changed to fps=" << item.fps << ", q=" << getImageQualityString(item.q) << " (day=" << item.day << ", hour=" << item.hour << ")";
        onImageQualityChange(DeviceSource::NullDeviceSource(), item.fps, static_cast<int>(item.q));
    }
}

RecordScheduler::RecordScheduleMap::iterator RecordScheduler::getRecordScheduledActive(time_t time) {
    auto week_time = StrTimeUtils::getWeekTime(time);
    string time_str = (StrPrinter << toRecordScheduleDay(week_time.day_of_week) << "," << week_time.hour);
    auto it = _items.find(time_str);
    if (it == _items.end()) {
        // throw exception if no schedule found for current time, this should not happen because we fill in default schedule for all time in parseRecordScheduleStr
        throw std::runtime_error("No record schedule found for current time: " + time_str);
    }
    return it;
}

bool RecordScheduler::setupRecordEvent(RecordEventType type, bool start) {
    RecordMode mode = RecordMode::NoRecord;
    if (_it != _items.end()) {
        mode = _it->second.mode;
    }
    _event_active = start;

    if (mode == RecordMode::NoRecord) {
        WarnL << "Current schedule mode is NoRecord, ignore record event: " << getRecordEventTypeString(type) << ", start=" << start;
        return false;
    } else if (mode == RecordMode::RecordOnlyMotion) {
        if (type != RecordEventType::Motion) {
            WarnL << "Current schedule mode is RecordOnlyMotion, ignore non-motion event: " << getRecordEventTypeString(type) << ", start=" << start;
            return false;
        } else {
            onRecordModeChange(DeviceSource::NullDeviceSource(), static_cast<int>(mode), start);
        }
    } else if (mode == RecordMode::RecordLowResAndMotion) {
        if (type != RecordEventType::Motion) {
            WarnL << "Current schedule mode is RecordLowResAndMotion, ignore non-motion event: " << getRecordEventTypeString(type) << ", start=" << start;
            return false;
        } else {
            onRecordModeChange(DeviceSource::NullDeviceSource(), static_cast<int>(mode), start);
        }
    } else if (mode == RecordMode::RecordAlways) {
        WarnL << "Current schedule mode is RecordAlways, ignore record event: " << getRecordEventTypeString(type) << ", start=" << start;
        return false; // always record, no need to handle record event
    }
    return true;
}

} // namespace managerkit
