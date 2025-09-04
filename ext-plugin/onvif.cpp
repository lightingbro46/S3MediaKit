#include "wsdd.nsmap" //Namespaces
#include "plugin/wsseapi.h" //WS-Sercurity
#include "Util/logger.h" 
#include "Extension/Plugin.h" 
#include "onvif.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

OnvifController::OnvifController(std::string strDeviceIp, std::string strUsername, std::string strPassword)
    : _strDeviceIp(strDeviceIp), _strUsername(strUsername), _strPassword(strPassword) {}

OnvifController::~OnvifController() {
    destroyControl();
}    

bool OnvifController::initControl() {
    if (_m_soap != nullptr) {
        destroyControl();
    }

    _m_soap = soap_new();
    _m_soap->connect_timeout = _m_soap->recv_timeout = _m_soap->send_timeout = 10; // timeout connection for 10 seconds
    soap_register_plugin(_m_soap, soap_wsse);

    _proxyDevice = new DeviceBindingProxy(_m_soap);

    if (!getDeviceInformation()) {
        return false;
    }

    if (!getNetworkInterfaces()) {
        return false;
    }

    if (!getDeviceCapabilities()) {
        return false;
    }

    if (!getMediaProfiles()) {
        return false;
    }
    return true;
}

bool OnvifController::destroyControl() {
    // free all deserialized and managed data, we can still reuse the context and proxies after this
    if (_m_soap != nullptr) {
        soap_destroy(_m_soap);
        soap_end(_m_soap);
        // free the shared context, proxy classes must terminate as well after this
        soap_free(_m_soap);
    }
    delete _proxyDevice;
    delete _proxyMedia;
    delete _proxyImaging;
    delete _proxyPTZ;

    _m_soap = nullptr;
    _proxyDevice = nullptr;
    _proxyMedia = nullptr;
    _proxyImaging = nullptr;
    _proxyPTZ = nullptr;

    return true;
}

bool OnvifController::getDeviceInformation() {
    // get device info and print
    thread_local string strDeviceUrl;
    strDeviceUrl = "http://" + _strDeviceIp + "/onvif/device_service";
    _proxyDevice->soap_endpoint = strDeviceUrl.c_str();
    DebugL << "Onvif device url: " << _proxyDevice->soap_endpoint;
    _tds__GetDeviceInformation *GetDeviceInformation = soap_new__tds__GetDeviceInformation(_m_soap);
    _tds__GetDeviceInformationResponse GetDeviceInformationResponse;
    if (!setCredentials()) {
        destroyControl();
        return false;
    }

    if (_proxyDevice->GetDeviceInformation(GetDeviceInformation, GetDeviceInformationResponse)) {
        reportError();
        destroyControl();
        return false;
    }

    _deviceInfo.manufacturer = GetDeviceInformationResponse.Manufacturer;
    _deviceInfo.model = GetDeviceInformationResponse.Model;
    _deviceInfo.firmwareVersion = GetDeviceInformationResponse.FirmwareVersion;
    _deviceInfo.serialNumber = GetDeviceInformationResponse.SerialNumber;
    _deviceInfo.hardwareId = GetDeviceInformationResponse.HardwareId;
    DebugL << "Manufacturer:     " << _deviceInfo.manufacturer;
    DebugL << "Model:            " << _deviceInfo.model;
    DebugL << "FirmwareVersion:  " << _deviceInfo.firmwareVersion;
    DebugL << "SerialNumber:     " << _deviceInfo.serialNumber;
    DebugL << "HardwareId:       " << _deviceInfo.hardwareId;
    return true;
}

