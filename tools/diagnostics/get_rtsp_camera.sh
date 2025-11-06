#!/bin/bash
# Script: fetch_camera_streams.sh
# Yêu cầu: cài đặt jq (sudo apt install jq -y)

API_BASE="https://anat.vtscloud.vn/api"

USERNAME="demotk"
PASSWORD="Demo@2025"

echo "=== Bước 1: Đăng nhập ==="
login_response=$(curl -s -X POST "$API_BASE/authentication/login" -H "Content-Type: application/json" -d "{\"username\":\"$USERNAME\",\"password\":\"$PASSWORD\",\"captcha\":\"\"}")

# Kiểm tra login thành công
access_token=$(echo "$login_response" | jq -r '.accessToken')
project_id=$(echo "$login_response" | jq -r '.projectId')

if [ -z "$access_token" ] || [ "$access_token" = "null" ]; then
  echo "❌ Login thất bại. Phản hồi:"
  echo "$login_response"
  exit 1
fi

echo "✅ Login thành công."
echo "AccessToken: $access_token"
echo "ProjectId: $project_id"

echo
echo "=== Bước 2: Gọi API lấy danh sách camera ==="

camera_response=$(curl -s -X POST "$API_BASE/camera/search?page=0&size=100&sort=updatedAt,desc" \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $access_token" \
  -d "{\"name\":null,\"status\":null,\"area\":null,\"ip\":null,\"rtspPort\":null,\"httpPort\":null,\"tcpPort\":null,\"projectId\":\"$project_id\"}")

# Kiểm tra dữ liệu hợp lệ
if ! echo "$camera_response" | jq -e . >/dev/null 2>&1; then
  echo "❌ Phản hồi không hợp lệ:"
  echo "$camera_response"
  exit 1
fi

echo
echo "=== Bước 3: In danh sách camera/stream ==="
echo "$camera_response" | jq -r '.data[] | . as $cam | .streams[]? | "rtsp://anat.vtscloud.vn:554/\($cam.id)/\(.id)"'


echo "=== Bước 3: Xuất JSON gồm tên camera + luồng stream ==="
result=$(echo "$camera_response" | jq -r '
  .data | map(
  . as $cam |
  {
    cameraName: .name,
    cameraId: .id,
    streams: (
      (.streams // []) | map("rtsp://anat.vtscloud.vn:554/\($cam.id)/\(.id)")
    )
  })
')

echo "$result" | jq .