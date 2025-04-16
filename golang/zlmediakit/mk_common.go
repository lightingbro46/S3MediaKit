package s3mediakit

//#include "mk_mediakit.h"
//#include "mk_common.h"
import "C"
import (
	"fmt"
	"s3mediakit/helper"
)

type LogMask int
type LogLevel int

const (
	LogConsole  LogMask = 1 << 0
	LogFile     LogMask = 1 << 1
	LogCallback LogMask = 2 << 1
)

const (
	LTrace LogLevel = 0
	LDebug LogLevel = 1
	LInfo  LogLevel = 2
	LWarn  LogLevel = 3
	LError LogLevel = 4
)

type Config struct {
	c *C.mk_config
}

func newConfigFromC(c *C.mk_config) *Config {
	if c == nil {
		return nil
	}
	return &Config{c: c}
}

func (conf *Config) ThreadNum() int {
	return int(conf.c.thread_num)
}

func (conf *Config) SetThreadNum(threadNum int) {
	conf.c.thread_num = C.int(threadNum)
}

func (conf *Config) LogLevel() LogLevel {
	return LogLevel(conf.c.log_level)
}

func (conf *Config) SetLogLevel(logLevel LogLevel) {
	conf.c.log_level = C.int(logLevel)
}

func (conf *Config) LogMask() LogMask {
	return LogMask(conf.c.log_mask)
}

func (conf *Config) SetLogMask(logMask LogMask) {
	conf.c.log_mask = C.int(logMask)
}

func (conf *Config) LogFilePath() string {
	return C.GoString(conf.c.log_file_path)
}

func (conf *Config) SetLogFilePath(logFilePath string) {
	logFilePathC := C.CString(logFilePath)
	conf.c.log_file_path = logFilePathC
}

func (conf *Config) LogFileDays() int {
	return int(conf.c.log_file_days)
}

func (conf *Config) SetLogFileDays(logFileDays int) {
	conf.c.log_file_days = C.int(logFileDays)
}

func (conf *Config) IniIsPath() bool {
	return int(conf.c.ini_is_path) == 1
}

func (conf *Config) SetIniIsPath(iniIsPath bool) {
	conf.c.ini_is_path = C.int(helper.Bool2Int(iniIsPath))
}

func (conf *Config) Ini() string {
	return C.GoString(conf.c.ini)
}

func (conf *Config) SetIni(ini string) {
	iniC := C.CString(ini)
	conf.c.ini = iniC
}

func (conf *Config) Ssl() string {
	return C.GoString(conf.c.ssl)
}

func (conf *Config) SetSsl(ssl string) {
	sslC := C.CString(ssl)
	conf.c.ssl = sslC
}

func (conf *Config) SslIsPath() bool {
	return int(conf.c.ssl_is_path) == 1
}

func (conf *Config) SetSslIsPath(sslIsPath bool) {
	conf.c.ssl_is_path = C.int(helper.Bool2Int(sslIsPath))
}

func (conf *Config) SslPwd() string {
	return C.GoString(conf.c.ssl_pwd)
}

func (conf *Config) SetSslPwd(sslPwd string) {
	sslPwdC := C.CString(sslPwd)
	conf.c.ssl_pwd = sslPwdC
}

// EnvInit initializes the environment, you need to call this function before calling the library
//
// threadNum: number of threads
// logLevel: log level, support 0~4
// logMask: The mask that controls the log output, please check the macros such as LOG_CONSOLE, LOG_FILE, LOG_CALLBACK
// logFilePath: The file log save path, the path can not exist (the folder can be created internally), set to NULL to close the log output to the file
// logFileDays: The number of days of saving the file log, set to 0 to close the log file
// iniIsPath: Is the configuration file content or path
// ini: The content or path of the configuration file can be empty. If the file does not exist, the default configuration will be exported to the file.
// sslIsPath: Is the ssl certificate content or path
// ssl: The content or path of the ssl certificate can be empty
// sslPwd: Certificate password, can be empty
func EnvInit(threadNum int, logLevel LogLevel, logMask LogMask, logFilePath string, logFileDays int, iniIsPath bool, ini string, sslIsPath bool, ssl string, sslPwd string) *Config {
	var c C.mk_config
	conf := newConfigFromC(&c)

	conf.SetThreadNum(threadNum)
	conf.SetLogLevel(logLevel)
	conf.SetLogMask(logMask)
	conf.SetLogFilePath(logFilePath)
	conf.SetLogFileDays(logFileDays)
	conf.SetIniIsPath(iniIsPath)
	conf.SetIni(ini)
	conf.SetSsl(ssl)
	conf.SetSslIsPath(sslIsPath)
	conf.SetSslPwd(sslPwd)

	C.mk_env_init(conf.c)
	return conf
}

