#ifndef CODEC_TRANSCODESTATS_H
#define CODEC_TRANSCODESTATS_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace mediakit {

/**
 * Global runtime counters for the transcode / decode pipeline.
 *
 * These free functions are always available (they return 0 when the relevant
 * feature is compiled out), so callers such as the GlobalMonitor do not need to
 * pull in any FFmpeg headers.
 */

/** Number of live TranscodeProcessor instances (active transcode streams). */
size_t getTranscodeStreamCount();

/**
 * Number of live FFmpegDecoder instances (video decoders currently in use,
 * shared by motion detection and transcode pipelines).
 */
size_t getVideoDecoderCount();

/** Number of live FFmpegEncoder instances (video encoders currently in use). */
size_t getVideoEncoderCount();

/** Number of camera MJPEG tracks re-encoded at ingest (MjpegIngestTranscoder). */
size_t getMjpegIngestTranscodeCount();

/** Number of live MotionProcessor instances (active motion-detection pipelines). */
size_t getMotionProcessorCount();

/** Video decoders / encoders currently running on the GPU (subset of the counts above). */
size_t getHwDecoderCount();
size_t getHwEncoderCount();

/** Decoders/encoders that started on the GPU and went back to the CPU since startup. */
uint64_t getHwFallbackCount();

/** Comma separated GPU backends in use ("cuda", "vaapi", "qsv"); empty when everything runs on the CPU. */
std::string getHwCodecBackends();

/** GPU load per device, "cuda:0 dec=120/720 enc=30" (1080p fps; budget after '/' when configured). */
std::string getHwCodecLoad();

} // namespace mediakit

#endif // CODEC_TRANSCODESTATS_H
