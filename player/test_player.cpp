#include <signal.h>
#include "Util/logger.h"
#include "Util/util.h"
#include <iostream>
#include "Common/config.h"
#include "Rtsp/UDPServer.h"
#include "Player/MediaPlayer.h"
#include "Util/onceToken.h"
#include "Codec/Transcode.h"
#include "YuvDisplayer.h"
#include "AudioSRC.h"
using namespace std;
using namespace toolkit;
using namespace mediakit;

#ifdef WIN32
#include <TCHAR.h>

extern int __argc;
extern TCHAR** __targv;

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstanc, LPSTR lpCmdLine, int nShowCmd) {
    int argc = __argc;
    char **argv = __targv;

    //1. First, call AllocConsole to create a console window
    AllocConsole();

    //2. However, calling cout or printf at this time cannot output text to the window (including input streams cin and scanf), so the input and output stream needs to be redirected as follows:
    FILE* stream;
    freopen_s(&stream, "CON", "r", stdin);//Redirecting the input stream
    freopen_s(&stream, "CON", "w", stdout);//Redirecting the input stream

    //3. If we need to use the console window handle, we can call FindWindow to get it：
    HWND _consoleHwnd;
    SetConsoleTitleA("test_player");//Set the window name
#else
#include <unistd.h>
int main(int argc, char *argv[]) {
#endif
    static char *url = argv[1];
    {
        // Set the exit signal processing function
        signal(SIGINT, [](int) { SDLDisplayerHelper::Instance().shutdown(); });
        // Setting up logs
        Logger::Instance().add(std::make_shared<ConsoleChannel>());
        Logger::Instance().setWriter(std::make_shared<AsyncLogWriter>());

        if (argc < 3) {
            ErrorL << "\r\nTest Method：./test_player rtxp_url rtp_type\r\n"
                   << "For example: ./test_player rtsp://admin:123456@127.0.0.1/live/0 0\r\n";
            return 0;
        }

        auto player = std::make_shared<MediaPlayer>();
        // sdl requires initialization in main thread
        auto displayer = std::make_shared<YuvDisplayer>(nullptr, url);
        weak_ptr<MediaPlayer> weakPlayer = player;
        player->setOnPlayResult([weakPlayer, displayer](const SockException &ex) {
            InfoL << "OnPlayResult:" << ex.what();
            auto strongPlayer = weakPlayer.lock();
            if (ex || !strongPlayer) {
                return;
            }

            auto videoTrack = dynamic_pointer_cast<VideoTrack>(strongPlayer->getTrack(TrackVideo, false));
            auto audioTrack = dynamic_pointer_cast<AudioTrack>(strongPlayer->getTrack(TrackAudio, false));

            if (videoTrack) {
                auto decoder = std::make_shared<FFmpegDecoder>(videoTrack);
                decoder->setOnDecode([displayer](const FFmpegFrame::Ptr &yuv) {
                    SDLDisplayerHelper::Instance().doTask([yuv, displayer]() {
                        // sdl requires rendering in main thread
                        displayer->displayYUV(yuv->get());
                        return true;
                    });
                });
                videoTrack->addDelegate([decoder](const Frame::Ptr &frame) { return decoder->inputFrame(frame, false, true); });
            }

            if (audioTrack) {
                auto decoder = std::make_shared<FFmpegDecoder>(audioTrack);
                auto audio_player = std::make_shared<AudioPlayer>();
                // FFmpeg is uniformly converted to 16-bit integer pcm when decoding
                audio_player->setup(audioTrack->getAudioSampleRate(), audioTrack->getAudioChannel(), AUDIO_S16);
                FFmpegSwr::Ptr swr;

                decoder->setOnDecode([audio_player, swr](const FFmpegFrame::Ptr &frame) mutable {
                    int chs = 0;
                    if (!swr) {
# if LIBAVCODEC_VERSION_INT >= FF_CODEC_VER_7_1
                        swr = std::make_shared<FFmpegSwr>(AV_SAMPLE_FMT_S16, &(frame->get()->ch_layout), frame->get()->sample_rate);
                        chs = (&frame->get()->ch_layout)->nb_channels;
#else
                        swr = std::make_shared<FFmpegSwr>(AV_SAMPLE_FMT_S16, frame->get()->channels, frame->get()->channel_layout, frame->get()->sample_rate);
                        chs = frame->get()->channels;
#endif
                    }
                    auto pcm = swr->inputFrame(frame);
                    auto len = pcm->get()->nb_samples * chs * av_get_bytes_per_sample((enum AVSampleFormat)pcm->get()->format);
                    audio_player->playPCM((const char *)(pcm->get()->data[0]), MIN(len, frame->get()->linesize[0]));
                });
                audioTrack->addDelegate([decoder](const Frame::Ptr &frame) { return decoder->inputFrame(frame, false, true); });
            }
        });

        player->setOnShutdown([](const SockException &ex) { WarnL << "play shutdown: " << ex.what(); });

        (*player)[Client::kRtpType] = atoi(argv[2]);
        // Don't wait for track ready to callback and playback successfully, which can speed up the second opening speed
        (*player)[Client::kWaitTrackReady] = false;
        if (argc > 3) {
            (*player)[Client::kPlayTrack] = atoi(argv[3]);
        }
        player->play(argv[1]);
        SDLDisplayerHelper::Instance().runLoop();
    }
    sleep(1);
    return 0;
}

