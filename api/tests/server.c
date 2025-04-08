#include <string.h>
#include <stdio.h>
#include "mk_mediakit.h"
#define LOG_LEV 4

/**
 * Register or unregister MediaSource event broadcast
 * @param regist Register as 1, unregister as 0
 * @param sender The MediaSource object
 */
void API_CALL on_mk_media_changed(int regist,
                                  const mk_media_source sender) {
    log_printf(LOG_LEV,"%d %s/%s/%s/%s",(int)regist,
              mk_media_source_get_schema(sender),
              mk_media_source_get_vhost(sender),
              mk_media_source_get_app(sender),
              mk_media_source_get_stream(sender));

}

/**
 * Receive rtsp/rtmp push stream event broadcast, control push stream authentication through this event
 * @see mk_publish_auth_invoker_do
 * @param url_info Push stream url related information
 * @param invoker Execute invoker to return authentication result
 * @param sender The tcp client related information
 */
void API_CALL on_mk_media_publish(const mk_media_info url_info,
                                  const mk_publish_auth_invoker invoker,
                                  const mk_sock_info sender) {
    char ip[64];
    log_printf(LOG_LEV,
               "client info, local: %s:%d, peer: %s:%d\n"
               "%s/%s/%s/%s, url params: %s",
               mk_sock_info_local_ip(sender,ip),
               mk_sock_info_local_port(sender),
               mk_sock_info_peer_ip(sender,ip + 32),
               mk_sock_info_peer_port(sender),
               mk_media_info_get_schema(url_info),
               mk_media_info_get_vhost(url_info),
               mk_media_info_get_app(url_info),
               mk_media_info_get_stream(url_info),
               mk_media_info_get_params(url_info));

    // Allow push stream, and allow to convert to hls/mp4
    mk_publish_auth_invoker_do(invoker, NULL, 1, 1);
}

/**
 * Play rtsp/rtmp/http-flv/hls event broadcast, control playback authentication through this event
 * @see mk_auth_invoker_do
 * @param url_info Play url related information
 * @param invoker Execute invoker to return authentication result
 * @param sender Play client related information
 */
void API_CALL on_mk_media_play(const mk_media_info url_info,
                               const mk_auth_invoker invoker,
                               const mk_sock_info sender) {

    char ip[64];
    log_printf(LOG_LEV,
               "client info, local: %s:%d, peer: %s:%d\n"
               "%s/%s/%s/%s, url params: %s",
               mk_sock_info_local_ip(sender,ip),
               mk_sock_info_local_port(sender),
               mk_sock_info_peer_ip(sender,ip + 32),
               mk_sock_info_peer_port(sender),
               mk_media_info_get_schema(url_info),
               mk_media_info_get_vhost(url_info),
               mk_media_info_get_app(url_info),
               mk_media_info_get_stream(url_info),
               mk_media_info_get_params(url_info));

    // Allow playback
    mk_auth_invoker_do(invoker, NULL);
}

/**
 * This event will be broadcast after the stream is not found. Please pull the stream or other methods to generate the stream after listening to this event, so that you can pull the stream on demand
 * @param url_info Play url related information
 * @param sender Play client related information
 * @return 1 Close directly
 *         0 Wait for stream registration
 */
int API_CALL on_mk_media_not_found(const mk_media_info url_info,
                                    const mk_sock_info sender) {
    char ip[64];
    log_printf(LOG_LEV,
               "client info, local: %s:%d, peer: %s:%d\n"
               "%s/%s/%s/%s, url params: %s",
               mk_sock_info_local_ip(sender,ip),
               mk_sock_info_local_port(sender),
               mk_sock_info_peer_ip(sender,ip + 32),
               mk_sock_info_peer_port(sender),
               mk_media_info_get_schema(url_info),
               mk_media_info_get_vhost(url_info),
               mk_media_info_get_app(url_info),
               mk_media_info_get_stream(url_info),
               mk_media_info_get_params(url_info));
    return 0;
}