// StopAllServer closes all servers, please call it when the main function exits.
func StopAllServer() {
	C.mk_stop_all_server()
}

// SetLog Set log file
//
// fileMaxSize Single slice file size (MB)
// fileMaxCount Number of slice files
func SetLog(fileMaxSize, fileMaxCount int) {
	C.mk_set_log(C.int(fileMaxSize), C.int(fileMaxCount))
}

// HttpServer Start Create https] server
//
// port http listening port, 80 is recommended, and 0 is passed in random allocation
// Is ssl a server of type ssl
func HttpServerStart(port uint16, ssl bool) (uint16, error) {
	ret := C.mk_http_server_start(C.ushort(port), C.int(helper.Bool2Int(ssl)))
	i := uint16(ret)
	if i == 0 {
		return 0, fmt.Errorf("http server start fail")
	}
	return i, nil
}

// RtspServerStart Create rtsp[s] server
//
// port rtsp listening port, recommended 554, pass 0, random allocation
// Is ssl a server of type ssl
func RtspServerStart(port uint16, ssl bool) (uint16, error) {
	ret := C.mk_rtsp_server_start(C.ushort(port), C.int(helper.Bool2Int(ssl)))
	i := uint16(ret)
	if i == 0 {
		return 0, fmt.Errorf("rtsp server start fail")
	}
	return i, nil
}

// RtmpServerStart Create rtmp[s] server
//
// port rtmp listen port, recommended 1935, if 0 is passed, random allocation will be given
// Is ssl a server of type ssl
func RtmpServerStart(port uint16, ssl bool) (uint16, error) {
	ret := C.mk_rtmp_server_start(C.ushort(port), C.int(helper.Bool2Int(ssl)))
	i := uint16(ret)
	if i == 0 {
		return 0, fmt.Errorf("rtmp server start fail")
	}
	return i, nil
}

// RtpServerStart Create rtp server
//
// port rtp listening port (including udp/tcp)
func RtpServerStart(port uint16) (uint16, error) {
	ret := C.mk_rtp_server_start(C.ushort(port))
	i := uint16(ret)
	if i == 0 {
		return 0, fmt.Errorf("rtp server start fail")
	}
	return i, nil
}

// RtcServerStart Create rtc server
//
// port rtc listening port
func RtcServerStart(port uint16) (uint16, error) {
	ret := C.mk_rtc_server_start(C.ushort(port))
	i := uint16(ret)
	if i == 0 {
		return 0, fmt.Errorf("rtc server start fail")
	}
	return i, nil
}

// todo mk_webrtc_get_answer_sdp
// todo mk_webrtc_get_answer_sdp2

// SrtServerStart Create srt server
//
// port srt listening port
func SrtServerStart(port uint16) (uint16, error) {
	ret := C.mk_srt_server_start(C.ushort(port))
	i := uint16(ret)
	if i == 0 {
		return 0, fmt.Errorf("srt server start fail")
	}
	return i, nil
}

// ShellServerStart Create a shell server
//
// port shell listen port
func ShellServerStart(port uint16) (uint16, error) {
	ret := C.mk_shell_server_start(C.ushort(port))
	i := uint16(ret)
	if i == 0 {
		return 0, fmt.Errorf("shell server start fail")
	}
	return i, nil
}
