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

camera_ids=($(echo "$camera_response" | jq -r '.data[].id'))

echo
echo "=== Bước 3: Lấy chi tiết từng camera (lat/lon) ==="

result="[]"

for cam_id in "${camera_ids[@]}"; do
  cam_name=$(echo "$camera_response" | jq -r --arg id "$cam_id" '.data[] | select(.id == $id) | .name')
  streams=$(echo "$camera_response" | jq --arg id "$cam_id" \
    '.data[] | select(.id == $id) | (.streams // []) | map("rtsp://vms:Vms@2025@10.49.100.187:8554/\($id)/\(.id)?realm=vms")')

  if [ -z "$streams" ]; then
    streams="[]"
  fi

  # Gọi API lấy chi tiết camera
  detail_response=$(curl -s -X GET "$API_BASE/camera/detail/$cam_id" \
    -H "Content-Type: application/json" \
    -H "Authorization: Bearer $access_token")

  lat=$(echo "$detail_response" | jq '.lat')
  lon=$(echo "$detail_response" | jq '.lon')

  # Ghép vào mảng kết quả
  item=$(jq -n \
    --arg name "$cam_name" \
    --arg id "$cam_id" \
    --argjson streams "$(echo "$streams" | jq '.')" \
    --argjson lat "$lat" \
    --argjson lon "$lon" \
    '{cameraName: $name, cameraId: $id, lat: $lat, lon: $lon, streams: $streams}')

  result=$(echo "$result" | jq --argjson new "$item" '. += [$new]')
done

echo
echo "=== Bước 4: Kết quả JSON gồm tên camera + lat/lon + streams ==="
echo "$result" | jq .