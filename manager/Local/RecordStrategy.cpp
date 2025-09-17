#include "RecordStrategy.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

RecordStrategy::RecordStrategy() {}

RecordStrategy::~RecordStrategy() {
    _scheduler.reset();
}

static string getFulltimeRecordAlwaysString() {
    string s(168, RecordModeHelper::toChar(RecordMode::RecordAlways));
    return s;
}

void RecordStrategy::setupScheduler(const std::string &schedule_str) {
    auto _tmp_str = schedule_str;
    if (_tmp_str.empty()) {
        // input empty, set default value;
        _tmp_str = getFulltimeRecordAlwaysString();
    }
    // compare config and recreate if config change
    if (_scheduler && _scheduler->getSchedulerString() == _tmp_str) {
        return;
    }

    _scheduler = std::make_shared<TimeScheduler<RecordMode, RecordModeHelper>>(_tmp_str);
    _scheduler->setOnChangeMode([&](RecordMode &mode) {
        onRecordModeChange(mode);
    });
    _scheduler->start();
}

void RecordStrategy::stopScheduler() {
    _scheduler.reset();
}

RecordMode RecordStrategy::getRecordModeActive() {
    return _scheduler->getModeActive();
}

} // namespace managerkit
