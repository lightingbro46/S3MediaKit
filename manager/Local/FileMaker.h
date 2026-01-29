#ifndef LOCAL_FILEMAKER_H_
#define LOCAL_FILEMAKER_H_

#include <string>
#include <deque>

namespace managerkit {

template<typename ENTRY>
class FileMaker {
public:
    virtual ~FileMaker() = default;

    virtual bool findLowerBound(ENTRY &entry, uint64_t &stamp) {
        bool found = false;
        if (onSeekEntry(0)) {
            bool eof = false;
            uint64_t last_time = 0;
            while (!eof && last_time < stamp) {
                ENTRY sample;
                onReadEntry(sample, eof);
                if (!eof) {
                    if (getStampOfEntry(sample) < stamp) {
                        entry = sample;
                        found = true;
                    }
                    last_time = getStampOfEntry(sample);
                }
            }
        }
        return found;
    }

    virtual bool inputEntry(ENTRY &entry) {
        onWriteEntry(entry);
        return true;
    }

    virtual uint64_t findFirstStamp() {
        uint64_t first_stamp = 0;
        if (onSeekEntry(0)) {
            bool eof = false;
            ENTRY entry;
            onReadEntry(entry, eof);
            if (!eof) {
                first_stamp = getStampOfEntry(entry);
            }
        }
        return first_stamp;
    }

    virtual uint64_t findLastStamp() {
        uint64_t last_stamp = 0;
        if (onSeekEntry(0)) {
            bool eof = false;
            ENTRY entry;
            while(!eof) {
                onReadEntry(entry, eof);
                if (!eof) {
                    last_stamp = getStampOfEntry(entry);
                }
            }
        }
        return last_stamp;
    }

    uint64_t getFirstStamp() { return _first_stamp; }
    void setFirstStamp(uint64_t stamp) { _first_stamp = stamp; }

    uint64_t getLastStamp() { return _last_stamp; }
    void setLastStamp(uint64_t stamp) { _last_stamp = stamp; }

protected:
    virtual void onWriteEntry(ENTRY &entry) = 0;

    virtual void onReadEntry(ENTRY &entry, bool &eof) = 0;

    virtual bool onSeekEntry(uint32_t offset) = 0;

    virtual uint64_t getStampOfEntry(ENTRY &entry) = 0;

private:
    uint64_t _first_stamp = 0;
    uint64_t _last_stamp = 0;
};

} // namespace managerkit

#endif // LOCAL_FILEMAKER_H_