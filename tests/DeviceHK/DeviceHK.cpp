#ifdef ENABLE_HKDEVICE
#include "DeviceHK.h"
#include "Util/TimeTicker.h"
#include "Util/MD5.h"
namespace mediakit {

#define HK_APP_NAME  "live"

DeviceHK::DeviceHK() {
    InfoL << endl;
    static onceToken token( []() {
        NET_DVR_Init();
        NET_DVR_SetDVRMessageCallBack_V31([](LONG lCommand,NET_DVR_ALARMER *pAlarmer,char *pAlarmInfo,DWORD dwBufLen,void* pUser){
            WarnL<<pAlarmInfo;
            return TRUE;
        },NULL);
    }, []() {
        NET_DVR_Cleanup();
    });
}

DeviceHK::~DeviceHK() {
    InfoL << endl;
}

void DeviceHK::connectDevice(const connectInfo &info, const connectCB& cb, int iTimeOut) {
    NET_DVR_USER_LOGIN_INFO loginInfo;
    NET_DVR_DEVICEINFO_V40 loginResult;

    //login info
    strcpy(loginInfo.sDeviceAddress, info.strDevIp.c_str());
    loginInfo.wPort = info.ui16DevPort;
    strcpy(loginInfo.sUserName, info.strUserName.c_str());
    strcpy(loginInfo.sPassword, info.strPwd.c_str());

    //callback info
    typedef function< void(LONG lUserID, DWORD dwResult, LPNET_DVR_DEVICEINFO_V30 lpDeviceInfo)> hkLoginCB;
    loginInfo.bUseAsynLogin = TRUE;
    weak_ptr<Device> weakSelf = shared_from_this();
    loginInfo.pUser = new hkLoginCB([weakSelf,cb](LONG lUserID, DWORD dwResult, LPNET_DVR_DEVICEINFO_V30 lpDeviceInfo ) {
                        //TraceL<<lUserID<<" "<<dwResult<<" "<<lpDeviceInfo->sSerialNumber;
                        connectResult result;
                        if(dwResult==TRUE) {
                            result.strDevName=(char *)(lpDeviceInfo->sSerialNumber);
                            result.ui16ChnStart=lpDeviceInfo->byStartChan;
                            result.ui16ChnCount=lpDeviceInfo->byChanNum;
                            auto _strongSelf=weakSelf.lock();
                            if(_strongSelf) {
                                auto strongSelf=dynamic_pointer_cast<DeviceHK>(_strongSelf);
                                strongSelf->onConnected(lUserID,lpDeviceInfo);
                            }
                        } else {
                            WarnL<<"connect deviceHK failed:"<<NET_DVR_GetLastError();
                        }
                        cb(dwResult==TRUE,result);
                    });
    loginInfo.cbLoginResult = [](LONG lUserID, DWORD dwResult, LPNET_DVR_DEVICEINFO_V30 lpDeviceInfo , void* pUser) {
                auto *fun=static_cast<hkLoginCB *>(pUser);
                (*fun)(lUserID,dwResult,lpDeviceInfo);
                delete fun;
            };
    NET_DVR_SetConnectTime(iTimeOut * 1000, 3);
    NET_DVR_Login_V40(&loginInfo, &loginResult);
}

void DeviceHK::disconnect(const relustCB& cb) {
    m_mapChannels.clear();
    if (m_i64LoginId >= 0) {
        NET_DVR_Logout(m_i64LoginId);
        m_i64LoginId = -1;
        Device::onDisconnected(true);
    }

}

void DeviceHK::addChannel(int iChn, bool bMainStream) {
    DevChannel::Ptr channel( new DevChannelHK(m_i64LoginId, (char *) m_deviceInfo.sSerialNumber, iChn, bMainStream));
    m_mapChannels[iChn] = channel;
}

void DeviceHK::delChannel(int chn) {
    m_mapChannels.erase(chn);
}

void DeviceHK::onConnected(LONG lUserID, LPNET_DVR_DEVICEINFO_V30 lpDeviceInfo) {
    m_i64LoginId = lUserID;
    m_deviceInfo = *lpDeviceInfo;
    Device::onConnected();
}

void DeviceHK::addAllChannel(bool bMainStream) {
    InfoL << endl;
    for (int i = 0; i < m_deviceInfo.byChanNum; i++) {
        addChannel(m_deviceInfo.byStartChan + i, bMainStream);
    }
}

DevChannelHK::DevChannelHK(int64_t i64LoginId, const char* pcDevName, int iChn, bool bMainStream) :
        DevChannel(HK_APP_NAME,(StrPrinter<<MD5(pcDevName).hexdigest()<<"_"<<iChn<<endl).data()),
        m_i64LoginId(i64LoginId) {
    InfoL << endl;
    NET_DVR_PREVIEWINFO previewInfo;
    previewInfo.lChannel = iChn; //Channel number
    previewInfo.dwStreamType = bMainStream ? 0 : 1; // Code stream type, 0-main code stream, 1-subcode stream, 2-code stream 3, 3-code stream 4, etc. and so on
    previewInfo.dwLinkMode = 1; //0: TCP mode, 1: UDP mode, 2: Multicast mode, 3 -RTP mode, 4-RTP/RTSP, 5-RSTP/HTTP
    previewInfo.hPlayWnd = 0; //The handle of the play window, NULL means that the image is not played
    previewInfo.byProtoType = 0; //Application layer flow fetch protocol, 0-private protocol, 1-RTSP protocol
    previewInfo.dwDisplayBufNum = 1; //The maximum number of buffered frames in the playback buffer of the playback library is 1-50. When set to 0, the default is 1.
    previewInfo.bBlocked = 0;
    m_i64PreviewHandle = NET_DVR_RealPlay_V40(m_i64LoginId, &previewInfo,
                                        [](LONG lPlayHandle,DWORD dwDataType,BYTE *pBuffer,DWORD dwBufSize,void* pUser) {
                                            DevChannelHK *self=reinterpret_cast<DevChannelHK *>(pUser);
                                            if(self->m_i64PreviewHandle!=(int64_t)lPlayHandle) {
                                                return;
                                            }
                                            self->onPreview(dwDataType,pBuffer,dwBufSize);
                                        }, this);
    if (m_i64PreviewHandle == -1) {
        throw std::runtime_error( StrPrinter 	<< "Equipment[" << pcDevName << "/" << iChn << "] failed to start live preview:"
                                                << NET_DVR_GetLastError() << endl);
    }
}

DevChannelHK::~DevChannelHK() {
    InfoL << endl;
    if (m_i64PreviewHandle >= 0) {
        NET_DVR_StopRealPlay(m_i64PreviewHandle);
        m_i64PreviewHandle = -1;
    }
    if (m_iPlayHandle >= 0) {
        PlayM4_StopSoundShare(m_iPlayHandle);
        PlayM4_Stop(m_iPlayHandle);
        m_iPlayHandle = -1;
    }
}

void DevChannelHK::onPreview(DWORD dwDataType, BYTE* pBuffer, DWORD dwBufSize) {
    //TimeTicker1(-1);
    switch (dwDataType) {
    case NET_DVR_SYSHEAD: { //System header data
        if (!PlayM4_GetPort(&m_iPlayHandle)) {  //Get the channel number that is not used by the playback library
            WarnL << "PlayM4_GetPort:" << NET_DVR_GetLastError();
            break;
        }
        if (dwBufSize > 0) {
            if (!PlayM4_SetStreamOpenMode(m_iPlayHandle, STREAME_REALTIME)) { //Set live streaming mode
                WarnL << "PlayM4_SetStreamOpenMode:" << NET_DVR_GetLastError();
                break;
            }
            if (!PlayM4_OpenStream(m_iPlayHandle, pBuffer, dwBufSize,
                    1024 * 1024)) {  //Open the streaming interface
                WarnL << "PlayM4_OpenStream:" << NET_DVR_GetLastError();
                break;
            }

            PlayM4_SetDecCallBackMend(m_iPlayHandle,
                    [](int nPort,char * pBuf,int nSize,FRAME_INFO * pFrameInfo, void* nUser,int nReserved2) {
                        DevChannelHK *chn=reinterpret_cast<DevChannelHK *>(nUser);
                        if(chn->m_iPlayHandle!=nPort) {
                            return;
                        }
                        chn->onGetDecData(pBuf,nSize,pFrameInfo);
                    }, this);
            if (!PlayM4_Play(m_iPlayHandle, 0)) {  //Playback starts
                WarnL << "PlayM4_Play:" << NET_DVR_GetLastError();
                break;
            }
            InfoL << "Setting the decoder successfully!" << endl;
            // Open audio decoding, requires the bitstream to be a composite stream
            if (!PlayM4_PlaySoundShare(m_iPlayHandle)) {
                WarnL << "PlayM4_PlaySound:" << NET_DVR_GetLastError();
                break;
            }
        }
    }
        break;
    case NET_DVR_STREAMDATA: { //Streaming data (including video stream data that is separated by composite streams or audio and video)
        if (dwBufSize > 0 && m_iPlayHandle != -1) {
            if (!PlayM4_InputData(m_iPlayHandle, pBuffer, dwBufSize)) {
                WarnL << "PlayM4_InputData:" << NET_DVR_GetLastError();
                break;
            }
        }
    }
        break;
    case NET_DVR_AUDIOSTREAMDATA: { //Audio data
    }
        break;
    case NET_DVR_PRIVATE_DATA: { //Private data, including smart information
    }
        break;
    default:
        break;
    }
}

void DevChannelHK::onGetDecData(char* pBuf, int nSize, FRAME_INFO* pFrameInfo) {
    //InfoL << pFrameInfo->nType;
    switch (pFrameInfo->nType) {
    case T_YV12: {
        if (!m_bVideoSeted) {
            m_bVideoSeted = true;
            VideoInfo video;
            video.iWidth = pFrameInfo->nWidth;
            video.iHeight = pFrameInfo->nHeight;
            video.iFrameRate = pFrameInfo->nFrameRate;
            initVideo(video);
        }
        char *yuv[3];
        int yuv_len[3];
        yuv_len[0] = pFrameInfo->nWidth;
        yuv_len[1] = pFrameInfo->nWidth / 2;
        yuv_len[2] = pFrameInfo->nWidth / 2;
        int dwOffset_Y = pFrameInfo->nWidth * pFrameInfo->nHeight;
        yuv[0] = pBuf;
        yuv[2] = yuv[0] + dwOffset_Y;
        yuv[1] = yuv[2] + dwOffset_Y / 4;
        inputYUV(yuv, yuv_len, pFrameInfo->nStamp);
    }
        break;
    case T_AUDIO16: {
        if (!m_bAudioSeted) {
            m_bAudioSeted = true;
            AudioInfo audio;
            audio.iChannel = pFrameInfo->nWidth;
            audio.iSampleBit = pFrameInfo->nHeight;
            audio.iSampleRate = pFrameInfo->nFrameRate;
            initAudio(audio);
        }
        inputPCM(pBuf, nSize, pFrameInfo->nStamp);
    }
        break;
    default:
        break;
    }
}

} /* namespace mediakit */

#endif //ENABLE_HKDEVICE