bool OnvifController::getDeviceCapabilities() {
    // get device capabilities and print media
    _tds__GetCapabilities *GetCapabilities = soap_new__tds__GetCapabilities(_m_soap);
    _tds__GetCapabilitiesResponse GetCapabilitiesResponse;
    if (!setCredentials()) {
        destroyControl();
        return false;
    }

    if (_proxyDevice->GetCapabilities(GetCapabilities, GetCapabilitiesResponse)) {
        reportError();
    }

    if (!GetCapabilitiesResponse.Capabilities || !GetCapabilitiesResponse.Capabilities->Media ||
        !GetCapabilitiesResponse.Capabilities->Imaging) {
        reportError();
        destroyControl();
        return false;
    }

    if (GetCapabilitiesResponse.Capabilities->Media != nullptr) {
        thread_local string strUrl;
        strUrl = GetCapabilitiesResponse.Capabilities->Media->XAddr;
        int indexFooter = strUrl.find("/onvif");
        // Check if contains onvif then replace cameraip to header
        if (indexFooter > 0) {
            strUrl.erase(0, indexFooter);
            strUrl.insert(0, "http://" + _strDeviceIp);
        }
        DebugL << "Media XAddr:  " << strUrl;
        _proxyMedia = new MediaBindingProxy(_m_soap);
        _proxyMedia->soap_endpoint = strUrl.c_str();
    }

    if (GetCapabilitiesResponse.Capabilities->Imaging != nullptr) {
        thread_local string strUrl;
        strUrl = GetCapabilitiesResponse.Capabilities->Imaging->XAddr;
        int indexFooter = strUrl.find("/onvif");
        // Check if contains onvif then replace cameraip to header
        if (indexFooter > 0) {
            strUrl.erase(0, indexFooter);
            strUrl.insert(0, "http://" + _strDeviceIp);
        }
        DebugL << "Imaging XAddr:  " << strUrl << endl;
        _proxyImaging = new ImagingBindingProxy(_m_soap);
        _proxyImaging->soap_endpoint = strUrl.c_str();
    }

    if (GetCapabilitiesResponse.Capabilities->PTZ != nullptr) {
        thread_local string strUrl;
        strUrl = GetCapabilitiesResponse.Capabilities->PTZ->XAddr;
        int indexFooter = strUrl.find("/onvif");
        // Check if contains onvif then replace cameraip to header
        if (indexFooter > 0) {
            strUrl.erase(0, indexFooter);
            strUrl.insert(0, "http://" + _strDeviceIp);
        }
        DebugL << "PTZ XAddr:  " << strUrl << endl;

        _proxyPTZ = new PTZBindingProxy(_m_soap);
        _proxyPTZ->soap_endpoint = strUrl.c_str();

        _trt__GetProfiles *GetProfiles = soap_new__trt__GetProfiles(_m_soap);
        _trt__GetProfilesResponse GetProfilesResponse;
        if (!setCredentials()) {
            destroyControl();
            return false;
        }

        if (_proxyMedia->GetProfiles(GetProfiles, GetProfilesResponse)) {
            reportError();
            destroyControl();
            return false;
        }
        if (GetProfilesResponse.Profiles[0]->PTZConfiguration) {
            DebugL << "MediaProfile token for PTZ:" << GetProfilesResponse.Profiles[0]->token;
            _ptzProfile.strMediaProfileToken = GetProfilesResponse.Profiles[0]->token;

            _tptz__GetConfigurationOptions *GetConfigurationOptions = soap_new__tptz__GetConfigurationOptions(_m_soap);
            _tptz__GetConfigurationOptionsResponse GetConfigurationOptionsResponse;
            GetConfigurationOptions->ConfigurationToken = GetProfilesResponse.Profiles[0]->PTZConfiguration->token;
            if (!setCredentials()) {
                destroyControl();
                return false;
            }

            if (_proxyPTZ->GetConfigurationOptions(GetConfigurationOptions, GetConfigurationOptionsResponse)) {
                reportError();
                destroyControl();
                return false;
            }

            if (GetConfigurationOptionsResponse.PTZConfigurationOptions->Spaces->AbsolutePanTiltPositionSpace.size() > 0) {
                _ptzProfile.isAbsMoveEnable = true;
                _ptzProfile.absMinPan = GetConfigurationOptionsResponse.PTZConfigurationOptions->Spaces->AbsolutePanTiltPositionSpace[0]->XRange->Min;
                _ptzProfile.absMaxPan = GetConfigurationOptionsResponse.PTZConfigurationOptions->Spaces->AbsolutePanTiltPositionSpace[0]->XRange->Max;
                _ptzProfile.absMinTilt = GetConfigurationOptionsResponse.PTZConfigurationOptions->Spaces->AbsolutePanTiltPositionSpace[0]->YRange->Min;
                _ptzProfile.absMaxTilt = GetConfigurationOptionsResponse.PTZConfigurationOptions->Spaces->AbsolutePanTiltPositionSpace[0]->YRange->Max;
                _ptzProfile.absMinZoom = GetConfigurationOptionsResponse.PTZConfigurationOptions->Spaces->AbsoluteZoomPositionSpace[0]->XRange->Min;
                _ptzProfile.absMaxZoom = GetConfigurationOptionsResponse.PTZConfigurationOptions->Spaces->AbsoluteZoomPositionSpace[0]->XRange->Max;
                DebugL << "AbsoluteMove supported: Pan[" << _ptzProfile.absMinPan << "~" << _ptzProfile.absMaxPan << "] Tilt[" << _ptzProfile.absMinTilt << "~" << _ptzProfile.absMaxTilt
                    << "] Zoom[" << _ptzProfile.absMinZoom << "~" << _ptzProfile.absMaxZoom << "]";
            }

            if (GetConfigurationOptionsResponse.PTZConfigurationOptions->Spaces->ContinuousPanTiltVelocitySpace.size() > 0) {
                _ptzProfile.isConsMoveEnable = true;
                _ptzProfile.consMinPan = GetConfigurationOptionsResponse.PTZConfigurationOptions->Spaces->ContinuousPanTiltVelocitySpace[0]->XRange->Min;
                _ptzProfile.consMaxPan = GetConfigurationOptionsResponse.PTZConfigurationOptions->Spaces->ContinuousPanTiltVelocitySpace[0]->XRange->Max;
                _ptzProfile.consMinTilt = GetConfigurationOptionsResponse.PTZConfigurationOptions->Spaces->ContinuousPanTiltVelocitySpace[0]->YRange->Min;
                _ptzProfile.consMaxTilt = GetConfigurationOptionsResponse.PTZConfigurationOptions->Spaces->ContinuousPanTiltVelocitySpace[0]->YRange->Max;
                _ptzProfile.consMinZoom = GetConfigurationOptionsResponse.PTZConfigurationOptions->Spaces->ContinuousZoomVelocitySpace[0]->XRange->Min;
                _ptzProfile.consMaxZoom = GetConfigurationOptionsResponse.PTZConfigurationOptions->Spaces->ContinuousZoomVelocitySpace[0]->XRange->Max;
                DebugL << "ContinuousMove supported: Pan[" << _ptzProfile.consMinPan << "~" << _ptzProfile.consMaxPan << "] Tilt[" << _ptzProfile.consMinTilt << "~" << _ptzProfile.consMaxTilt
                    << "] Zoom[" << _ptzProfile.consMinZoom << "~" << _ptzProfile.consMaxZoom << "]";
            }

            if (GetConfigurationOptionsResponse.PTZConfigurationOptions->Spaces->RelativePanTiltTranslationSpace.size() > 0) {
                _ptzProfile.isRelMoveEnable = true;
                _ptzProfile.relMinPan = GetConfigurationOptionsResponse.PTZConfigurationOptions->Spaces->RelativePanTiltTranslationSpace[0]->XRange->Min;
                _ptzProfile.relMaxPan = GetConfigurationOptionsResponse.PTZConfigurationOptions->Spaces->RelativePanTiltTranslationSpace[0]->XRange->Max;
                _ptzProfile.relMinTilt = GetConfigurationOptionsResponse.PTZConfigurationOptions->Spaces->RelativePanTiltTranslationSpace[0]->YRange->Min;
                _ptzProfile.relMaxTilt = GetConfigurationOptionsResponse.PTZConfigurationOptions->Spaces->RelativePanTiltTranslationSpace[0]->YRange->Max;
                _ptzProfile.relMinZoom = GetConfigurationOptionsResponse.PTZConfigurationOptions->Spaces->RelativeZoomTranslationSpace[0]->XRange->Min;
                _ptzProfile.relMaxZoom = GetConfigurationOptionsResponse.PTZConfigurationOptions->Spaces->RelativeZoomTranslationSpace[0]->XRange->Max;
                DebugL << "RelativeMove supported: Pan[" << _ptzProfile.relMinPan << "~" << _ptzProfile.relMaxPan << "] Tilt[" << _ptzProfile.relMinTilt << "~" << _ptzProfile.relMaxTilt
                    << "] Zoom[" << _ptzProfile.relMinZoom << "~" << _ptzProfile.relMaxZoom << "]";
            }
        }
    }
    return true;
}

