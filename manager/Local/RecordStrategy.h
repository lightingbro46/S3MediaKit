#ifndef LOCAL_RECORDSTRATEGY_H
#define LOCAL_RECORDSTRATEGY_H

#include <string>
#include "Local/TimeScheduler.h"

namespace managerkit {
    
enum class RecordMode : uint8_t {
    NoRecord = 0,
    RecordAlways,
    RecordOnlyMotion,
    RecordLowResAndMotion,
    RecordMax
};

class RecordModeHelper {
public:
    static std::string toString(RecordMode mode) {
        switch (mode) {
            case RecordMode::NoRecord:             return "NoRecord";
            case RecordMode::RecordAlways:         return "RecordAlways";
            case RecordMode::RecordOnlyMotion:     return "RecordOnlyMotion";
            case RecordMode::RecordLowResAndMotion:return "RecordLowResAndMotion";
            default:                               return "Unknown";
        }
    }

    static RecordMode fromChar(const char c) {
        int mode = c - '0';
        if (mode < count()) {
            return static_cast<RecordMode>(mode);
        }
        return RecordMode::NoRecord; // default fallback
    }

    static char toChar(RecordMode mode) {
        char c = '0' + (int) mode;
        return c;
    }

    static int count() {
        return static_cast<int>(RecordMode::RecordMax);
    }
};

class RecordStrategy {
public:
    RecordStrategy();
    ~RecordStrategy();

    void setupScheduler(const std::string &schedule_str);
    
    void stopScheduler();

    RecordMode getRecordModeActive();

protected:
    virtual void onRecordModeChange(RecordMode mode) {}

private:
    TimeScheduler<RecordMode, RecordModeHelper>::Ptr _scheduler;
};

} // namespace managerkit

#endif // LOCAL_RECORDSTRATEGY_H