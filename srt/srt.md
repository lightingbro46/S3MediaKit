## Features
- NACK (retransmission)
- listener Support
- Pushing flow only supports ts push flow
- Pulling stream only supports ts pull stream
- Protocol Implementation [Reference](https://haivision.github.io/srt-rfc/draft-sharabayko-srt.html)
- Version support (>=1.3.0)
- fec has not been implemented
## use

The srt in s3m determines whether it is a streaming or a streaming based on streamid to determine vhost, app, streamid (in S3M),

The streamid in srt is `#!::key1=value1,key2=value2,key3=value4......`

h,r is a special key to determine vhost, app, streamid. If there is no h, vhost is the default value

m is a special key to determine whether it is a push stream or a pull stream. If it is publish, it is a push stream, otherwise it is a pull stream. If m does not exist, it is a pull stream.

Other keys and m will be used as authentication parameters for webhook

like:
  #!::h=s3mediakit.com,r=live/test,m=publish

  vhost = s3mediakit.com

  app = live

  streamid = test

  It's push


- OBS streaming address

    `srt://192.168.1.105:9000?streamid=#!::r=live/test,m=publish`
- ffmpeg flow

    `ffmpeg -re -stream_loop -1 -i test.ts -c:v copy -c:a copy -f mpegts srt://192.168.1.105:9000?streamid=#!::r=live/test,m=publish`
- ffplay pull stream

    `ffplay -i srt://192.168.1.105:9000?streamid=#!::r=live/test`

- vlc pull stream
    - vlc pull streaming needs to set streamid in preferences -> stream output -> access output -> SRT, for example `#!::r=live/test`
    - When pulling the stream, just fill in `srt://192.168.1.105:9000`