bool OnvifController::getMediaProfiles() {
    if (_proxyMedia == nullptr) {
        WarnL << "Unknown proxyMedia";
        return false;
    }

    _trt__GetProfiles *GetProfiles = soap_new__trt__GetProfiles(_m_soap);
    _trt__GetProfilesResponse GetProfilesResponse;
    if (!setCredentials()) {
        destroyControl();
        return false;
    }

    if (_proxyMedia->GetProfiles(GetProfiles, GetProfilesResponse)) {
        reportError();
        destroyControl();
        return false;
    }

    for (const auto &profile : GetProfilesResponse.Profiles) {
        if (!profile || profile->token.empty()) {
            continue;
        }
        DebugL << "Read a profile with MediaProfile token: " << profile->token;

        OnvifMediaProfile _profile;
        _profile.token = profile->token;
        if (profile->VideoSourceConfiguration && profile->VideoEncoderConfiguration) {
            // profile has video source
            _profile.hasVideo = true;

            // get video configuration in profile
            DebugL << profile->VideoEncoderConfiguration->Encoding;
            DebugL << profile->VideoEncoderConfiguration->Resolution->Width;
            DebugL << profile->VideoEncoderConfiguration->Resolution->Height;
            DebugL << profile->VideoEncoderConfiguration->RateControl->BitrateLimit;
            DebugL << profile->VideoEncoderConfiguration->RateControl->FrameRateLimit;
            DebugL << profile->VideoEncoderConfiguration->RateControl->EncodingInterval;
            DebugL << profile->VideoEncoderConfiguration->Quality;
            //todo: using ext-codec function
            switch (profile->VideoEncoderConfiguration->Encoding) {
                case tt__VideoEncoding__JPEG: _profile.vcodec = "JPEG"; break;
                case tt__VideoEncoding__MPEG4: _profile.vcodec = "MPEG4"; break;
                case tt__VideoEncoding__H264: _profile.vcodec = "H264"; break;
                default: _profile.vcodec = "UNKNOWN"; break;
            }

            if (_profile.vcodec == "UNKNOWN") {
                continue;
            }
            _profile.bitrate = profile->VideoEncoderConfiguration->RateControl->BitrateLimit;
            int framerate_limit = profile->VideoEncoderConfiguration->RateControl->FrameRateLimit;
            int encoding_interval = profile->VideoEncoderConfiguration->RateControl->EncodingInterval;
            _profile.fps = framerate_limit / encoding_interval;
            _profile.width = profile->VideoEncoderConfiguration->Resolution->Width;
            _profile.height = profile->VideoEncoderConfiguration->Resolution->Height;
            _profile.quality = profile->VideoEncoderConfiguration->Quality;

            // get video configuration option in profile
            // _trt__GetVideoEncoderConfigurationOptions* GetVideoConfigOptions = soap_new__trt__GetVideoEncoderConfigurationOptions(_m_soap);
            // _trt__GetVideoEncoderConfigurationOptionsResponse GetVideoConfigOptionResponse;
            // GetVideoConfigOptions->ProfileToken = &profile->token;
            // GetVideoConfigOptions->ConfigurationToken =  &profile->VideoEncoderConfiguration->token;
            // if (!setCredentials()) {
            //     destroyControl();
            //     return false;
            // }

            // if (_proxyMedia->GetVideoEncoderConfigurationOptions(GetVideoConfigOptions, GetVideoConfigOptionResponse)) {
            //     reportError();
            //     destroyControl();
            //     return false;
            // }

            // if (_profile.vcodec == "JPEG") {
            //     // DebugL << GetVideoConfigOptionResponse.Options->H264
            // }
        }

        if (_profile.hasVideo) {
            // profile has video stream, get stream uri
            _trt__GetStreamUri *GetStreamUri = soap_new__trt__GetStreamUri(_m_soap);
            _trt__GetStreamUriResponse GetStreamUriResponse;
            GetStreamUri->ProfileToken = profile->token;
            GetStreamUri->StreamSetup = soap_new_tt__StreamSetup(_m_soap, -1);
            GetStreamUri->StreamSetup->Stream = tt__StreamType__RTP_Unicast;
            GetStreamUri->StreamSetup->Transport = soap_new_tt__Transport(_m_soap, -1);
            GetStreamUri->StreamSetup->Transport->Protocol = tt__TransportProtocol__RTSP;
            if (!setCredentials()) {
                destroyControl();
                return false;
            }

            if (_proxyMedia->GetStreamUri(GetStreamUri, GetStreamUriResponse)) {
                reportError();
                destroyControl();
                return false;
            }
            DebugL << GetStreamUriResponse.MediaUri->Uri;
            _profile.url = GetStreamUriResponse.MediaUri->Uri;
            if (_profile.url.empty()) {
                continue;
            }
        }
        
        // add media profile to manager
        _mediaProfile.push_back(_profile);
    }
    return true;    
}