/**
 * Triggered when no one consumes a certain stream, the purpose is to achieve business logic such as actively disconnecting the pull stream when no one is watching
 * @param sender The MediaSource object
 */
void API_CALL on_mk_media_no_reader(const mk_media_source sender) {
    log_printf(LOG_LEV,
               "%s/%s/%s/%s",
               mk_media_source_get_schema(sender),
               mk_media_source_get_vhost(sender),
               mk_media_source_get_app(sender),
               mk_media_source_get_stream(sender));
}

// Escape webrtc answer sdp according to json escape rules
static char *escape_string(const char *ptr){
    char *escaped = malloc(2 * strlen(ptr));
    char *ptr_escaped = escaped;
    while (1) {
        switch (*ptr) {
            case '\r': {
                *(ptr_escaped++) = '\\';
                *(ptr_escaped++) = 'r';
                break;
            }
            case '\n': {
                *(ptr_escaped++) = '\\';
                *(ptr_escaped++) = 'n';
                break;
            }
            case '\t': {
                *(ptr_escaped++) = '\\';
                *(ptr_escaped++) = 't';
                break;
            }

            default: {
                *(ptr_escaped++) = *ptr;
                if (!*ptr) {
                    return escaped;
                }
                break;
            }
        }
        ++ptr;
    }
}

static void on_mk_webrtc_get_answer_sdp_func(void *user_data, const char *answer, const char *err) {
    const char *response_header[] = { "Content-Type", "application/json", "Access-Control-Allow-Origin", "*" , NULL};
    if (answer) {
        answer = escape_string(answer);
    }
    size_t len = answer ? 2 * strlen(answer) : 1024;
    char *response_content = (char *)malloc(len);

    if (answer) {
        snprintf(response_content, len,
                 "{"
                 "\"sdp\":\"%s\","
                 "\"type\":\"answer\","
                 "\"code\":0"
                 "}",
                 answer);
    } else {
        snprintf(response_content, len,
                 "{"
                 "\"msg\":\"%s\","
                 "\"code\":-1"
                 "}",
                 err);
    }

    mk_http_response_invoker_do_string(user_data, 200, response_header, response_content);
    mk_http_response_invoker_clone_release(user_data);
    free(response_content);
    if (answer) {
        free((void *)answer);
    }
}

void API_CALL on_get_statistic_cb(void *user_data, mk_ini ini) {
    const char *response_header[] = { NULL };
    char *str = mk_ini_dump_string(ini);
    mk_http_response_invoker_do_string(user_data, 200, response_header, str);
    mk_free(str);
}

/**
 * Receive http api request broadcast (including GET/POST)
 * @param parser Http request content object
 * @param invoker Execute this invoker to return http reply
 * @param consumed Set to 1 if we want to handle this event
 * @param sender Http client related information
 */
