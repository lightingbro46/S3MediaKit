package com.s3mediakit.jni;

public class S3MediaKit {
    static public class MediaFrame{

        /**
         * Returns the decoded timestamp in milliseconds
         */
        public int dts;

        /**
         * Returns the display timestamp in milliseconds
         */
        public int pts;

        /**
         * The prefix length, for example, the prefix is ​​0x00 00 00 01, then the prefix length is 4
         * The aac prefix is ​​7 bytes
         */
        public int prefixSize;

        /**
         * Returns whether it is a keyframe
         */
        public boolean keyFrame;

        /**
         * Audio and video data
         */
        public byte[] data;

        /**
         * Is it audio or video
         * typedef enum {
         *     TrackInvalid = -1,
         *     TrackVideo = 0,
         *     TrackAudio,
         *     TrackTitle,
         *     TrackMax = 0x7FFF
         * } TrackType;
         */
        public int trackType;


        /**
         * Coding type
         * typedef enum {
         *     CodecInvalid = -1,
         *     CodecH264 = 0,
         *     CodecH265,
         *     CodecAAC,
         *     CodecMax = 0x7FFF
         * } CodecId;
         */
        public int codecId;
    }

    static public interface MediaPlayerCallBack{
        void onPlayResult(int code,String msg);
        void onShutdown(int code,String msg);
        void onData(MediaFrame frame);
    };


    static public class MediaPlayer{
        private long _ptr;
        private MediaPlayerCallBack _callback;
        public MediaPlayer(String url,MediaPlayerCallBack callBack){
            _callback = callBack;
            _ptr = createMediaPlayer(url,callBack);
        }
        public void release(){
            if(_ptr != 0){
                releaseMediaPlayer(_ptr);
                _ptr = 0;
            }
        }

        @Override
        protected void finalize() throws Throwable {
            super.finalize();
            release();
        }
    }

    static public native boolean startDemo(String sd_path);
    static public native void releaseMediaPlayer(long ptr);
    static public native long createMediaPlayer(String url,MediaPlayerCallBack callback);

    static {
        System.loadLibrary("s3mediakit_jni");
    }
}