bool OnvifController::getNetworkInterfaces() {
    if (_proxyDevice == nullptr) {
        WarnL << "Unknown proxyDevice";
        return false;
    }
    // get network interface and print mac address
    _tds__GetNetworkInterfaces *GetNetworkInterfaces = soap_new__tds__GetNetworkInterfaces(_m_soap);
    _tds__GetNetworkInterfacesResponse GetNetworkInterfacesResponse;
    if (!setCredentials()) {
        destroyControl();
        return false;
    }
    if (_proxyDevice->GetNetworkInterfaces(GetNetworkInterfaces, GetNetworkInterfacesResponse)) {
        reportError();
        destroyControl();
        return false;
    }

    for (const auto network : GetNetworkInterfacesResponse.NetworkInterfaces) {
        if (network->Enabled && !network->Info->HwAddress.empty()) {
            _deviceInfo.macAddress = network->Info->HwAddress;
            break;
        }
    }
    return true;
}

void OnvifController::reportError() {
    std::ostringstream oss;
    soap_stream_fault(_m_soap, oss);
    _soapErrMsg = oss.str();
    WarnL << "Oops, something went wrong: " << oss.str();
}

bool OnvifController::setCredentials() {
    soap_wsse_delete_Security(_m_soap);
    // Access with username, password and lifetime
    if (soap_wsse_add_Timestamp(_m_soap, "Time", 10) ||
        soap_wsse_add_UsernameTokenDigest(_m_soap, "Auth", _strUsername.c_str(), _strPassword.c_str())) {
        reportError();
        return false;
    }
    return true;
}

