FROM ubuntu:22.04 AS build
ARG MODEL
#shell,rtmp,rtsp,rtsps,http,https,rtp
EXPOSE 1935/tcp
EXPOSE 554/tcp
EXPOSE 8080/tcp
EXPOSE 8443/tcp
EXPOSE 10000/udp
EXPOSE 10000/tcp
EXPOSE 8000/udp
EXPOSE 8000/tcp
EXPOSE 9000/udp

# ADD sources.list /etc/apt/sources.list

RUN apt-get update && \
         DEBIAN_FRONTEND="noninteractive" \
         apt-get install -y --no-install-recommends \
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
         libsrtp2-dev  \
         nasm \
         pkg-config \
         libfreetype-dev \
         librsvg2-dev \
         libva-dev \
         libmfx-dev \
         libx264-dev \
         libx265-dev \
         libfaac-dev \
         zlib1g-dev \
         gcc \
         g++ \
         gdb && \
         apt-get autoremove -y && \
         apt-get clean -y && \
         rm -rf /var/lib/apt/lists/*

RUN mkdir -p /opt/media
COPY . /opt/media/S3MediaKit
WORKDIR /opt/media/S3MediaKit

# FFmpeg (3rdpart/FFmpeg) and the AWS SDK (3rdpart/aws-sdk-cpp) are git submodules built by CMake
# into release/linux/3rdpart. The submodules must be checked out before docker build
# (build_docker_images.sh checks it).
RUN mkdir -p build release/linux/${MODEL}/

WORKDIR /opt/media/S3MediaKit/build
RUN cmake -DCMAKE_BUILD_TYPE=${MODEL} -DENABLE_WEBRTC=false -DENABLE_FFMPEG=true -DENABLE_MOTION=true -DENABLE_AWS_SDK=true -DENABLE_TESTS=false -DENABLE_API=false -DS3_DEPS_JOBS=$(nproc) .. && \
    make -j $(nproc)

# /opt/media/lib holds only the libraries built here (FFmpeg *_s3, AWS SDK); system libraries come
# from apt runtime packages in the final image. ffmpeg/ffprobe go to /usr/local/bin (found through
# PATH): /opt/media/bin/ffmpeg/ is the default ffmpeg log directory.
RUN mkdir -p /opt/media/bin /opt/media/lib /opt/media/ffmpeg-bin && \
    cp /opt/media/S3MediaKit/release/linux/${MODEL}/MediaServer /opt/media/bin/ && \
    cp /opt/media/S3MediaKit/release/linux/3rdpart/ffmpeg/bin/ffmpeg \
       /opt/media/S3MediaKit/release/linux/3rdpart/ffmpeg/bin/ffprobe /opt/media/ffmpeg-bin/ && \
    cp -a /opt/media/S3MediaKit/release/linux/3rdpart/ffmpeg/lib/*.so* \
          /opt/media/S3MediaKit/release/linux/3rdpart/aws-sdk/lib/*.so* /opt/media/lib/ && \
    echo "apt packages needed at runtime:" && \
    /opt/media/S3MediaKit/tools/package/check_runtime_libs.sh --apt-packages \
        /opt/media/lib /opt/media/bin/MediaServer /opt/media/ffmpeg-bin/ffmpeg /opt/media/ffmpeg-bin/ffprobe

FROM ubuntu:22.04
ARG MODEL

# ADD sources.list /etc/apt/sources.list

RUN apt-get update && \
         DEBIAN_FRONTEND="noninteractive" \
         apt-get install -y --no-install-recommends \
         vim \
         wget \
         ca-certificates \
         tzdata \
         curl \
         sqlite3 \
         libssl3 \
         libcurl4 \
         libsqlite3-0 \
         libprotobuf23 \
         zlib1g \
         libfreetype6 \
         librsvg2-2 \
         libx264-163 \
         libx265-199 \
         libva2 \
         libva-drm2 \
         libmfx1 && \
         apt-get autoremove -y && \
         apt-get clean -y && \
    rm -rf /var/lib/apt/lists/*

ENV TZ="Asia/Ho_Chi_Minh"
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime \
        && echo $TZ > /etc/timezone && \
        mkdir -p /opt/media/bin/www

WORKDIR /opt/media/bin/
COPY --from=build /opt/media/bin/ /opt/media/bin/
COPY --from=build /opt/media/lib/ /opt/media/lib/
COPY --from=build /opt/media/ffmpeg-bin/ /usr/local/bin/
COPY --from=build /opt/media/S3MediaKit/default.pem /opt/media/bin/
# COPY --from=build /opt/media/S3MediaKit/release/linux/${MODEL}/config.ini /opt/media/conf/
COPY --from=build /opt/media/S3MediaKit/www/console/ /opt/media/bin/www/console/
COPY --from=build /opt/media/S3MediaKit/migration/mserver_updates/ /opt/media/bin/mserver_updates/
COPY --from=build /opt/media/S3MediaKit/migration/updates/ /opt/media/bin/updates/
COPY --from=build /opt/media/S3MediaKit/tools/package/check_runtime_libs.sh /opt/media/tools/
# Standard loader lookup for /opt/media/lib; RUNPATH $ORIGIN/../lib in the binaries is the fallback.
# The image build fails here, not at run time, when a runtime package is missing.
RUN echo /opt/media/lib > /etc/ld.so.conf.d/s3media.conf && ldconfig && \
    /opt/media/tools/check_runtime_libs.sh --strict /opt/media/lib \
        /opt/media/bin/MediaServer /usr/local/bin/ffmpeg /usr/local/bin/ffprobe
ENV PATH=/opt/media/bin:$PATH
CMD ["./MediaServer","-s", "default.pem", "-c", "../conf/config.ini", "-l", "2", "-d"]