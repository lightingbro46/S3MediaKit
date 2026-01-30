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
echo "=== Bước 3: Lấy danh sách Media Server ==="

media_server_response=$(curl -s -X GET "$API_BASE/recording/get-list-media-server" \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $access_token")

if ! echo "$media_server_response" | jq -e . >/dev/null 2>&1; then
  echo "❌ Không lấy được danh sách media server"
  echo "$media_server_response"
  exit 1
fi

echo "✅ Lấy media server thành công"

echo
echo "=== Bước 4: Lấy chi tiết từng camera (lat/lon) ==="

result="[]"

for cam_id in "${camera_ids[@]}"; do
  cam_name=$(echo "$camera_response" | jq -r --arg id "$cam_id" '.data[] | select(.id == $id) | .name')
  streams_json=$(echo "$camera_response" | jq --arg id "$cam_id" \
    '.data[] | select(.id == $id) | (.streams // [])')

  if [ -z "$streams_json" ]; then
    streams_json="[]"
  fi

  media_server_id=$(echo "$camera_response" | jq -r --arg id "$cam_id" \
    '.data[] | select(.id == $id) | .mediaServerId')
  if [ -z "$media_server_id" ] || [ "$media_server_id" = "null" ]; then
    echo "⚠️ Camera $cam_name chưa gán media server"
    media_server_found=false
  else
    media_server=$(echo "$media_server_response" | jq --arg msid "$media_server_id" \
      '.[] | select(.id == $msid)')

    if [ -z "$media_server" ]; then
      echo "⚠️ Không tìm thấy media server cho camera $cam_name"
      media_server_found=false
    else 
      media_server_found=true
    fi
  fi
  
  if [ "$media_server_found" = true ]; then
    ms_ip=$(echo "$media_server" | jq -r '.ip // empty')
    ms_rtsp_port=$(echo "$media_server" | jq -r '.rtspPort // empty')
    ms_domain=$(echo "$media_server" | jq -r '.domain // empty')
    ms_https_port=$(echo "$media_server" | jq -r '.httpsPort // empty')
    client_ssl=$(echo "$media_server" | jq -r '.clientUseSsl')
    scheme="http"
    [ "$client_ssl" = "true" ] && scheme="https"
  else
    ms_ip=""
    ms_domain=""
    ms_rtsp_port=""
    ms_https_port=""
    scheme=""
  fi

  streams_with_links=$(echo "$streams_json" | jq -c \
    --arg cam_id "$cam_id" \
    --arg ip "$ms_ip" \
    --arg domain "$ms_domain" \
    --arg rtsp_port "$ms_rtsp_port" \
    --arg https_port "$ms_https_port" \
    --arg scheme "$scheme"              \
    --arg ms_found "$media_server_found" '
    map({
      streamId: .id,
      streamName: .name,
      rtsp: (
        if ($ms_found == "true" and $ip != "" and $rtsp_port != "")
        then "rtsp://vms:Vms@2026@\($ip):\($rtsp_port)/\($cam_id)/\(.id)?realm=ioc"
        else null
        end
      ),
      http_mp4: (
        if ($ms_found == "true" and $domain != "" and $https_port != "")
        then "\($scheme)://\($domain):\($https_port)/media/live/\($cam_id)/\(.id).mp4"
        else null
        end
      )
    })
  ')

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
  --argjson lat "$lat" \
  --argjson lon "$lon" \
  --argjson streams "$streams_with_links" \
  --arg msid "$media_server_id" \
  --arg ms_found "$media_server_found" \
  '{
    cameraName: $name, 
    cameraId: $id, 
    lat: $lat, 
    lon: $lon, 
    mediaServer: {
      id: $msid,
      found: ($ms_found == "true")
    },
    streams: $streams
  }')

  result=$(echo "$result" | jq --argjson new "$item" '. += [$new]')
done

echo
echo "=== Bước 4: Kết quả JSON gồm tên camera + lat/lon + streams ==="
echo "$result" | jq .