bool OnvifController::PTZ_AbsoluteMove(float pan, float tilt, float zoom) {
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return false;
    }

    _tptz__AbsoluteMove *AbsoluteMove = soap_new__tptz__AbsoluteMove(_m_soap);
    _tptz__AbsoluteMoveResponse AbsoluteMoveResponse;

    AbsoluteMove->ProfileToken = _ptzProfile.strMediaProfileToken;
    if (AbsoluteMove->Position == nullptr)
        AbsoluteMove->Position = soap_new_tt__PTZVector(_m_soap);
    if (AbsoluteMove->Position->PanTilt == nullptr)
        AbsoluteMove->Position->PanTilt = soap_new_tt__Vector2D(_m_soap);
    if (AbsoluteMove->Position->Zoom == nullptr)
        AbsoluteMove->Position->Zoom = soap_new_tt__Vector1D(_m_soap);
    AbsoluteMove->Position->PanTilt->x = pan;
    AbsoluteMove->Position->PanTilt->y = tilt;
    AbsoluteMove->Position->Zoom->x = zoom;

    if (!setCredentials()) {
        return false;
    }

    if (_proxyPTZ->AbsoluteMove(AbsoluteMove, AbsoluteMoveResponse)) {
        reportError();
        return false;
    }
    return true;
}

