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
         libavcodec-dev \
         libavformat-dev \
         libavutil-dev \
         libswscale-dev \
         libswresample-dev \
         libavfilter-dev \
         libfreetype6-dev \
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

# # 3rdpart init
# WORKDIR /opt/media/ZLMediaKit/3rdpart
# RUN wget https://github.com/cisco/libsrtp/archive/v2.3.0.tar.gz -O libsrtp-2.3.0.tar.gz && \
#     tar xfv libsrtp-2.3.0.tar.gz && \
#     mv libsrtp-2.3.0 libsrtp && \
#     cd libsrtp && CFLAGS="-fcommon" ./configure --enable-openssl && \
#     make -j$(nproc) && make install

# 3rdpart init
# WORKDIR /opt/media/S3MediaKit/3rdpart
# RUN wget --no-check-certificate https://github.com/cisco/libsrtp/archive/v2.3.0.tar.gz -O libsrtp-2.3.0.tar.gz && \
#     tar xfv libsrtp-2.3.0.tar.gz && \
#     mv libsrtp-2.3.0 libsrtp && \
#     cd libsrtp && ./configure --enable-openssl && make -j $(nproc) && make install
# 3rdpart aws-sdk
# RUN apt-get install -y --no-install-recommends zlib1g-dev
# WORKDIR /opt/media/S3MediaKit/3rdpart
# RUN git clone --branch main --single-branch --recurse-submodules https://github.com/aws/aws-sdk-cpp.git aws-sdk-cpp && \
#     cd aws-sdk-cpp && mkdir build && mkdir -p ../aws-sdk/bin && \
#     cd build && \
#     cmake ..    \ 
#     -DCMAKE_BUILD_TYPE=Release \
#     -DBUILD_SHARED_LIBS=ON \
#     -DBUILD_ONLY="s3;core" \
#     -DENABLE_TESTING=OFF \
#     -DAUTORUN_UNIT_TESTS=OFF \
#     -DCMAKE_INSTALL_PREFIX="../../aws-sdk/bin"  \
#     && make -j$(nproc) && make install

#RUN git submodule update --init --recursive && \

RUN mkdir -p build release/linux/${MODEL}/

WORKDIR /opt/media/S3MediaKit/build
RUN cmake -DCMAKE_BUILD_TYPE=${MODEL} -DENABLE_WEBRTC=false -DENABLE_FFMPEG=true -DENABLE_MOTION=true -DENABLE_AWS_SDK=true -DENABLE_TESTS=false -DENABLE_API=false .. && \
    make -j $(nproc)

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
         ffmpeg && \
         apt-get autoremove -y && \
         apt-get clean -y && \
    rm -rf /var/lib/apt/lists/*

ENV TZ="Asia/Ho_Chi_Minh"
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime \
        && echo $TZ > /etc/timezone && \
        mkdir -p /opt/media/bin/www

WORKDIR /opt/media/bin/
COPY --from=build /opt/media/S3MediaKit/release/linux/${MODEL}/MediaServer /opt/media/S3MediaKit/default.pem /opt/media/bin/
# COPY --from=build /opt/media/S3MediaKit/release/linux/${MODEL}/config.ini /opt/media/conf/
COPY --from=build /opt/media/S3MediaKit/www/console/ /opt/media/bin/www/console/
COPY --from=build /opt/media/S3MediaKit/migration/mserver_updates/ /opt/media/bin/mserver_updates/
COPY --from=build /opt/media/S3MediaKit/migration/updates/ /opt/media/bin/updates/
# Copy AWS SDK shared libraries alongside the binary so RPATH=$ORIGIN resolves them
COPY --from=build /opt/media/S3MediaKit/3rdpart/aws-sdk/bin/lib/* /opt/media/bin/
ENV PATH=/opt/media/bin:$PATH
CMD ["./MediaServer","-s", "default.pem", "-c", "../conf/config.ini", "-l", "2", "-d"]