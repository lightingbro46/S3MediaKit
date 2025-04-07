#include <signal.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include "windows.h"
#else
#include "unistd.h"
#endif
#include "mk_mediakit.h"

static int exit_flag = 0;
static void s_on_exit(int sig) {
    exit_flag = 1;
}

static void on_h264_frame(void *user_data, mk_h264_splitter splitter, const char *data, int size) {
#ifdef _WIN32
    Sleep(40);
#else
    usleep(40 * 1000);
#endif
    static int dts = 0;
    mk_frame frame = mk_frame_create(MKCodecH264, dts, dts, data, size, NULL, NULL);
    dts += 40;
    mk_media_input_frame((mk_media)user_data, frame);
    mk_frame_unref(frame);
}

typedef struct {
    mk_pusher pusher;
    char *url;
} Context;

void release_context(void *user_data) {
    Context *ptr = (Context *)user_data;
    if (ptr->pusher) {
        mk_pusher_release(ptr->pusher);
    }
    free(ptr->url);
    free(ptr);
    log_info("Stop pushing");
}

void on_push_result(void *user_data, int err_code, const char *err_msg) {
    Context *ptr = (Context *)user_data;
    if (err_code == 0) {
        log_info("Successful push: %s", ptr->url);
    } else {
        log_warn("Pushing %s failed: %d(%s)", ptr->url, err_code, err_msg);
    }
}

void on_push_shutdown(void *user_data, int err_code, const char *err_msg) {
    Context *ptr = (Context *)user_data;
    log_warn("Push %s interrupt: %d(%s)", ptr->url, err_code, err_msg);
}

void API_CALL on_regist(void *user_data, mk_media_source sender, int regist) {
    Context *ptr = (Context *)user_data;
    const char *schema = mk_media_source_get_schema(sender);
    if (strstr(ptr->url, schema) != ptr->url) {
        // Protocol matching failed
        return;
    }

    if (!regist) {
        // Log out
        if (ptr->pusher) {
            mk_pusher_release(ptr->pusher);
            ptr->pusher = NULL;
        }
    } else {
        // Register
        if (!ptr->pusher) {
            ptr->pusher = mk_pusher_create_src(sender);
            mk_pusher_set_on_result2(ptr->pusher, on_push_result, ptr, NULL);
            mk_pusher_set_on_shutdown2(ptr->pusher, on_push_shutdown, ptr, NULL);
            // Start streaming
            mk_pusher_publish(ptr->pusher, ptr->url);
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        log_error("Usage: /path/to/h264/file rtsp_or_rtmp_url");
        return -1;
    }
    mk_config config = { .ini = NULL,
                         .ini_is_path = 1,
                         .log_level = 0,
                         .log_mask = LOG_CONSOLE,
                         .log_file_path = NULL,
                         .log_file_days = 0,
                         .ssl = NULL,
                         .ssl_is_path = 1,
                         .ssl_pwd = NULL,
                         .thread_num = 0 };
    mk_env_init(&config);

    FILE *fp = fopen(argv[1], "rb");
    if (!fp) {
        log_error("Failed to open the file!");
        return -1;
    }

    mk_media media = mk_media_create("__defaultVhost__", "live", "test", 0, 0, 0);
    // h264 codec
    codec_args v_args = { 0 };
    mk_track v_track = mk_track_create(MKCodecH264, &v_args);
    mk_media_init_track(media, v_track);
    mk_media_init_complete(media);
    mk_track_unref(v_track);

    Context *ctx = (Context *)malloc(sizeof(Context));
    memset(ctx, 0, sizeof(Context));
    ctx->url = strdup(argv[2]);

    mk_media_set_on_regist2(media, on_regist, ctx, release_context);

    // Create h264 frame splitter
    mk_h264_splitter splitter = mk_h264_splitter_create(on_h264_frame, media, 0);
    signal(SIGINT, s_on_exit); // Set the exit signal
    signal(SIGTERM, s_on_exit); // Set the exit signal

    char buf[1024];
    while (!exit_flag) {
        int size = fread(buf, 1, sizeof(buf) - 1, fp);
        if (size > 0) {
            mk_h264_splitter_input_data(splitter, buf, size);
        } else {
            // File read finished, start again
            fseek(fp, 0, SEEK_SET);
        }
    }

    log_info("File reading is complete");
    mk_h264_splitter_release(splitter);
    mk_media_release(media);
    fclose(fp);
    return 0;
}