// Test url : http://127.0.0.1/api/test
void API_CALL on_mk_http_request(const mk_parser parser,
                                 const mk_http_response_invoker invoker,
                                 int *consumed,
                                 const mk_sock_info sender) {

    char ip[64];
    log_printf(LOG_LEV,
               "client info, local: %s:%d, peer: %s:%d\n"
               "%s %s?%s %s\n"
               "User-Agent: %s\n"
               "%s",
               mk_sock_info_local_ip(sender,ip),
               mk_sock_info_local_port(sender),
               mk_sock_info_peer_ip(sender,ip + 32),
               mk_sock_info_peer_port(sender),
               mk_parser_get_method(parser),
               mk_parser_get_url(parser),
               mk_parser_get_url_params(parser),
               mk_parser_get_tail(parser),
               mk_parser_get_header(parser, "User-Agent"),
               mk_parser_get_content(parser,NULL));

    const char *url = mk_parser_get_url(parser);
    *consumed = 1;

    // Intercept api: /api/test
    if (strcmp(url, "/api/test") == 0) {
        const char *response_header[] = { "Content-Type", "text/html", NULL };
        const char *content = "<html>"
                              "<head>"
                              "<title>hello world</title>"
                              "</head>"
                              "<body bgcolor=\"white\">"
                              "<center><h1>hello world</h1></center><hr>"
                              "<center>"
                              "S3MediaKit-4.0</center>"
                              "</body>"
                              "</html>";
        mk_http_body body = mk_http_body_from_string(content, 0);
        mk_http_response_invoker_do(invoker, 200, response_header, body);
        mk_http_body_release(body);
    } else if (strcmp(url, "/index/api/webrtc") == 0) {
        // Intercept api: /index/api/webrtc
        char rtc_url[1024];
        snprintf(rtc_url, sizeof(rtc_url), "rtc://%s/%s/%s?%s", mk_parser_get_header(parser, "Host"),
                 mk_parser_get_url_param(parser, "app"), mk_parser_get_url_param(parser, "stream"),
                 mk_parser_get_url_params(parser));

        mk_webrtc_get_answer_sdp(mk_http_response_invoker_clone(invoker), on_mk_webrtc_get_answer_sdp_func,
                                 mk_parser_get_url_param(parser, "type"), mk_parser_get_content(parser, NULL), rtc_url);
    } else if (strcmp(url, "/index/api/getStatistic") == 0) {
        // Intercept api: /index/api/webrtc
        mk_get_statistic(on_get_statistic_cb, mk_http_response_invoker_clone(invoker), (on_user_data_free) mk_http_response_invoker_clone_release);
    } else {
        *consumed = 0;
        return;
    }
}

/**
 * In the http file server, receive the broadcast of http access to files or directories, control the access permission of http directory through this event
 * @param parser Http request content object
 * @param path File absolute path
 * @param is_dir Whether path is a folder
 * @param invoker Execute invoker to return the result of accessing the file this time
 * @param sender Http client related information
 */
void API_CALL on_mk_http_access(const mk_parser parser,
                                const char *path,
                                int is_dir,
                                const mk_http_access_path_invoker invoker,
                                const mk_sock_info sender) {

    char ip[64];
    log_printf(LOG_LEV,
               "client info, local: %s:%d, peer: %s:%d, path: %s ,is_dir: %d\n"
               "%s %s?%s %s\n"
               "User-Agent: %s\n"
               "%s",
               mk_sock_info_local_ip(sender, ip),
               mk_sock_info_local_port(sender),
               mk_sock_info_peer_ip(sender, ip + 32),
               mk_sock_info_peer_port(sender),
               path,(int)is_dir,
               mk_parser_get_method(parser),
               mk_parser_get_url(parser),
               mk_parser_get_url_params(parser),
               mk_parser_get_tail(parser),
               mk_parser_get_header(parser,"User-Agent"),
               mk_parser_get_content(parser,NULL));

    // Has access permission, each access to the file needs authentication
    mk_http_access_path_invoker_do(invoker, NULL, NULL, 0);
}

/**
 * In the http file server, receive the broadcast before http access to files or directories, through this event you can control the mapping of http url to file path
 * By overriding the path parameter in this event, you can achieve the purpose of selecting different http root directories according to virtual hosts or apps
 * @param parser Http request content object
 * @param path File absolute path, override it to redirect to other files
 * @param sender Http client related information
 */
void API_CALL on_mk_http_before_access(const mk_parser parser,
                                       char *path,
                                       const mk_sock_info sender) {

    char ip[64];
    log_printf(LOG_LEV,
               "client info, local: %s:%d, peer: %s:%d, path: %s\n"
               "%s %s?%s %s\n"
               "User-Agent: %s\n"
               "%s",
               mk_sock_info_local_ip(sender,ip),
               mk_sock_info_local_port(sender),
               mk_sock_info_peer_ip(sender,ip + 32),
               mk_sock_info_peer_port(sender),
               path,
               mk_parser_get_method(parser),
               mk_parser_get_url(parser),
               mk_parser_get_url_params(parser),
               mk_parser_get_tail(parser),
               mk_parser_get_header(parser, "User-Agent"),
               mk_parser_get_content(parser,NULL));
    // Overriding the value of path can redirect files
}

