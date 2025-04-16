## Parameters that affect performance of configuration files

### 1、protocol.enable_xxx 
Control the to-protocol switch, turn off certain protocols to save CPU and memory.

### 2、protocol.xxx_demand
Control the on-demand transfer protocol. When the on-demand transfer protocol is enabled and the on-demand transfer protocol is transferred, the CPU and memory will be saved when no one watches, but the first player cannot be opened in seconds, which will affect the experience.

### 3、protocol.paced_sender_ms
The smooth sending timer frequency is used to solve the problem of unsmooth forwarding of the player caused by unsmooth transmission of data sources. After turning on, the timer drives data transmission based on the data timestamp to improve user experience.
But increase CPU and memory usage. The smaller the timer interval, the higher the CPU occupies, but the better the smoothness. It is recommended to set it to 30~100ms. This function combines protocol.modify_stamp with 2 (suppress timestamp jumps).

### 4、general.mergeWriteMS 
Turn on merge write to reduce the number of system calls when sending data and the frequency of data sharing between threads, greatly improving forwarding performance, but sacrificing playback delay and sending smoothness.

### 5、rtp_proxy.gop_cache
Turn on the gop cache function of the startSendRtp cascade interface, which is used to open the national standard cascade in seconds. This option does not affect the instant opening of s3mediakit's live broadcast service provided to the outside world.
If you turn on this option, increase memory usage will have little impact on CPU. If you do not call the startSendRtp interface, it is recommended to turn it off.

### 6、hls.fileBufSize
Adjusting this configuration can improve the performance of HLS protocol write disk io.

### 7、record.fileBufSize
Adjusting this configuration can improve mp4 recording and writing disk io performance.
