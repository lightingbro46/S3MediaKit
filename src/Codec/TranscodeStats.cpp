#include "TranscodeStats.h"

#if defined(ENABLE_FFMPEG)
#include "Util/util.h"
#include "Transcode.h"
#include "Transcode/TranscodeProcessor.h"
#endif

#if defined(ENABLE_MOTION) && defined(ENABLE_FFMPEG)
#include "Motion/MotionProcessor.h"
#endif

namespace mediakit {

#if defined(ENABLE_FFMPEG)

size_t getTranscodeStreamCount() {
    return toolkit::ObjectStatistic<TranscodeProcessor>::count();
}

size_t getVideoDecoderCount() {
    return toolkit::ObjectStatistic<FFmpegDecoder>::count();
}

size_t getVideoEncoderCount() {
    return toolkit::ObjectStatistic<FFmpegEncoder>::count();
}

#else // ENABLE_FFMPEG

size_t getTranscodeStreamCount() { return 0; }
size_t getVideoDecoderCount() { return 0; }
size_t getVideoEncoderCount() { return 0; }

#endif // ENABLE_FFMPEG

#if defined(ENABLE_MOTION) && defined(ENABLE_FFMPEG)

size_t getMotionProcessorCount() {
    return toolkit::ObjectStatistic<MotionProcessor>::count();
}

#else

size_t getMotionProcessorCount() { return 0; }

#endif // ENABLE_MOTION && ENABLE_FFMPEG

} // namespace mediakit