/**
 * Does this rtsp stream need authentication? If so, call invoker and pass in realm, otherwise pass in empty realm
 * @param url_info Request rtsp url related information
 * @param invoker Execute invoker to return whether rtsp exclusive authentication is required
 * @param sender Rtsp client related information
 */
void API_CALL on_mk_rtsp_get_realm(const mk_media_info url_info,
                                   const mk_rtsp_get_realm_invoker invoker,
                                   const mk_sock_info sender) {
    char ip[64];
    log_printf(LOG_LEV,
               "client info, local: %s:%d, peer: %s:%d\n"
               "%s/%s/%s/%s, url params: %s",
               mk_sock_info_local_ip(sender,ip),
               mk_sock_info_local_port(sender),
               mk_sock_info_peer_ip(sender,ip + 32),
               mk_sock_info_peer_port(sender),
               mk_media_info_get_schema(url_info),
               mk_media_info_get_vhost(url_info),
               mk_media_info_get_app(url_info),
               mk_media_info_get_stream(url_info),
               mk_media_info_get_params(url_info));

    // Rtsp playback default authentication
    mk_rtsp_get_realm_invoker_do(invoker, "s3mediakit");
}

/**
 * Request authentication user password event, user_name is the username, must_no_encrypt if it is 1, then you must provide plain text password (because it is base64 authentication method at this time), otherwise it will lead to authentication failure
 * After getting the password, please call invoker and enter the corresponding type of password and password type, invoker will match the password when executing
 * @param url_info Request rtsp url related information
 * @param realm Rtsp authentication realm
 * @param user_name Rtsp authentication username
 * @param must_no_encrypt If it is 1, then you must provide plain text password (because it is base64 authentication method at this time), otherwise it will lead to authentication failure
 * @param invoker  Execute invoker to return the password of rtsp exclusive authentication
 * @param sender Rtsp client information
 */
void API_CALL on_mk_rtsp_auth(const mk_media_info url_info,
                              const char *realm,
                              const char *user_name,
                              int must_no_encrypt,
                              const mk_rtsp_auth_invoker invoker,
                              const mk_sock_info sender) {

    char ip[64];
    log_printf(LOG_LEV,
               "client info, local: %s:%d, peer: %s:%d\n"
               "%s/%s/%s/%s, url params: %s\n"
               "realm: %s, user_name: %s, must_no_encrypt: %d",
               mk_sock_info_local_ip(sender,ip),
               mk_sock_info_local_port(sender),
               mk_sock_info_peer_ip(sender,ip + 32),
               mk_sock_info_peer_port(sender),
               mk_media_info_get_schema(url_info),
               mk_media_info_get_vhost(url_info),
               mk_media_info_get_app(url_info),
               mk_media_info_get_stream(url_info),
               mk_media_info_get_params(url_info),
               realm,user_name,(int)must_no_encrypt);

    // Rtsp playback username and password are consistent
    mk_rtsp_auth_invoker_do(invoker,0,user_name);
}

/**
 * Broadcast after recording mp4 fragment file successfully
 */
void API_CALL on_mk_record_mp4(const mk_record_info mp4) {
    log_printf(LOG_LEV,
               "\nstart_time: %d\n"
               "time_len: %d\n"
               "file_size: %d\n"
               "file_path: %s\n"
               "file_name: %s\n"
               "folder: %s\n"
               "url: %s\n"
               "vhost: %s\n"
               "app: %s\n"
               "stream: %s\n",
               mk_record_info_get_start_time(mp4),
               mk_record_info_get_time_len(mp4),
               mk_record_info_get_file_size(mp4),
               mk_record_info_get_file_path(mp4),
               mk_record_info_get_file_name(mp4),
               mk_record_info_get_folder(mp4),
               mk_record_info_get_url(mp4),
               mk_record_info_get_vhost(mp4),
               mk_record_info_get_app(mp4),
               mk_record_info_get_stream(mp4));
}

