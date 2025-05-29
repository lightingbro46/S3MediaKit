#ifndef SRC_MEDIAFILE_RECORDER_H_
#define SRC_MEDIAFILE_RECORDER_H_

#include <memory>
#include <string>

namespace mediakit {
class MediaSinkInterface;
class ProtocolOption;

struct MediaTuple {
    std::string vhost;
    std::string app;
    std::string stream;
    std::string params;
    std::string shortUrl() const {
        return vhost + '/' + app + '/' + stream;
    }
};

class RecordInfo: public MediaTuple {
public:
    time_t start_time;  // GMT standard time, unit seconds
    float time_len;     // Recording length, unit seconds
    uint64_t file_size;    // File size, unit BYTE
    std::string file_path;   // File path
    std::string file_name;   // File name
    std::string folder;      // Folder path
    std::string url;         // Play path
};

class Recorder{
public:
    typedef enum {
        // Record hls
        type_hls = 0,
        // Record MP4
        type_mp4 = 1,
        // Record hls.fmp4
        type_hls_fmp4 = 2,
        // fmp4 live
        type_fmp4 = 3,
        // ts live
        type_ts = 4,
        // Record MKV
        type_mkv = 5,
        // WebM live
        type_webm = 6,
    } type;

    /**
     * Get the absolute path of the recording file
     * @param type hls or MP4 recording
     * @param vhost virtual host
     * @param app application name
     * @param stream_id stream id
     * @param customized_path custom root directory for saving recording files, empty means using configuration file settings
     * @return  absolute path of the recording file
     */
    static std::string getRecordPath(type type, const MediaTuple& tuple, const std::string &customized_path = "");

    /**
     * Create a recorder object
     * @param type hls or MP4 recording
     * @param vhost virtual host
     * @param app application name
     * @param stream_id stream id
     * @param customized_path custom root directory for saving recording files, empty means using configuration file settings
     * @param max_second maximum slice time for mp4 recording, in seconds, 0 means using configuration file settings
     * @return object pointer, may be nullptr
     */
    static std::shared_ptr<MediaSinkInterface> createRecorder(type type, const MediaTuple& tuple, const ProtocolOption &option);

private:
    Recorder() = delete;
    ~Recorder() = delete;
};

} /* namespace mediakit */
#endif /* SRC_MEDIAFILE_RECORDER_H_ */
