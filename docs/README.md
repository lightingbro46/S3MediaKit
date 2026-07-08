# 🚀 Hướng dẫn Build S3MediaKit từ Source

## Tài liệu tích hợp/API

- [Luồng Playback](./playback-flow.md)
- [Timeline & Thumbnail](./api-timeline-thumbnail.md)
- [Bookmark](./api-bookmark.md)
- [PTZ Preset](./api-ptz-preset.md)
- [Sync DB](./api-sync.md)
- [Storage Tiering](./api-storage-tiering.md)

## Mục lục

1. [Yêu cầu hệ thống](#-yêu-cầu-hệ-thống)
2. [Cài đặt Dependencies](#-cài-đặt-dependencies)
3. [Build AWS SDK (bắt buộc)](#-build-aws-sdk-bắt-buộc)
4. [Build S3MediaKit](#-build-s3mediakit)
5. [Cấu hình sau khi build](#-cấu-hình-sau-khi-build)
6. [Chạy MediaServer](#-chạy-mediaserver)
7. [Docker Build](#-docker-build)
8. [Xử lý lỗi thường gặp](#-xử-lý-lỗi-thường-gặp)

---

## 🖥️ Yêu cầu hệ thống

| Thành phần | Phiên bản tối thiểu |
|------------|---------------------|
| **OS** | Ubuntu 22.04 (khuyến nghị) hoặc tương đương |
| **CMake** | >= 3.16 (Khuyến nghị 3.22.1)|
| **GCC/G++** | Hỗ trợ C++11 |
| **Git** | Bất kỳ phiên bản nào |
| **Make** | GNU Make |

---

## 📦 Cài đặt Dependencies

### Bước 1: Cài đặt các gói cần thiết

```bash
# Cập nhật package list
sudo apt-get update

# Cài đặt các dependencies
sudo apt-get install -y \
    build-essential \
    cmake \
    git \
    curl \
    vim \
    wget \
    ca-certificates \
    tzdata \
    libssl-dev \
    protobuf-compiler \
    libprotobuf-dev \
    libsqlite3-dev \
    libcurl4-openssl-dev \
    libsrtp2-dev \
    gcc \
    g++ \
    gdb \
    ffmpeg

# Cài đặt pkg-config (cần thiết cho một số thư viện)
sudo apt-get install -y pkg-config
```

### Bước 2: Kiểm tra FFmpeg

```bash
# Kiểm tra FFmpeg đã cài đặt
ffmpeg -version

# Nếu chưa có, cài đặt FFmpeg
sudo apt-get install -y ffmpeg
```

---

## 🔧 Build AWS SDK (BẮT BUỘC nếu ENABLE_AWS=true)

> ⚠️ **Lưu ý quan trọng:** AWS SDK cần được build riêng TRƯỚC KHI build S3MediaKit. Thư viện SDK phải được install vào thư mục `3rdpart/aws-sdk/bin/` để CMake có thể tìm thấy.

### Bước 1: Clone AWS SDK (nếu chưa có)

```bash
cd 3rdpart
# Kiểm tra xem đã có aws-sdk chưa
ls -la aws-sdk/

# Nếu chưa có, clone về
# git clone https://github.com/aws/aws-sdk-cpp.git aws-sdk-cpp
```

### Bước 2: Tạo thư mục build và install

```bash
cd 3rdpart/aws-sdk-cpp
mkdir -p build
mkdir -p ../aws-sdk/bin
```

### Bước 3: Configure AWS SDK

```bash
cd 3rdpart/aws-sdk-cpp/build

cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON \
    -DBUILD_ONLY="s3;core" \
    -DENABLE_TESTING=OFF \
    -DAUTORUN_UNIT_TESTS=OFF \
    -DCMAKE_INSTALL_PREFIX=../../aws-sdk/bin
```

**Giải thích các options:**

| Option | Mô tả |
|--------|-------|
| `BUILD_SHARED_LIBS=ON` | Build thư viện dạng shared (.so) thay vì static |
| `BUILD_ONLY="s3;core"` | Chỉ build S3 và Core modules (tiết kiệm thời gian) |
| `ENABLE_TESTING=OFF` | Tắt unit tests |
| `AUTORUN_UNIT_TESTS=OFF` | Không chạy tests tự động |
| `CMAKE_INSTALL_PREFIX=../../aws-sdk/bin` | Install vào thư mục `3rdpart/aws-sdk/bin/` |

### Bước 4: Build AWS SDK

```bash
make -j$(nproc)
```

> ⏱️ **Thời gian build:** Có thể mất 10-30 phút tùy thuộc vào cấu hình máy.

### Bước 5: Install AWS SDK

```bash
make install
```

### Bước 6: Xác minh cài đặt thành công

```bash
# Kiểm tra thư mục install
ls -la ../../aws-sdk/bin/

# Phải thấy:
# ├── bin/          (thư viện executable nếu có)
# ├── include/      (header files)
# └── lib/          (shared libraries .so)
```

---

## 🔨 Build S3MediaKit

### Bước 1: Tạo thư mục build

```bash
cd S3MediaKit
mkdir -p build release/linux/Release
```

### Bước 2: Configure với CMake

```bash
cd build

# Debug build
cmake .. \
    -DCMAKE_BUILD_TYPE=Debug \
    -DENABLE_WEBRTC=false \
    -DENABLE_FFMPEG=false \
    -DENABLE_TESTS=false \
    -DENABLE_API=false \
    -DENABLE_AWS_SDK=ON

# Hoặc Release build (khuyến nghị cho production)
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_WEBRTC=false \
    -DENABLE_FFMPEG=false \
    -DENABLE_TESTS=false \
    -DENABLE_API=false \
    -DENABLE_AWS_SDK=ON
```

**Các options quan trọng:**

| Option | Mặc định | Mô tả |
|--------|----------|-------|
| `ENABLE_AWS_SDK` | ON | Bật/tắt hỗ trợ AWS S3 |
| `ENABLE_WEBRTC` | OFF | Bật WebRTC support |
| `ENABLE_FFMPEG` | ON | Bật FFmpeg (cần cài sẵn) |
| `ENABLE_TESTS` | OFF | Bật unit tests |
| `ENABLE_API` | OFF | Bật C API SDK |
| `ENABLE_MP4` | ON | Bật hỗ trợ ghi MP4 |
| `ENABLE_HLS` | ON | Bật HLS streaming |
| `ENABLE_MANAGER` | ON | Bật resource manager |
| `ENABLE_MOTION` | ON | Bật motion detection |

### Bước 3: Build

```bash
make -j$(nproc)
```

> ⏱️ **Thời gian build:** Có thể mất 5-15 phút tùy thuộc vào cấu hình máy.

### Bước 4: Kiểm tra output

```bash
# Binary đã build
ls -la ../release/linux/Release/MediaServer

# Các file đi kèm
ls -la ../release/linux/Release/
```

---

## ⚙️ Cấu hình sau khi build

### Bước 1: Tạo thư mục cấu hình

```bash
cd release/linux/Release
mkdir -p ../conf
```

### Bước 2: Copy file cấu hình mẫu

```bash
# Copy config file (nếu có trong source)
# cp ../../../../conf/config.ini ../conf/

# Tạo file config.ini đơn giản
cat > ../conf/config.ini << 'EOF'
[protocol]
# Cấu hình các port
port=8080
rtmp_port=1935
rtsp_port=554
rtc_port=8000

[api]
# Cấu hình API secret
secret=your_secret_key_here
EOF
```

### Bước 3: Kiểm tra thư mục www

```bash
# Thư mục www phải có
ls -la www/

# Nếu chưa có, copy từ source
cp -r ../../../../www/ .
```

---

## ▶️ Chạy MediaServer

### Khởi động đơn giản

```bash
cd release/linux/Release

# Chạy với config mặc định
./MediaServer

# Hoặc với config tùy chỉnh
./MediaServer -c ../conf/config.ini
```

### Các tham số dòng lệnh

```bash
# Hiển thị help
./MediaServer -h

# Các options phổ biến:
#   -s <file>    : File SSL certificate (default.pem)
#   -c <file>    : File cấu hình (config.ini)
#   -l <level>   : Log level (0-6)
#   -d           : Daemon mode (chạy nền)
```

### Kiểm tra trạng thái

```bash
# Kiểm tra MediaServer đang chạy
ps aux | grep MediaServer

# Kiểm tra log
tail -f ./logs/MediaServer.log
```

---

## 🐳 Docker Build

### Build Docker Image

```bash
# Build với Debug mode
docker build \
    --build-arg MODEL=Debug \
    -t s3mediakit:debug \
    .

# Build với Release mode
docker build \
    --build-arg MODEL=Release \
    -t s3mediakit:release \
    .

# Build với proxy (nếu cần)
docker build \
    --build-arg MODEL=Release \
    --build-arg HTTP_PROXY=http://proxy.example.com:8080 \
    --build-arg HTTPS_PROXY=http://proxy.example.com:8080 \
    -t s3mediakit:release \
    .
```

### Chạy Docker Container

```bash
# Chạy container
docker run -d \
    --name s3mediakit \
    -p 8080:8080 \
    -p 1935:1935 \
    -p 554:554 \
    -p 8443:8443 \
    -p 10000:10000 \
    -p 8000:8000 \
    -v $(pwd)/config:/opt/media/conf \
    s3mediakit:release

# Kiểm tra logs
docker logs -f s3mediakit

# Stop container
docker stop s3mediakit
```

### Sử dụng docker-compose

```yaml
# docker-compose.yml
version: '3.8'

services:
  mediaserver:
    build:
      context: .
      args:
        MODEL: Release
    container_name: s3mediakit
    ports:
      - "8080:8080"    # HTTP API
      - "1935:1935"    # RTMP
      - "554:554"      # RTSP
      - "8443:8443"    # HTTPS
      - "10000:10000"  # WebRTC UDP
      - "8000:8000"    # WebRTC TCP
    volumes:
      - ./config:/opt/media/conf
      - ./recordings:/opt/media/recordings
    environment:
      - TZ=Asia/Ho_Chi_Minh
    restart: unless-stopped
```

```bash
# Chạy với docker-compose
docker-compose up -d

# Xem logs
docker-compose logs -f

# Stop
docker-compose down
```

---

## 🐛 Xử lý lỗi thường gặp

### Lỗi 1: AWS SDK không tìm thấy

```
CMake Error: AWS SDK headers not found at .../aws-sdk/bin/include
```

**Giải pháp:**
```bash
# Kiểm tra thư mục install của AWS SDK
ls -la 3rdpart/aws-sdk/bin/

# Nếu chưa install, chạy lại:
cd 3rdpart/aws-sdk-cpp/build
make install
```

### Lỗi 2: FFmpeg không tìm thấy

```
warning: ffmpeg related functions not found
```

**Giải pháp:**
```bash
# Cài đặt FFmpeg development libraries
sudo apt-get install -y \
    libavutil-dev \
    libavcodec-dev \
    libavfilter-dev \
    libswscale-dev \
    libswresample-dev

# Hoặc cài FFmpeg đầy đủ
sudo apt-get install -y ffmpeg
```

### Lỗi 3: OpenSSL không tìm thấy

```
warning: openssl not found, rtmp will not support flash player
```

**Giải pháp:**
```bash
sudo apt-get install -y libssl-dev
```

### Lỗi 4: SQLite3 không tìm thấy

**Giải pháp:**
```bash
sudo apt-get install -y libsqlite3-dev
```

### Lỗi 5: Build chậm hoặc hết memory

**Giải pháp:**
```bash
# Giảm số core build
make -j2

# Hoặc build trong Docker với nhiều memory hơn
docker build --build-arg MAKEFLAGS="-j2" ...
```

---

## 📝 Scripts hỗ trợ

### Script build nhanh

```bash
#!/bin/bash
# build.sh - Script build nhanh S3MediaKit

set -e

echo "=== Building AWS SDK ==="
cd 3rdpart/aws-sdk-cpp
mkdir -p build 
mkdir -p ../aws-sdk/bin
cd build
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON \
    -DBUILD_ONLY="s3;core" \
    -DENABLE_TESTING=OFF \
    -DAUTORUN_UNIT_TESTS=OFF \
    -DCMAKE_INSTALL_PREFIX=../../aws-sdk/bin
make -j$(nproc)
make install

echo "=== Building S3MediaKit ==="
cd ../../..
mkdir -p build release/linux/Release
cd build
cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_WEBRTC=false \
    -DENABLE_FFMPEG=false \
    -DENABLE_TESTS=false \
    -DENABLE_API=false \
    -DENABLE_AWS_SDK=ON
make -j$(nproc)

echo "=== Build hoàn tất ==="
ls -la ../release/linux/Release/MediaServer
```

---

## 📞 Hỗ trợ

Nếu gặp vấn đề trong quá trình build, vui lòng:

1. Kiểm tra log lỗi chi tiết
2. Đảm bảo đã build AWS SDK trước
3. Kiểm tra các dependencies đã được cài đặt đầy đủ
4. Liên hệ đội ngũ hỗ trợ kỹ thuật

---

**Chúc bạn build thành công!** 🎉
