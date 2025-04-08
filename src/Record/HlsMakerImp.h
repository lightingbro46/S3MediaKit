#ifndef HLSMAKERIMP_H
#define HLSMAKERIMP_H

#include <memory>
#include <string>
#include <stdlib.h>
#include "HlsMaker.h"
#include "HlsMediaSource.h"

namespace mediakit {

class HlsMakerImp : public HlsMaker {
public:
    HlsMakerImp(bool is_fmp4, const std::string &m3u8_file, const std::string &params, uint32_t bufSize = 64 * 1024,
                float seg_duration = 5, uint32_t seg_number = 3, bool seg_keep = false);
    ~HlsMakerImp() override;

    /**
     * Set media information
     */
    void setMediaSource(const MediaTuple& tuple);

    /**
     * Get MediaSource
     * @return
     */
    HlsMediaSource::Ptr getMediaSource() const;

     /**
      * Clear cache
      */
     void clearCache();

protected:
    std::string onOpenSegment(uint64_t index) override ;
    void onDelSegment(uint64_t index) override;
    void onWriteInitSegment(const char *data, size_t len) override;
    void onWriteSegment(const char *data, size_t len) override;
    void onWriteHls(const std::string &data, bool include_delay) override;
    void onFlushLastSegment(uint64_t duration_ms) override;

private:
    std::shared_ptr<FILE> makeFile(const std::string &file,bool setbuf = false);
    void clearCache(bool immediately, bool eof);
    void saveCurrentDir();

private:
    int _buf_size;
    std::string _params;
    std::string _path_hls;
    std::string _path_hls_delay;
    std::string _path_init;
    std::string _path_prefix;
    std::string _current_dir;
    std::string _current_dir_init_file;
    RecordInfo _info;
    std::shared_ptr<FILE> _file;
    std::shared_ptr<char> _file_buf;
    HlsMediaSource::Ptr _media_src;
    toolkit::EventPoller::Ptr _poller;
    std::map<uint64_t/*index*/,std::string/*file_path*/> _segment_file_paths;
    std::deque<std::tuple<int,std::string> > _current_dir_seg_list;
};

}//namespace mediakit
#endif //HLSMAKERIMP_H
