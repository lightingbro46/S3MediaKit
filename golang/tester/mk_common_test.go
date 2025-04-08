package tester

import (
	"github.com/stretchr/testify/require"
	"testing"
	"time"
	"s3mediakit/s3mediakit"
)

func TestCommonEnvInit(t *testing.T) {
	conf := s3mediakit.EnvInit(
		0, s3mediakit.LTrace, s3mediakit.LogConsole|s3mediakit.LogCallback|s3mediakit.LogFile,
		"log", 1, true, "../../conf/config.ini", false, "../../default.pem", "")

	require.Equal(t, 0, conf.ThreadNum())
	require.Equal(t, s3mediakit.LTrace, conf.LogLevel())
	require.Equal(t, s3mediakit.LogConsole|s3mediakit.LogCallback|s3mediakit.LogFile, conf.LogMask())
	require.Equal(t, "log", conf.LogFilePath())
	require.Equal(t, 1, conf.LogFileDays())
	require.Equal(t, true, conf.IniIsPath())
	require.Equal(t, "../../conf/config.ini", conf.Ini())
	require.Equal(t, false, conf.SslIsPath())
	require.Equal(t, "../../default.pem", conf.Ssl())
	require.Equal(t, "", conf.SslPwd())
}

func TestCommonSetLog(t *testing.T) {
	s3mediakit.SetLog(1, 1)
}

func TestCommonServer(t *testing.T) {
	p, err := s3mediakit.HttpServerStart(1180, false)
	require.Nil(t, err)
	require.Equal(t, uint16(1180), p)

	p, err = s3mediakit.RtspServerStart(11935, false)
	require.Nil(t, err)
	require.Equal(t, uint16(11935), p)

	p, err = s3mediakit.RtmpServerStart(11554, false)
	require.Nil(t, err)
	require.Equal(t, uint16(11554), p)

	p, err = s3mediakit.RtpServerStart(11111)
	require.Nil(t, err)
	require.Equal(t, uint16(11111), p)

	p, err = s3mediakit.RtcServerStart(11222) // 未启用webrtc功能
	require.NotNil(t, err)
	//require.Equal(t, uint16(11222), p)

	p, err = s3mediakit.SrtServerStart(11333)
	require.Nil(t, err)
	require.Equal(t, uint16(11333), p)

	p, err = s3mediakit.ShellServerStart(11444)
	require.Nil(t, err)
	require.Equal(t, uint16(11444), p)

	<-time.After(time.Minute * 5)
	s3mediakit.StopAllServer()
}
