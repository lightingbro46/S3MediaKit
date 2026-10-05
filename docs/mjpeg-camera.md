# Camera RTSP MJPEG

Áp dụng từ nhánh `feature/mjpeg-ingest-transcode`. Phần chưa chạy thật: [pending-real-device-checks.md](./pending-real-device-checks.md).

## Cách hoạt động

Camera RTSP gửi video MJPEG (RTP/JPEG, RFC 2435, payload 26) được MediaServer **giải mã rồi mã hoá lại thành H264
một lần ngay lúc kéo luồng**. Mọi thứ phía sau chỉ thấy H264 giống như camera H264:

- xem trực tiếp trên mọi giao thức: WebRTC, HTTP-FLV/RTMP, HLS, fMP4, RTSP;
- ghi hình MP4 và xem lại (timeline, seek, chuyển Main/Sub);
- motion, snapshot, watermark/privacy mask, reduce quality.

Audio của camera (nếu có) đi thẳng, không bị mã hoá lại.

Không có transcode (config tắt, bản build `ENABLE_FFMPEG=OFF`, hoặc vượt `ingestMaxSessions`) thì track JPEG đi
thẳng như trước. Khi đó chỉ RTSP và file MP4 chứa được MJPEG; WebRTC, FLV, HLS và fMP4 trên trình duyệt không phát
được.

Giới hạn:
- RFC 2435 chỉ chứa ảnh tối đa 2040×2040 px, nên camera MJPEG 4K không kéo được qua RTSP.
- Chỉ áp dụng cho camera **kéo** RTSP (`addStreamProxy`, camera VMS). MJPEG đẩy vào bằng RTSP push, RTP/GB28181
  hoặc FFmpegSource chưa được transcode.
- Giải mã MJPEG chạy CPU. Mã hoá H264 dùng GPU nếu `hwcodec` có backend, không thì libx264
  (xem [hw-codec.md](./hw-codec.md)). Với libx264, mỗi camera 1080p25 tốn khoảng 0,5–1 core.

## Config (`[mjpeg]`)

| Key | Mặc định | Ý nghĩa |
|---|---|---|
| `mjpeg.ingestTranscode` | `1` | Bật/tắt transcode cho mọi camera (toàn cục). Đổi giá trị có hiệu lực từ lần kéo luồng tiếp theo |
| `mjpeg.ingestCodec` | `H264` | Codec đầu ra: `H264` hoặc `H265` |
| `mjpeg.ingestBitrate` | `0` | bit/s; `0` = tự chọn theo độ phân giải và fps |
| `mjpeg.ingestGopSec` | `2` | Khoảng cách keyframe (giây) |
| `mjpeg.ingestMaxSessions` | `0` | Số camera MJPEG transcode cùng lúc; `0` = không giới hạn. Vượt thì track JPEG đi thẳng |

## Báo codec lên client

Field cũ giữ nguyên nghĩa: là codec **client nhận được**, nên player chọn decoder theo nó như trước. Field mới cho
biết codec thật của camera. Tên codec gốc của camera MJPEG là `JPEG` (giống `Encoding` của ONVIF).

| API | Codec client nhận | Codec của camera | Cờ |
|---|---|---|---|
| `getMediaList` / `getMediaInfo` và webhook `on_stream_changed` (mỗi phần tử `tracks[]`) | `codec_id`, `codec_id_name` | `origin_codec_id`, `origin_codec_id_name` | `transcoded` |
| Thống kê stream của camera (`primaryStream` / `secondaryStream`) | `vcodec` | `sourceVcodec` | `transcoded` |

Ví dụ một camera MJPEG:

```json
{ "vcodec": "H264", "sourceVcodec": "JPEG", "transcoded": true, "width": 1280, "height": 720 }
```

Camera không bị transcode thì `sourceVcodec` = `vcodec`, `origin_codec_id_name` = `codec_id_name`, `transcoded` =
`false`. Thống kê lưu từ trước bản này không có `source_vcodec` nên được đọc ra bằng `vcodec`.

`getStatistic` thêm `MjpegIngestTranscode` = số camera MJPEG đang được transcode.