bool OnvifController::PTZ_AbsoluteMove(float pan, float tilt, float zoom, float panSpeed, float tiltSpeed, float zoomSpeed) {
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return false;
    }

    cout << panSpeed << tiltSpeed << zoomSpeed << endl;

    _tptz__AbsoluteMove *AbsoluteMove = soap_new__tptz__AbsoluteMove(_m_soap);
    _tptz__AbsoluteMoveResponse AbsoluteMoveResponse;

    AbsoluteMove->ProfileToken = _ptzProfile.strMediaProfileToken;
    if (AbsoluteMove->Position == nullptr)
        AbsoluteMove->Position = soap_new_tt__PTZVector(_m_soap);
    if (AbsoluteMove->Position->PanTilt == nullptr)
        AbsoluteMove->Position->PanTilt = soap_new_tt__Vector2D(_m_soap);
    if (AbsoluteMove->Position->Zoom == nullptr)
        AbsoluteMove->Position->Zoom = soap_new_tt__Vector1D(_m_soap);
    AbsoluteMove->Position->PanTilt->x = pan;
    AbsoluteMove->Position->PanTilt->y = tilt;
    AbsoluteMove->Position->Zoom->x = zoom;

    if (AbsoluteMove->Speed == nullptr)
        AbsoluteMove->Speed = soap_new_tt__PTZSpeed(_m_soap);
    if (AbsoluteMove->Speed->PanTilt == nullptr)
        AbsoluteMove->Speed->PanTilt = soap_new_tt__Vector2D(_m_soap);
    if (AbsoluteMove->Speed->Zoom == nullptr)
        AbsoluteMove->Speed->Zoom = soap_new_tt__Vector1D(_m_soap);
    AbsoluteMove->Speed->PanTilt->x = panSpeed;
    AbsoluteMove->Speed->PanTilt->y = tiltSpeed;
    AbsoluteMove->Speed->Zoom->x = zoomSpeed;

    if (!setCredentials()) {
        return false;
    }

    if (_proxyPTZ->AbsoluteMove(AbsoluteMove, AbsoluteMoveResponse)) {
        reportError();
        return false;
    }
    return true;
}

tt__MoveStatus OnvifController::PTZ_GetStatus(float &pan, float &tilt, float &zoom) {
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return tt__MoveStatus__UNKNOWN;
    }

    _tptz__GetStatus *GetStatus = soap_new__tptz__GetStatus(_m_soap);
    _tptz__GetStatusResponse GetStatusResponse;
    GetStatus->ProfileToken = _ptzProfile.strMediaProfileToken;
    if (!setCredentials())
        return tt__MoveStatus__UNKNOWN;
    if (_proxyPTZ->GetStatus(GetStatus, GetStatusResponse)) {
        reportError();
        return tt__MoveStatus__UNKNOWN;
    }
    DebugL << "Pan: " << GetStatusResponse.PTZStatus->Position->PanTilt->x;
    DebugL << "Tilt:" << GetStatusResponse.PTZStatus->Position->PanTilt->y;
    DebugL << "Zoom:" << GetStatusResponse.PTZStatus->Position->Zoom->x;

    pan = GetStatusResponse.PTZStatus->Position->PanTilt->x;
    tilt = GetStatusResponse.PTZStatus->Position->PanTilt->y;
    zoom = GetStatusResponse.PTZStatus->Position->Zoom->x;

    if (GetStatusResponse.PTZStatus->MoveStatus->PanTilt && GetStatusResponse.PTZStatus->MoveStatus->Zoom) {
        if (*GetStatusResponse.PTZStatus->MoveStatus->PanTilt == tt__MoveStatus__UNKNOWN ||
            *GetStatusResponse.PTZStatus->MoveStatus->Zoom == tt__MoveStatus__UNKNOWN) {
            return tt__MoveStatus__UNKNOWN;
        }
        if (*GetStatusResponse.PTZStatus->MoveStatus->PanTilt == tt__MoveStatus__MOVING ||
            *GetStatusResponse.PTZStatus->MoveStatus->Zoom == tt__MoveStatus__MOVING) {
            return tt__MoveStatus__MOVING;
        }
        if (*GetStatusResponse.PTZStatus->MoveStatus->PanTilt == tt__MoveStatus__IDLE &&
            *GetStatusResponse.PTZStatus->MoveStatus->Zoom == tt__MoveStatus__IDLE) {
            return tt__MoveStatus__IDLE;
        }
    }
    return tt__MoveStatus__UNKNOWN;
}