/**
 * Shell login authentication
 */
void API_CALL on_mk_shell_login(const char *user_name,
                                const char *passwd,
                                const mk_auth_invoker invoker,
                                const mk_sock_info sender) {

    char ip[64];
    log_printf(LOG_LEV,"client info, local: %s:%d, peer: %s:%d\n"
              "user_name: %s, passwd: %s",
              mk_sock_info_local_ip(sender,ip),
              mk_sock_info_local_port(sender),
              mk_sock_info_peer_ip(sender,ip + 32),
              mk_sock_info_peer_port(sender),
              user_name, passwd);
    // Allow login shell
    mk_auth_invoker_do(invoker, NULL);
}

/**
 * Stop rtsp/rtmp/http-flv session after traffic report event broadcast
 * @param url_info Play url related information
 * @param total_bytes Total traffic consumed up and down, unit is byte
 * @param total_seconds The duration of this tcp session, unit is second
 * @param is_player Whether the client is a player
 * @param peer_ip Client ip
 * @param peer_port Client port number
 */
void API_CALL on_mk_flow_report(const mk_media_info url_info,
                                size_t total_bytes,
                                size_t total_seconds,
                                int is_player,
                                const mk_sock_info sender) {
    char ip[64];
    log_printf(LOG_LEV,"%s/%s/%s/%s, url params: %s,"
              "total_bytes: %d, total_seconds: %d, is_player: %d, peer_ip:%s, peer_port:%d",
               mk_media_info_get_schema(url_info),
               mk_media_info_get_vhost(url_info),
               mk_media_info_get_app(url_info),
               mk_media_info_get_stream(url_info),
               mk_media_info_get_params(url_info),
              (int)total_bytes,
              (int)total_seconds,
              (int)is_player,
              mk_sock_info_peer_ip(sender,ip),
              (int)mk_sock_info_peer_port(sender));
}

int main(int argc, char *argv[]) {
    char *ini_path = mk_util_get_exe_dir("c_api.ini");
    char *ssl_path = mk_util_get_exe_dir("ssl.p12");

    mk_config config = {
            .ini = ini_path,
            .ini_is_path = 1,
            .log_level = 0,
            .log_mask = LOG_CONSOLE,
            .log_file_path = NULL,
            .log_file_days = 0,
            .ssl = ssl_path,
            .ssl_is_path = 1,
            .ssl_pwd = NULL,
            .thread_num = 0
    };
    mk_env_init(&config);
    free(ini_path);
    free(ssl_path);

    mk_http_server_start(80, 0);
    mk_http_server_start(443, 1);
    mk_rtsp_server_start(554, 0);
    mk_rtmp_server_start(1935, 0);
    mk_shell_server_start(9000);
    mk_rtp_server_start(10000);
    mk_rtc_server_start(8000);
    mk_srt_server_start(9000);

    mk_events events = {
            .on_mk_media_changed = on_mk_media_changed,
            .on_mk_media_publish = on_mk_media_publish,
            .on_mk_media_play = on_mk_media_play,
            .on_mk_media_not_found = on_mk_media_not_found,
            .on_mk_media_no_reader = on_mk_media_no_reader,
            .on_mk_http_request = on_mk_http_request,
            .on_mk_http_access = on_mk_http_access,
            .on_mk_http_before_access = on_mk_http_before_access,
            .on_mk_rtsp_get_realm = on_mk_rtsp_get_realm,
            .on_mk_rtsp_auth = on_mk_rtsp_auth,
            .on_mk_record_mp4 = on_mk_record_mp4,
            .on_mk_shell_login = on_mk_shell_login,
            .on_mk_flow_report = on_mk_flow_report
    };
    mk_events_listen(&events);
    log_info("media server %s", "stared!");

    log_info("enter any key to exit");
    getchar();

    mk_stop_all_server();
    return 0;
}