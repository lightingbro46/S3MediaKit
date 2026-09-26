# Kiểm thử E2E với camera thật (Main + Sub) và MinIO

Harness cho danh mục kịch bản ở [`docs/e2e-test-plan.md`](../../../docs/e2e-test-plan.md).
Dựng một MediaServer Debug riêng với 1 camera thật, JWT RS256 tự ký, mock service API,
rồi kiểm tự động các kịch bản có cột **Auto = A**.

## Yêu cầu

- `MediaServer` bản **Debug** (chỉ Debug đọc `./api-config.json`), `ENABLE_TIER_STORAGE=ON`,
  `ENABLE_AWS_SDK=ON`, `ENABLE_FFMPEG=ON`, `ENABLE_MOTION=ON`.
- `ffmpeg`/`ffprobe`, `python3` với gói `cryptography` (ký JWT), `curl`, `ss`.
- Cổng trống: `PORT_BASE` (19080) HTTP, +1 RTSP, +2 RTMP, +10 mock API.

## Nhập thông tin (không ghi vào file, không để trong history)

```bash
read -r -s -p 'RTSP Main: ' E2E_RTSP_MAIN; echo
read -r -s -p 'RTSP Sub:  ' E2E_RTSP_SUB; echo
read -r -p 'MinIO endpoint (http://host:9000): ' E2E_S3_ENDPOINT
read -r -p 'MinIO bucket test: ' E2E_S3_BUCKET
read -r -p 'MinIO access key: ' E2E_S3_ACCESS_KEY
read -r -s -p 'MinIO secret key: ' E2E_S3_SECRET_KEY; echo
export E2E_RTSP_MAIN E2E_RTSP_SUB E2E_S3_ENDPOINT E2E_S3_BUCKET E2E_S3_ACCESS_KEY E2E_S3_SECRET_KEY
```

`api-config.json` (chứa URL RTSP), `secrets.json`, `jwt_private.pem`, `encrypt_password`
trong work dir đều có quyền 600. Báo cáo và output đã che JWT, `secret=`, `user:pass@`.

## Chạy

```bash
H=tests/integration/e2e_real/run_e2e.sh
W=/tmp/s3mk_e2e

$H $W layout                      # tạo work dir
$H $W start
$H $W check smoke live auth       # ~3 phút
# để ghi ≥ 10 phút rồi:
$H $W check record --expect-main 1 --expect-sub 1 --scenario REC-01
$H $W check replay bookmark extract

$H $W camera --overlay 1          # nạp lại cấu hình camera (≤ 10 s), không restart
$H $W check overlay               # rồi xem ảnh trong $W/evidence/

$H $W camera --mode lowres_motion --overlay 0
sleep 180
$H $W check record --expect-main 0 --expect-sub 1 --scenario REC-04

$H $W check tier-setup            # cần E2E_S3_*; policy HOT(0 ngày)→COLD MinIO
sleep 700                         # ≥ 2 chu kỳ tiering 300 s
$H $W check tier-verify           # TIR-05..07, 09

$H $W stop
```

Mã hoá chạy trên work dir **mới** để bản ghi cũ không lẫn vào:

```bash
W=/tmp/s3mk_e2e_enc; $H $W layout && $H $W start
$H $W check tier-setup            # tuỳ chọn: để kiểm .enc lên MinIO
$H $W check encrypt-setup --encrypt-mode 2
sleep 180; $H $W check encrypt-verify
```

Tham số `camera` (giá trị không truyền được giữ nguyên từ lần trước, lưu ở
`camera_state.json`): `--mode always|lowres_motion|only_motion|none`,
`--schedule all|flip|otherday`, `--enable-recording 0|1`, `--overlay 0|1`,
`--disable-main/--disable-sub 0|1`, `--no-record-main/--no-record-sub 0|1`,
`--disable-audio 0|1`, `--motion 0|1`, `--rtp-transport TCP|UDP|AUTO`,
`--keep-max-hours N`, `--keep-min-hours N`, `--present 0|1` (gỡ camera khỏi cấu hình).

`kill9` dùng cho REC-13 (`$H $W kill9 && $H $W start`).

## Kết quả

- Màn hình: một dòng mỗi kịch bản `ID  PASS|FAIL|SKIP  chi tiết`.
- `<work>/report.json`: gộp theo ID, lần chạy sau ghi đè lần trước.
- `<work>/evidence/`: ảnh live/replay theo từng role overlay, thumbnail, file extract.
- `<work>/mock_api_events.jsonl`: mọi hook server đã gọi (kiểm AUTH-06, EXT-07).
- Log server: `<work>/stdout.log`, `<work>/log/`.

`SKIP` không phải `PASS`. Kịch bản **M** trong kế hoạch vẫn phải làm tay.

## Những điều harness phải làm khác cấu hình mặc định (và vì sao)

- `hook.on_play=/api/camera/check-camera-of-user`: `conf/config.ini` để trống khoá này,
  ghi đè mặc định trong code. Với `manager.enableAuthorize=1`, `on_play` rỗng làm **mọi**
  phiên media bị từ chối "Unauthorized" mà không gọi hook nào.
- Mock trả `media_server: {}` ở top-level cho hook cấu hình: khi `manager.apiSecret` khác
  rỗng, server coi body là cấu hình mã hoá trừ khi có khoá đó.
- Mock từ chối quyền bằng **HTTP 403**: server không xét trường `code` của body hook
  (đoạn kiểm trong `parse_http_response` đang `#if 0`), chỉ xét HTTP status.