bool OnvifController::PTZ_ContinuousMove(float pan, float tilt, float zoom, int timeout) {
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return false;
    }

    _tptz__ContinuousMove *ContinuosMove = soap_new__tptz__ContinuousMove(_m_soap);
    _tptz__ContinuousMoveResponse ContinuosMoveResponse;

    ContinuosMove->ProfileToken = _ptzProfile.strMediaProfileToken;
    if (ContinuosMove->Velocity == nullptr)
        ContinuosMove->Velocity = soap_new_tt__PTZSpeed(_m_soap);
    if (ContinuosMove->Velocity->PanTilt == nullptr)
        ContinuosMove->Velocity->PanTilt = soap_new_tt__Vector2D(_m_soap);
    if (ContinuosMove->Velocity->Zoom == nullptr)
        ContinuosMove->Velocity->Zoom = soap_new_tt__Vector1D(_m_soap);
    ContinuosMove->Velocity->PanTilt->x = pan;
    ContinuosMove->Velocity->PanTilt->y = tilt;
    ContinuosMove->Velocity->Zoom->x = zoom;
    if (timeout > 0) {
        ostringstream ss;
        ss << "PT" << timeout << "S";
        ContinuosMove->Timeout = soap_new_std__string(_m_soap, -1);
        *ContinuosMove->Timeout = ss.str();
    }

    if (!setCredentials()) {
        return false;
    }

    if (_proxyPTZ->ContinuousMove(ContinuosMove, ContinuosMoveResponse)) {
        reportError();
        return false;
    }
    return true;
}

bool OnvifController::PTZ_Stop(bool panTilt, bool zoom) {
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return false;
    }

    _tptz__Stop *Stop = soap_new__tptz__Stop(_m_soap);
    _tptz__StopResponse StopResponse;

    Stop->ProfileToken = _ptzProfile.strMediaProfileToken;
    if (Stop->PanTilt == nullptr) {
        Stop->PanTilt = soap_new_bool(_m_soap, -1);
        *Stop->PanTilt = panTilt;
    }

    if (Stop->Zoom == nullptr) {
        Stop->Zoom = soap_new_bool(_m_soap, -1);
        *Stop->Zoom = zoom;
    }

    if (!setCredentials()) {
        return false;
    }

    if (_proxyPTZ->Stop(Stop, StopResponse)) {
        reportError();
        return false;
    }
    return true;
}

