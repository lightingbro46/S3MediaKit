#ifndef LOCAL_STORAGEPOLICY_H
#define LOCAL_STORAGEPOLICY_H

#include <cstdint>
#include <string>

namespace managerkit {

/**
 * Pure helpers behind the storage retention policy. They live apart from StorageManager so the
 * decision rules can be unit tested without dragging the whole manager (and the server symbols it
 * references) into the link.
 */

/**
 * Oldest date directory actually present for one stream, as a unix timestamp, or 0 when the stream
 * has no recording on disk. `key` is "<device_id>/<stream_id>", matching the layout
 * "<record_path>/<device_id>/<stream_id>/<YYYY-MM-DD>/<segment>.mp4". Only directory names are
 * read: no file is opened and no database is touched. Names that do not parse as a date are
 * ignored, so the archive subtree and any stray directory are skipped.
 */
uint64_t oldestSegmentDate(const std::string &record_path, const std::string &key);

/**
 * Deletion threshold for one stream: `keep_percent` of the span between the two bounds, measured
 * back from the newer one. `min_value` is normally the newer bound, but a keepArchivedMinFor
 * larger than keepArchivedMaxFor inverts the pair, so both orders are accepted rather than
 * wrapping the unsigned subtraction. `keep_percent` is clamped to [0, 100].
 */
uint64_t keepTimeThreshold(uint64_t min_value, uint64_t max_value, int keep_percent);

/**
 * Deletion floor for Auto mode. `max_value` comes from CameraStatistic, which can report a date
 * later than the oldest file really on disk: time blocks whose end_time <= created_at are dropped
 * entirely, archiveStartTime is assigned once without a min(), and it jumps forward when tier
 * storage relocates a block. Taking that value at face value silently deletes a camera's first
 * segments, so it is clamped down to what the filesystem shows. When `is_auto` is false the value
 * is an explicit operator choice and is returned unchanged.
 */
uint64_t clampMaxToDisk(const std::string &record_path, const std::string &key, uint64_t max_value,
                        bool is_auto);

} // namespace managerkit

#endif // LOCAL_STORAGEPOLICY_H
