FROM ubuntu:20.04 AS build
ARG MODEL
#shell,rtmp,rtsp,rtsps,http,https,rtp
EXPOSE 1935/tcp
EXPOSE 554/tcp
EXPOSE 80/tcp
EXPOSE 443/tcp
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
         libcurl4-openssl-dev \
         gcc \
         g++ \
         gdb && \
         apt-get autoremove -y && \
         apt-get clean -y && \
         rm -rf /var/lib/apt/lists/*

RUN mkdir -p /opt/media
COPY . /opt/media/S3MediaKit
WORKDIR /opt/media/S3MediaKit

# 3rdpart init
WORKDIR /opt/media/S3MediaKit/3rdpart
RUN wget https://github.com/cisco/libsrtp/archive/v2.3.0.tar.gz -O libsrtp-2.3.0.tar.gz && \
    tar xfv libsrtp-2.3.0.tar.gz && \
    mv libsrtp-2.3.0 libsrtp && \
    cd libsrtp && ./configure --enable-openssl && make -j $(nproc) && make install
#RUN git submodule update --init --recursive && \

RUN mkdir -p build release/linux/${MODEL}/

WORKDIR /opt/media/S3MediaKit/build
RUN cmake -DCMAKE_BUILD_TYPE=${MODEL} -DENABLE_WEBRTC=true -DENABLE_FFMPEG=true -DENABLE_TESTS=false -DENABLE_API=false .. && \
    make -j $(nproc)

FROM ubuntu:20.04
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
         libssl-dev \
         libprotobuf-dev \
         libsqlite3-dev \
         libcurl4-openssl-dev \
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
COPY --from=build /opt/media/S3MediaKit/release/linux/${MODEL}/config.ini /opt/media/conf/
COPY --from=build /opt/media/S3MediaKit/www/ /opt/media/bin/www/
ENV PATH=/opt/media/bin:$PATH
CMD ["./MediaServer","-s", "default.pem", "-c", "../conf/config.ini", "-l", "3"]