bool OnvifController::PTZ_RelativeMove(float pan, float tilt, float zoom) {
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return false;
    }

    _tptz__RelativeMove *RelativeMove = soap_new__tptz__RelativeMove(_m_soap);
    _tptz__RelativeMoveResponse RelativeMoveResponse;

    RelativeMove->ProfileToken = _ptzProfile.strMediaProfileToken;
    if (RelativeMove->Translation == nullptr)
        RelativeMove->Translation = soap_new_tt__PTZVector(_m_soap);
    if (RelativeMove->Translation->PanTilt == nullptr)
        RelativeMove->Translation->PanTilt = soap_new_tt__Vector2D(_m_soap);
    if (RelativeMove->Translation->Zoom == nullptr)
        RelativeMove->Translation->Zoom = soap_new_tt__Vector1D(_m_soap);
    RelativeMove->Translation->PanTilt->x = pan;
    RelativeMove->Translation->PanTilt->y = tilt;
    RelativeMove->Translation->Zoom->x = zoom;

    if (!setCredentials()) {
        return false;
    }

    if (_proxyPTZ->RelativeMove(RelativeMove, RelativeMoveResponse)) {
        reportError();
        return false;
    }
    return true;
}

bool OnvifController::PTZ_RelativeMove(float pan, float tilt, float zoom, float panSpeed, float tiltSpeed, float zoomSpeed) {
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return false;
    }

    DebugL << pan << " " << tilt << " " << zoom;
    DebugL << panSpeed << " " << tiltSpeed << " " << zoomSpeed;

    _tptz__RelativeMove *RelativeMove = soap_new__tptz__RelativeMove(_m_soap);
    _tptz__RelativeMoveResponse RelativeMoveResponse;

    RelativeMove->ProfileToken = _ptzProfile.strMediaProfileToken;
    if (RelativeMove->Translation == nullptr)
        RelativeMove->Translation = soap_new_tt__PTZVector(_m_soap);
    if (RelativeMove->Translation->PanTilt == nullptr)
        RelativeMove->Translation->PanTilt = soap_new_tt__Vector2D(_m_soap);
    if (RelativeMove->Translation->Zoom == nullptr)
        RelativeMove->Translation->Zoom = soap_new_tt__Vector1D(_m_soap);
    RelativeMove->Translation->PanTilt->x = pan;
    RelativeMove->Translation->PanTilt->y = tilt;
    RelativeMove->Translation->Zoom->x = zoom;

    if (RelativeMove->Speed == nullptr)
        RelativeMove->Speed = soap_new_tt__PTZSpeed(_m_soap);
    if (RelativeMove->Speed->PanTilt == nullptr)
        RelativeMove->Speed->PanTilt = soap_new_tt__Vector2D(_m_soap);
    if (RelativeMove->Speed->Zoom == nullptr)
        RelativeMove->Speed->Zoom = soap_new_tt__Vector1D(_m_soap);
    RelativeMove->Speed->PanTilt->x = panSpeed;
    RelativeMove->Speed->PanTilt->y = tiltSpeed;
    RelativeMove->Speed->Zoom->x = zoomSpeed;

    if (!setCredentials()) {
        return false;
    }

    if (_proxyPTZ->RelativeMove(RelativeMove, RelativeMoveResponse)) {
        reportError();
        return false;
    }
    return true;
}

vector<OnvifMediaProfile> OnvifController::selectStreamUrls(bool include_secondary) {
    vector<OnvifMediaProfile> ret;
    bool found_primary = false;
    // find primary stream that eligible for
    for (const auto &it : _mediaProfile) {
        if (eligibleForPrimaryStream(it.vcodec, it.width, it.height, it.bitrate, it.fps)) {
            ret.push_back(it);
            found_primary = true;
            break;
        }
    }

    // if can not find any profile eligible for primary stream or require finding profile eligible for secordary stream
    if (!found_primary || include_secondary) {
        for (const auto &it : _mediaProfile) {
            if (eligibleForSecondaryStream(it.vcodec, it.width, it.height, it.bitrate, it.fps)) {
                ret.push_back(it);
                break;
            }
        }
    }

    return ret;
}

} // namespace managerkit

// namespace {

// DriverId getDriver() {
//     return OnvifDriver;
// }

// Controller::Ptr getControllerByDriverId(std::string ip, int port, std::string username, std::string password) {
//     return std::make_shared<OnvifController>();
// }

// } // namespace

// DriverPlugin onvif_plugin = {
//     getDriver,
//     getControllerByDriverId
// }