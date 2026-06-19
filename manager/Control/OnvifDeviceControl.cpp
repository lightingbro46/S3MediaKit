#include "wsdd.nsmap" //Namespaces
#include "plugin/wsseapi.h" //WS-Sercurity
#include "Util/logger.h" 
#include "Extension/Plugin.h" 
#include "OnvifDeviceControl.h"

using namespace std;
using namespace toolkit;

namespace managerkit {

OnvifControl::OnvifControl(std::string strDeviceIp, std::string strUsername, std::string strPassword) 
    : DeviceControl(), _strDeviceIp(strDeviceIp), _strUsername(strUsername), _strPassword(strPassword) {}

OnvifControl::~OnvifControl() {
    disconnect();
}    

static std::string ResRangeToString(const std::vector<std::pair<int, int>>& v)
{
    std::ostringstream oss;

    bool first = true;
    for (const auto& p : v)
    {
        if (!first) oss << " ";
        oss << p.first << "x" << p.second;
        first = false;
    }

    return oss.str();
}

static std::string bitrateRangeToString(int minBitrate, int maxBitrate) {
    if (minBitrate <= 0 || maxBitrate < minBitrate)
        return "";

    int steps = (maxBitrate >= 8000) ? 8 : (maxBitrate / 1000);
    steps = std::max(steps, 2);

    double step = static_cast<double>(maxBitrate - minBitrate) / (steps - 1);

    std::string result;

    for (int i = 0; i < steps; ++i) {
        int value;

        if (i == 0) {
            value = minBitrate;
        } else if (i == steps - 1) {
            value = maxBitrate;
        } else {
            int raw = static_cast<int>(minBitrate + i * step);
            int rounded = (raw + 128) / 256 * 256;

            value = std::max(minBitrate, std::min(maxBitrate, rounded));
        }

        if (!result.empty()) {
            size_t pos = result.find_last_of(' ');
            int last = (pos == std::string::npos) ? std::stoi(result) : std::stoi(result.substr(pos + 1));
            if (last == value) continue;

            result += " ";
        }

        result += std::to_string(value);
    }
    return result;
}

static int parse_timezone_offset_seconds(const std::string& tz) {
    if (tz.empty()) return 0;

    int sign = 1;
    size_t pos = std::string::npos;

    // ========================
    // 1. Find the '+' or '-' sign (if any)
    // ========================
    size_t plus_pos  = tz.find('+');
    size_t minus_pos = tz.find('-');

    if (plus_pos != std::string::npos) {
        sign = -1;
        pos = plus_pos + 1;
    } else if (minus_pos != std::string::npos) {
        sign = +1;
        pos = minus_pos + 1;
    } else {
        // ========================
        // 2. If there is no '+' or '-' → find the first digi
        // ========================
        for (size_t i = 0; i < tz.size(); ++i) {
            if (std::isdigit(tz[i])) {
                pos = i;
                break;
            }
        }
        if (pos == std::string::npos) return 0;

        sign = -1;
    }

    // The remaining part should contain only digits and ':' character
    std::string num = tz.substr(pos);

    int hour = 0, min = 0, sec = 0;

    // ========================
    // 3. If ':' is present
    // ========================
    if (num.find(':') != std::string::npos) {
        size_t p1 = num.find(':');
        size_t p2 = num.find(':', p1 + 1);

        hour = std::atoi(num.substr(0, p1).c_str());

        if (p2 != std::string::npos) {
            // HH:MM:SS
            min = std::atoi(num.substr(p1 + 1, p2 - p1 - 1).c_str());
            sec = std::atoi(num.substr(p2 + 1).c_str());
        } else {
            // HH:MM
            min = std::atoi(num.substr(p1 + 1).c_str());
        }
    }
    // ========================
    // 4. If ':' is not present → formats: H, HH, HMM, HHMM, HMMSS, HHMMSS
    // ========================
    else {
        int len = num.length();

        if (len <= 2) {
            // HH
            hour = std::atoi(num.c_str());
        } 
        else if (len == 3) {
            // HMM (730 → 7:30)
            hour = std::atoi(num.substr(0, 1).c_str());
            min  = std::atoi(num.substr(1, 2).c_str());
        } 
        else if (len == 4) {
            // HHMM (0730 → 07:30)
            hour = std::atoi(num.substr(0, 2).c_str());
            min  = std::atoi(num.substr(2, 2).c_str());
        } 
        else if (len == 5) {
            // HMMSS (53000 → 5:30:00)
            hour = std::atoi(num.substr(0, 1).c_str());
            min  = std::atoi(num.substr(1, 2).c_str());
            sec  = std::atoi(num.substr(3, 2).c_str());
        } 
        else if (len == 6) {
            // HHMMSS (073000 → 07:30:00)
            hour = std::atoi(num.substr(0, 2).c_str());
            min  = std::atoi(num.substr(2, 2).c_str());
            sec  = std::atoi(num.substr(4, 2).c_str());
        } 
        else {
            return 0;
        }
    }

    return sign * (hour * 3600 + min * 60 + sec);
}

static int get_system_offset_seconds() {
    time_t now = time(nullptr);

    struct tm gmt{};
    struct tm local{};

    gmtime_r(&now, &gmt);
    localtime_r(&now, &local);

    return (int)difftime(mktime(&local), mktime(&gmt));
}

static time_t build_camera_utc(const std::string& camera_tz, const time_t& time_point) {
    int offset_camera = parse_timezone_offset_seconds(camera_tz);
    int offset_system = get_system_offset_seconds();

    return time_point + (offset_system - offset_camera);
}

bool OnvifControl::connect() {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (_m_soap != nullptr) {
        disconnect();
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

void OnvifControl::disconnect() {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    // Delete proxies BEFORE freeing the shared soap context.
    // Proxy destructors may call soap_done() on the context they were created
    // with; freeing the context first causes a use-after-free crash.
    delete _proxyDevice;  _proxyDevice = nullptr;
    delete _proxyMedia;   _proxyMedia = nullptr;
    delete _proxyMedia2;  _proxyMedia2 = nullptr;
    delete _proxyImaging; _proxyImaging = nullptr;
    delete _proxyPTZ;     _proxyPTZ = nullptr;
    if (_m_soap != nullptr) {
        soap_destroy(_m_soap);
        soap_end(_m_soap);
        soap_free(_m_soap);
        _m_soap = nullptr;
    }
}

bool OnvifControl::getDeviceInformation() {
    // get device info and print
    _strDeviceUrl = "http://" + _strDeviceIp + "/onvif/device_service";
    _proxyDevice->soap_endpoint = _strDeviceUrl.c_str();
    TraceL << "Onvif device url: " << _proxyDevice->soap_endpoint;
    _tds__GetDeviceInformation *GetDeviceInformation = soap_new__tds__GetDeviceInformation(_m_soap);
    _tds__GetDeviceInformationResponse GetDeviceInformationResponse;
    if (!setCredentials()) {
        disconnect();
        return false;
    }

    if (_proxyDevice->GetDeviceInformation(GetDeviceInformation, GetDeviceInformationResponse)) {
        reportError();
        disconnect();
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

bool OnvifControl::getDeviceCapabilities() {
    // get device capabilities and print media
    _tds__GetCapabilities *GetCapabilities = soap_new__tds__GetCapabilities(_m_soap);
    _tds__GetCapabilitiesResponse GetCapabilitiesResponse;
    if (!setCredentials()) {
        disconnect();
        return false;
    }

    if (_proxyDevice->GetCapabilities(GetCapabilities, GetCapabilitiesResponse)) {
        reportError();
    }

    if (!GetCapabilitiesResponse.Capabilities || !GetCapabilitiesResponse.Capabilities->Media) {
        reportError();
        disconnect();
        return false;
    }

    if (GetCapabilitiesResponse.Capabilities->Media != nullptr) {
        _strMediaUrl = GetCapabilitiesResponse.Capabilities->Media->XAddr;
        int indexFooter = _strMediaUrl.find("/onvif");
        // Check if contains onvif then replace cameraip to header
        if (indexFooter > 0) {
            _strMediaUrl.erase(0, indexFooter);
            _strMediaUrl.insert(0, "http://" + _strDeviceIp);
        }
        TraceL << "Media XAddr:  " << _strMediaUrl;
        _proxyMedia = new MediaBindingProxy(_m_soap);
        _proxyMedia->soap_endpoint = _strMediaUrl.c_str();
        _proxyMedia2 = new Media2BindingProxy(_m_soap);
        _proxyMedia2->soap_endpoint = _strMediaUrl.c_str();
    }

    if (GetCapabilitiesResponse.Capabilities->Imaging != nullptr) {
        _strImagingUrl = GetCapabilitiesResponse.Capabilities->Imaging->XAddr;
        int indexFooter = _strImagingUrl.find("/onvif");
        // Check if contains onvif then replace cameraip to header
        if (indexFooter > 0) {
            _strImagingUrl.erase(0, indexFooter);
            _strImagingUrl.insert(0, "http://" + _strDeviceIp);
        }
        TraceL << "Imaging XAddr:  " << _strImagingUrl << endl;
        _proxyImaging = new ImagingBindingProxy(_m_soap);
        _proxyImaging->soap_endpoint = _strImagingUrl.c_str();
    }

    if (GetCapabilitiesResponse.Capabilities->PTZ != nullptr) {
        _strPTZUrl = GetCapabilitiesResponse.Capabilities->PTZ->XAddr;
        int indexFooter = _strPTZUrl.find("/onvif");
        // Check if contains onvif then replace cameraip to header
        if (indexFooter > 0) {
            _strPTZUrl.erase(0, indexFooter);
            _strPTZUrl.insert(0, "http://" + _strDeviceIp);
        }
        TraceL << "PTZ XAddr:  " << _strPTZUrl << endl;

        _proxyPTZ = new PTZBindingProxy(_m_soap);
        _proxyPTZ->soap_endpoint = _strPTZUrl.c_str();

        _trt__GetProfiles *GetProfiles = soap_new__trt__GetProfiles(_m_soap);
        _trt__GetProfilesResponse GetProfilesResponse;
        if (!setCredentials()) {
            disconnect();
            return false;
        }

        if (_proxyMedia->GetProfiles(GetProfiles, GetProfilesResponse)) {
            reportError();
            disconnect();
            return false;
        }
        if (GetProfilesResponse.Profiles[0]->PTZConfiguration) {
            DebugL << "MediaProfile token for PTZ:" << GetProfilesResponse.Profiles[0]->token;
            _ptzProfile.strMediaProfileToken = GetProfilesResponse.Profiles[0]->token;

            _tptz__GetConfigurationOptions *GetConfigurationOptions = soap_new__tptz__GetConfigurationOptions(_m_soap);
            _tptz__GetConfigurationOptionsResponse GetConfigurationOptionsResponse;
            GetConfigurationOptions->ConfigurationToken = GetProfilesResponse.Profiles[0]->PTZConfiguration->token;
            if (!setCredentials()) {
                disconnect();
                return false;
            }

            if (_proxyPTZ->GetConfigurationOptions(GetConfigurationOptions, GetConfigurationOptionsResponse)) {
                reportError();
                disconnect();
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

            if (_ptzProfile.isAbsMoveEnable) {
                getPTZPresets();
            } else {
                _ptzProfile.isPresetEnable = false;
                _ptzProfile.isHomePresetEnable = false;
                _ptzProfile.homePresetToken = "";
                _ptzProfile.presetMap.clear();
                DebugL << "Preset is not supported since AbsoluteMove is not supported";
            }
        }
    }
    return true;
}

bool OnvifControl::getMediaProfiles() {
    if (_proxyMedia == nullptr || _proxyMedia2 == nullptr) {
        WarnL << "Unknown proxyMedia";
        return false;
    }

    _trt__GetProfiles *GetProfiles = soap_new__trt__GetProfiles(_m_soap);
    _trt__GetProfilesResponse GetProfilesResponse;
    if (!setCredentials()) {
        disconnect();
        return false;
    }

    if (_proxyMedia->GetProfiles(GetProfiles, GetProfilesResponse)) {
        reportError();
        disconnect();
        return false;
    }

    // note: reset media profile list every time when get media profiles since profile token may change after device reboot
    _mediaProfile.clear();

    for (const auto &profile : GetProfilesResponse.Profiles) {
        if (!profile || profile->token.empty()) {
            continue;
        }
        TraceL << "========================================";
        TraceL << "Read a profile with MediaProfile token: " << profile->token;

        OnvifMediaProfile _profile;
        _profile.token = profile->token;
        if (profile->VideoSourceConfiguration && profile->VideoEncoderConfiguration) {
            // profile has video source
            _profile.hasVideo = true;

            ns1__GetConfiguration *GetVideoConfig  = soap_new_ns1__GetConfiguration(_m_soap);
            GetVideoConfig->ProfileToken = &profile->token;
            GetVideoConfig->ConfigurationToken =  &profile->VideoEncoderConfiguration->token;
            _ns1__GetVideoEncoderConfigurationsResponse GetVideoConfigResponse;
            if (!setCredentials()) {
                disconnect();
                return false;
            }
            if (!_proxyMedia2->GetVideoEncoderConfigurations(GetVideoConfig, GetVideoConfigResponse)) {                
                TraceL << "Get video encoder configuration for token " << profile->token << ", configurations size = " << GetVideoConfigResponse.Configurations.size();
                auto tokenConfig = GetVideoConfigResponse.Configurations[0];
                if (!tokenConfig) {
                    continue;
                }
                _profile.vcodec = tokenConfig->Encoding;
                TraceL << "Video Codec: " << _profile.vcodec;
                _profile.bitrate = tokenConfig->RateControl ? tokenConfig->RateControl->BitrateLimit : 0;
                TraceL << "Bitrate Limit: " << _profile.bitrate;
                _profile.fps = tokenConfig->RateControl ? tokenConfig->RateControl->FrameRateLimit : 0;
                TraceL << "Bitrate Limit: " << _profile.fps;
                _profile.width = tokenConfig->Resolution ? tokenConfig->Resolution->Width : 0;
                TraceL << "Width: " << _profile.width;
                _profile.height = tokenConfig->Resolution ? tokenConfig->Resolution->Height : 0;
                TraceL << "Height: " << _profile.height;
                _profile.quality = tokenConfig->Quality;
                TraceL << "Quality: " << _profile.quality;
    
                // get video configuration option in profile
                ns1__GetConfiguration *GetVideoConfigOptions  = soap_new_ns1__GetConfiguration(_m_soap);
                _ns1__GetVideoEncoderConfigurationOptionsResponse GetVideoConfigOptionResponse;
                GetVideoConfigOptions->ProfileToken = &profile->token;
                GetVideoConfigOptions->ConfigurationToken =  &profile->VideoEncoderConfiguration->token;
                if (!setCredentials()) {
                    reportError();
                    return false;
                }
                if (_proxyMedia2->GetVideoEncoderConfigurationOptions(GetVideoConfigOptions, GetVideoConfigOptionResponse)) {
                    reportError();
                    return false;
                }
                for (const auto &Option : GetVideoConfigOptionResponse.Options) {
                    if (!Option) {
                        continue;
                    }
                    VideoEncoderConfigOption cfg_option;
                    for (size_t i = 0; i < Option->ResolutionsAvailable.size(); ++i)
                    {
                        tt__VideoResolution2* r = Option->ResolutionsAvailable[i];
                        cfg_option.ResolutionsAvailable.push_back(std::make_pair(r->Width, r->Height));
                    }
                    cfg_option.BitRateRange = bitrateRangeToString(Option->BitrateRange->Min, Option->BitrateRange->Max);
                    cfg_option.QualityRange = std::make_pair(Option->QualityRange->Min, Option->QualityRange->Max);
                    if (Option->FrameRatesSupported) {
                        cfg_option.FrameRatesSupported = *Option->FrameRatesSupported;
                    }
                    cfg_option.FPSEditable = _profile.fps != 0 && !cfg_option.FrameRatesSupported.empty();
                    cfg_option.bitrateEditable = _profile.bitrate != 0 && Option->BitrateRange->Min != Option->BitrateRange->Max;
                    cfg_option.resolutionEditable = cfg_option.ResolutionsAvailable.size() > 1;
                    _profile.vEncoderOptionMap[Option->Encoding] = cfg_option;
                }
    
                _profile.videoEncEditable = _profile.vEncoderOptionMap.size() > 1;
                bool editable = false;
                for (const auto &it : _profile.vEncoderOptionMap) {
                    editable = it.second.bitrateEditable || it.second.FPSEditable || it.second.resolutionEditable;
                }
                _profile.videoConfigEditable = editable || _profile.videoEncEditable;
            } else {
                reportError();
            }
        }

        // if (profile->AudioSourceConfiguration && profile->AudioEncoderConfiguration) {
        //     // profile has audio source
        //     _profile.hasAudio = true;
            
        //     // get audio configuration in profile
        //     switch (profile->AudioEncoderConfiguration->Encoding) {
        //         case tt__AudioEncoding__G711: _profile.acodec = "G711"; break;
        //         case tt__AudioEncoding__G726: _profile.acodec = "G726"; break;
        //         case tt__AudioEncoding__AAC: _profile.acodec = "AAC"; break;
        //         default: _profile.acodec = "UNKNOWN"; break;
        //     }

        //     if (_profile.acodec == "UNKNOWN") {
        //         WarnL << "Unsupported audio codec: " << profile->AudioEncoderConfiguration->Encoding << ". Ignore";
        //         continue;
        //     }

        //     TraceL << "Audio Codec: " << profile->AudioEncoderConfiguration->Encoding << ">> " << _profile.acodec;
        //     _profile.sampleBit = profile->AudioEncoderConfiguration->Bitrate ? profile->AudioEncoderConfiguration->Bitrate : 0;
        //     TraceL << "Sample Bit Limit: " << _profile.sampleBit;
        //     int sample = profile->VideoEncoderConfiguration->RateControl ? profile->VideoEncoderConfiguration->RateControl->FrameRateLimit : 0;
        //     TraceL << "Sample Rate Limit: " << _profile.sampleRate;
        //     _trt__GetAudioSources *GetAudioSources = soap_new__trt__GetAudioSources(_m_soap);    
        //     _trt__GetAudioSourcesResponse GetAudioSourcesResponse;
        //     if (!setCredentials()) {
        //         disconnect();
        //         return false;
        //     }

        //     if (_proxyMedia->GetAudioSources(GetAudioSources, GetAudioSourcesResponse)) {
        //         reportError();
        //         disconnect();
        //         return false;
        //     }
        //     auto sourceToken = profile->AudioSourceConfiguration->SourceToken;
        //     for (const auto &audioSource : GetAudioSourcesResponse.AudioSources) {
        //         WarnL << "Found audio source token: " << sourceToken << ", " << audioSource->token;
        //         if (audioSource->token == sourceToken) {
        //             _profile.channelNo = audioSource->Channels ? audioSource->Channels : 0;
        //             break;
        //         }
        //     }
    
        //     TraceL << "Channel No: " << _profile.channelNo;
        //     // todo: get audio configuration option in profile
        //     // _trt__GetAudioEncoderConfigurationOptions* GetAudioConfigOptions = soap_new__trt__GetAudioEncoderConfigurationOptions(_m_soap);
        //     // _trt__GetAudioEncoderConfigurationOptionsResponse GetAudioConfigOptionResponse;
        //     // GetAudioConfigOptions->ProfileToken = &profile->token;
        //     // GetAudioConfigOptions->ConfigurationToken =  &profile->AudioEncoderConfiguration->token;
        //     // if (!setCredentials()) {
        //     //     disconnect();
        //     //     return false;
        //     // }

        //     // if (_proxyMedia->GetAudioEncoderConfigurationOptions(GetAudioConfigOptions, GetAudioConfigOptionResponse)) {
        //     //     reportError();
        //     //     disconnect();
        //     //     return false;
        //     // }
        // }

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
                disconnect();
                return false;
            }

            if (_proxyMedia->GetStreamUri(GetStreamUri, GetStreamUriResponse)) {
                reportError();
                disconnect();
                return false;
            }
            TraceL << "Uri: " << GetStreamUriResponse.MediaUri->Uri;
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

tt__VideoEncoder2Configuration* OnvifControl::getConfigVideoEncoder2ByToken(const std::string& profileToken)
{
    if (_proxyMedia == nullptr || _proxyMedia2 == nullptr) {
        WarnL << "Unknown proxyMedia";
        return nullptr;
    }

    _trt__GetProfiles *GetProfiles = soap_new__trt__GetProfiles(_m_soap);
    _trt__GetProfilesResponse GetProfilesResponse;
    if (!setCredentials()) {
        disconnect();
        return nullptr;
    }

    if (_proxyMedia->GetProfiles(GetProfiles, GetProfilesResponse)) {
        reportError();
        disconnect();
        return nullptr;
    }

    for (auto profile : GetProfilesResponse.Profiles)
    {
        if (profile->token == profileToken)
        {
            ns1__GetConfiguration *GetVideoConfig  = soap_new_ns1__GetConfiguration(_m_soap);
            GetVideoConfig->ProfileToken = &profile->token;
            GetVideoConfig->ConfigurationToken =  &profile->VideoEncoderConfiguration->token;
            _ns1__GetVideoEncoderConfigurationsResponse GetVideoConfigResponse;
            if (!setCredentials()) {
                disconnect();
                return nullptr;
            }
            if (_proxyMedia2->GetVideoEncoderConfigurations(GetVideoConfig, GetVideoConfigResponse)) {
                reportError();
                disconnect();
                return nullptr;
            }
            return GetVideoConfigResponse.Configurations[0];
        }
    }

    return nullptr;
}

bool OnvifControl::setVideoEncoderConfigByToken(const std::string& token, const VideoEncoderConfig& vConfigNew)
{
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    auto config = getConfigVideoEncoder2ByToken(token);
    if (!config) {
        return false;
    }

    tt__VideoEncoder2Configuration* configToSet = soap_new_tt__VideoEncoder2Configuration(_m_soap);
    *configToSet = *config;

    configToSet->Encoding = vConfigNew.vcodec;

    if (!configToSet->Resolution)
        configToSet->Resolution = soap_new_tt__VideoResolution2(_m_soap);
    configToSet->Resolution->Width = vConfigNew.width;
    configToSet->Resolution->Height = vConfigNew.height;

    if (!configToSet->RateControl)
        configToSet->RateControl = soap_new_tt__VideoRateControl2(_m_soap);

    configToSet->RateControl->FrameRateLimit = vConfigNew.fps;
    configToSet->RateControl->BitrateLimit = vConfigNew.bitrate;

    _ns1__SetVideoEncoderConfiguration* setReq = soap_new__ns1__SetVideoEncoderConfiguration(_m_soap);
    ns1__SetConfigurationResponse setResp;

    setReq->Configuration = configToSet;

    if (!setCredentials()) {
        disconnect();
        return false;
    }

    if (_proxyMedia2->SetVideoEncoderConfiguration(setReq, setResp))
    {
        reportError();
        return false;
    }

    _ns1__GetStreamUri *GetStreamUri = soap_new__ns1__GetStreamUri(_m_soap);
    _ns1__GetStreamUriResponse GetStreamUriResponse;
    GetStreamUri->ProfileToken = token;
    GetStreamUri->Protocol = "RTSP";
    if (!setCredentials()) {
        disconnect();
        return false;
    }

    if (_proxyMedia2->GetStreamUri(GetStreamUri, GetStreamUriResponse)) {
        reportError();
        disconnect();
        return false;
    }

    InfoL << "The new config has been sent to the camera, current URI: " << GetStreamUriResponse.Uri;

    return true;
}

bool OnvifControl::getVideoEncoderConfigByToken(const std::string& token, VideoEncoderConfig& vConfig) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    auto config = getConfigVideoEncoder2ByToken(token);
    if (!config) {
        return false;
    }

    vConfig.vcodec = config->Encoding;
    vConfig.bitrate = config->RateControl ? config->RateControl->BitrateLimit : 0;
    vConfig.fps = config->RateControl ? config->RateControl->FrameRateLimit : 0;
    vConfig.width = config->Resolution ? config->Resolution->Width : 0;
    vConfig.height = config->Resolution ? config->Resolution->Height : 0;
    vConfig.quality = config->Quality;

    return true;
}

bool OnvifControl::getNetworkInterfaces() {
    if (_proxyDevice == nullptr) {
        WarnL << "Unknown proxyDevice";
        return false;
    }
    // get network interface and print mac address
    _tds__GetNetworkInterfaces *GetNetworkInterfaces = soap_new__tds__GetNetworkInterfaces(_m_soap);
    _tds__GetNetworkInterfacesResponse GetNetworkInterfacesResponse;
    if (!setCredentials()) {
        disconnect();
        return false;
    }
    if (_proxyDevice->GetNetworkInterfaces(GetNetworkInterfaces, GetNetworkInterfacesResponse)) {
        reportError();
        disconnect();
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

void OnvifControl::reportError() {
    std::ostringstream oss;
    soap_stream_fault(_m_soap, oss);
    _soapErrMsg = oss.str();
    WarnL << "Oops, something went wrong: " << oss.str();
}

bool OnvifControl::getCameraTime() {
    if (!_proxyDevice || !_m_soap) {
        return false;
    }
    _tds__GetSystemDateAndTime* req = soap_new__tds__GetSystemDateAndTime(_m_soap, -1);
    _tds__GetSystemDateAndTimeResponse resp;

    if (_proxyDevice->GetSystemDateAndTime(req, resp)) {
        reportError();
        return false;
    }

    if (!resp.SystemDateAndTime) {
        WarnL << "Invalid camera time response";
        return false;
    }

    if (resp.SystemDateAndTime->TimeZone) {
        _camTimeInfo.TZ = resp.SystemDateAndTime->TimeZone->TZ;
    }

    auto buildTime = [](tt__DateTime* dt, bool isUtc) -> time_t
    {
        if (!dt || !dt->Date || !dt->Time)
            return 0;

        struct tm t{};
        t.tm_year = dt->Date->Year - 1900;
        t.tm_mon  = dt->Date->Month - 1;
        t.tm_mday = dt->Date->Day;
        t.tm_hour = dt->Time->Hour;
        t.tm_min  = dt->Time->Minute;
        t.tm_sec  = dt->Time->Second;

        if (isUtc) {
            return timegm(&t);
        } else {
            return mktime(&t);
        }
    };

    if (resp.SystemDateAndTime->UTCDateTime) {
        time_t t = buildTime(resp.SystemDateAndTime->UTCDateTime, true);
        if (t > 0) {
            _camTimeInfo.cam_time = t;
            int offset_cam = parse_timezone_offset_seconds(_camTimeInfo.TZ);
            int offset_sys = get_system_offset_seconds();
            _camTimeInfo.cam_sys_time = _camTimeInfo.cam_time + offset_cam - offset_sys;
            return true;
        }
    }

    if (resp.SystemDateAndTime->LocalDateTime) {
        time_t t = buildTime(resp.SystemDateAndTime->LocalDateTime, false);
        if (t > 0) {
            _camTimeInfo.cam_time = t;
            _camTimeInfo.cam_sys_time = t;
            return true;
        }
    }

    return false;
}

struct SoapHookContext {
    int (*original_fsend)(struct soap*, const char*, size_t);
    time_t offset;
};

void OnvifControl::installSoapHook(struct soap* soap, time_t offset) {
    if (!soap || !soap->fsend)
        return;

    if (soap->user) {
        auto ctx = static_cast<SoapHookContext*>(soap->user);
        ctx->offset = offset;
        return;
    }

    auto ctx = (SoapHookContext*)soap_malloc(soap, sizeof(SoapHookContext));
    ctx->original_fsend = soap->fsend;
    ctx->offset = offset;

    soap->user = ctx;

    soap->fsend = [](struct soap* soap, const char* buf, size_t len) -> int {
        auto ctx = static_cast<SoapHookContext*>(soap->user);
        if (!ctx || !ctx->original_fsend)
            return SOAP_ERR;

        // create UTC string (format: Y-m-dTH:M:SZ)
        auto makeUtcTime = [](time_t t) -> std::string {
            struct tm gmt;
            gmtime_r(&t, &gmt);

            char buf[64];
            strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &gmt);
            return std::string(buf);
        };

        auto replaceTagValue = [](std::string& xml, const std::string& tag, const std::string& value) {
            std::string open  = "<" + tag + ">";
            std::string close = "</" + tag + ">";

            size_t pos = 0;
            while ((pos = xml.find(open, pos)) != std::string::npos) {
                size_t end = xml.find(close, pos);
                if (end == std::string::npos) break;

                xml.replace(pos, end - pos + close.length(), open + value + close);

                pos += value.length();
            }
        };

        std::string xml(buf, len);

        if (xml.find("wsse:Security") != std::string::npos) {

            time_t now = time(nullptr) + ctx->offset;

            std::string created = makeUtcTime(now);
            std::string expires = makeUtcTime(now + 10);

            replaceTagValue(xml, "wsu:Created", created);
            replaceTagValue(xml, "wsu:Expires", expires);
        }

        return ctx->original_fsend(soap, xml.c_str(), xml.size());
    };
}

void OnvifControl::setCameraTimeManual() {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (!_proxyDevice) return;

    soap *local_soap = soap_new();
    local_soap->connect_timeout = local_soap->recv_timeout = local_soap->send_timeout = 10; // timeout connection for 10 seconds
    soap_register_plugin(local_soap, soap_wsse);

    // RAII guard: ensures local_soap and proxy are freed on ALL exit paths,
    // including early returns when getCameraTime() fails or time is in sync.
    // Proxy is deleted before soap_free to avoid a use-after-free in the
    // proxy destructor.
    struct Guard {
        soap *s;
        DeviceBindingProxy *p;
        ~Guard() {
            delete p;
            if (s) { soap_destroy(s); soap_end(s); soap_free(s); }
        }
    } guard{local_soap, new DeviceBindingProxy(local_soap)};
    DeviceBindingProxy *proxy = guard.p;
    proxy->soap_endpoint = _proxyDevice->soap_endpoint;

    if (!getCameraTime() || _camTimeInfo.TZ.empty()) {
        return;
    }

    if (abs(_camTimeInfo.cam_sys_time - time(nullptr)) >= 10) {
        time_t now = time(nullptr);
        time_t camera_utc = build_camera_utc(_camTimeInfo.TZ, now);

        struct tm utc_tm;
        gmtime_r(&camera_utc, &utc_tm);

        _tds__SetSystemDateAndTime req;
        _tds__SetSystemDateAndTimeResponse resp;

        req.DateTimeType = tt__SetDateTimeType__Manual;
        req.DaylightSavings = false;

        req.UTCDateTime = soap_new_tt__DateTime(local_soap);
        req.UTCDateTime->Date = soap_new_tt__Date(local_soap);
        req.UTCDateTime->Time = soap_new_tt__Time(local_soap);

        req.UTCDateTime->Date->Year  = utc_tm.tm_year + 1900;
        req.UTCDateTime->Date->Month = utc_tm.tm_mon + 1;
        req.UTCDateTime->Date->Day   = utc_tm.tm_mday;

        req.UTCDateTime->Time->Hour   = utc_tm.tm_hour;
        req.UTCDateTime->Time->Minute = utc_tm.tm_min;
        req.UTCDateTime->Time->Second = utc_tm.tm_sec;

        time_t offset_time = _camTimeInfo.cam_time - time(nullptr);
        installSoapHook(local_soap, offset_time);

        if (soap_wsse_add_Timestamp(local_soap, "Time", 10) == SOAP_OK && soap_wsse_add_UsernameTokenDigest_at(local_soap, "Auth", _strUsername.c_str(), _strPassword.c_str(), _camTimeInfo.cam_time) == SOAP_OK) {
            int ret = proxy->SetSystemDateAndTime(&req, resp);
            if (ret != SOAP_OK) {
                WarnL << "Set camera time failed, ret " << ret;
            }
        }
    }
}

bool OnvifControl::setCredentials() {
    soap_wsse_delete_Security(_m_soap);
    // Access with username, password and lifetime
    bool need_update_time = false;
    if (getCameraTime()) {
        time_t offset_time = _camTimeInfo.cam_time - time(nullptr);
        if (abs(offset_time) >= 5) {
            installSoapHook(_m_soap, offset_time);
            need_update_time = true;
        }
    }

    const int ret = need_update_time ? (soap_wsse_add_UsernameTokenDigest_at(_m_soap, "Auth", _strUsername.c_str(), _strPassword.c_str(), _camTimeInfo.cam_time))
                                     : (soap_wsse_add_UsernameTokenDigest(_m_soap, "Auth", _strUsername.c_str(), _strPassword.c_str()));

    if (soap_wsse_add_Timestamp(_m_soap, "Time", 10) || ret) {
        reportError();
        return false;
    }
    return true;
}

bool OnvifControl::PTZ_AbsoluteMove(float pan, float tilt, float zoom) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return false;
    }
    DebugL << "Execute PTZ AbsoluteMove: Pan(" << pan << "), Tilt(" << tilt << "), Zoom(" << zoom << "). Use default speed of device";

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

bool OnvifControl::PTZ_AbsoluteMove(float pan, float tilt, float zoom, float panSpeed, float tiltSpeed, float zoomSpeed) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return false;
    }
    DebugL << "Execute PTZ AbsoluteMove: Pan(" << pan << "), Tilt(" << tilt << "), Zoom(" << zoom << "), "
            << "panSpeed(" << panSpeed << "), tiltSpeed(" << tiltSpeed << "), zoomSpeed(" << zoomSpeed << ")";

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

tt__MoveStatus OnvifControl::PTZ_GetStatus(float &pan, float &tilt, float &zoom) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
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

bool OnvifControl::PTZ_ContinuousMove(float pan, float tilt, float zoom, int timeout) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return false;
    }
    DebugL << "Execute PTZ ContinuousMove: Pan(" << pan << "), Tilt(" << tilt << "), Zoom(" << zoom << "). Use default speed of device";

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

bool OnvifControl::PTZ_Stop(bool panTilt, bool zoom) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return false;
    }
    DebugL << "Execute PTZ ContinuousMove Stop: PanTilt(" << panTilt << "), Zoom(" << zoom << ")";

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

bool OnvifControl::PTZ_RelativeMove(float pan, float tilt, float zoom) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return false;
    }
    DebugL << "Execute PTZ RelativeMove: Pan(" << pan << "), Tilt(" << tilt << "), Zoom(" << zoom << "). Use default speed of device";

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

bool OnvifControl::PTZ_RelativeMove(float pan, float tilt, float zoom, float panSpeed, float tiltSpeed, float zoomSpeed) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return false;
    }
    DebugL << "Execute PTZ RelativeMove: Pan(" << pan << "), Tilt(" << tilt << "), Zoom(" << zoom << "), "
            << "panSpeed(" << panSpeed << "), tiltSpeed(" << tiltSpeed << "), zoomSpeed(" << zoomSpeed << ")";

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

vector<OnvifMediaProfile> OnvifControl::selectStreamUrls(bool include_secondary) {
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

bool OnvifControl::PTZ_GotoPreset(const string &presetToken, float panSpeed, float tiltSpeed, float zoomSpeed) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return false;
    }
    DebugL << "Execute PTZ GotoPreset: presetToken(" << presetToken << "), panSpeed(" << panSpeed << "), tiltSpeed(" << tiltSpeed << "), zoomSpeed(" << zoomSpeed << ")";

    _tptz__GotoPreset *GotoPreset = soap_new__tptz__GotoPreset(_m_soap);
    _tptz__GotoPresetResponse GotoPresetResponse;

    GotoPreset->ProfileToken = _ptzProfile.strMediaProfileToken;
    GotoPreset->PresetToken = presetToken;
    if (GotoPreset->Speed == nullptr)
        GotoPreset->Speed = soap_new_tt__PTZSpeed(_m_soap);
    if (GotoPreset->Speed->PanTilt == nullptr)
        GotoPreset->Speed->PanTilt = soap_new_tt__Vector2D(_m_soap);
    if (GotoPreset->Speed->Zoom == nullptr)
        GotoPreset->Speed->Zoom = soap_new_tt__Vector1D(_m_soap);
    GotoPreset->Speed->PanTilt->x = panSpeed;
    GotoPreset->Speed->PanTilt->y = tiltSpeed;
    GotoPreset->Speed->Zoom->x = zoomSpeed;

    if (!setCredentials()) {
        return false;
    }

    if (_proxyPTZ->GotoPreset(GotoPreset, GotoPresetResponse)) {
        reportError();
        return false;
    }

    return true;
}

bool OnvifControl::PTZ_SetPreset(const string &presetName, const string &presetToken, float &pan, float &tilt, float &zoom) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return false;
    }

    pan = 0.0f, tilt = 0.0f, zoom = 0.0f;
    tt__MoveStatus moveStatus = PTZ_GetStatus(pan, tilt, zoom);
    if (moveStatus == tt__MoveStatus__MOVING) {
        WarnL << "PTZ is moving, can not set preset";
        return false;
    }

    DebugL << "Execute PTZ SetPreset: presetName(" << presetName << "), presetToken(" << presetToken << "), "
           << "current pan(" << pan << "), current tilt(" << tilt << "), current zoom(" << zoom << ")";
    
    _tptz__SetPreset *SetPreset = soap_new__tptz__SetPreset(_m_soap);
    _tptz__SetPresetResponse SetPresetResponse;

    SetPreset->ProfileToken = _ptzProfile.strMediaProfileToken;
    SetPreset->PresetToken = soap_new_std__string(_m_soap);
    SetPreset->PresetName  = soap_new_std__string(_m_soap);
    *SetPreset->PresetToken = presetToken;
    *SetPreset->PresetName = presetName;

    if (!setCredentials()) {
        return false;
    }

    if (_proxyPTZ->SetPreset(SetPreset, SetPresetResponse)) {
        reportError();
        return false;
    }
    // todo: update preset list after set preset since some devices may change preset token after reboot
    // todo: update home preset if current preset is home preset
    // todo: store user defined preset token and name in local storage
    return true;
}

bool  OnvifControl::PTZ_GotoHomePosition(float panSpeed, float tiltSpeed, float zoomSpeed) {
    std::lock_guard<std::recursive_mutex> lk(_soap_mtx);
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return false;
    }

    if (_ptzProfile.isPresetEnable && _ptzProfile.isHomePresetEnable) {
        DebugL << "Goto Home preset: " << _ptzProfile.homePresetToken;
        return PTZ_GotoPreset(_ptzProfile.homePresetToken, panSpeed, tiltSpeed, zoomSpeed);
    }

    WarnL << "Home preset is not supported. Try execute GotoHomePosition command directly";
    _tptz__GotoHomePosition *GotoHomePosition = soap_new__tptz__GotoHomePosition(_m_soap);
    _tptz__GotoHomePositionResponse GotoHomePositionResponse;
    GotoHomePosition->ProfileToken = _ptzProfile.strMediaProfileToken;
    if (GotoHomePosition->Speed == nullptr)
        GotoHomePosition->Speed = soap_new_tt__PTZSpeed(_m_soap);
    if (GotoHomePosition->Speed->PanTilt == nullptr)
        GotoHomePosition->Speed->PanTilt = soap_new_tt__Vector2D(_m_soap);
    if (GotoHomePosition->Speed->Zoom == nullptr)
        GotoHomePosition->Speed->Zoom = soap_new_tt__Vector1D(_m_soap);
    GotoHomePosition->Speed->PanTilt->x = panSpeed;
    GotoHomePosition->Speed->PanTilt->y = tiltSpeed;
    GotoHomePosition->Speed->Zoom->x = zoomSpeed;

    if (!setCredentials()) {
        return false;
    }

    if (_proxyPTZ->GotoHomePosition(GotoHomePosition, GotoHomePositionResponse)) {
        reportError();
        return false;
    }

    return true;
}

bool OnvifControl::getPTZPresets() {
    if (_proxyPTZ == nullptr) {
        WarnL << "Unknown proxyPTZ";
        return false;
    }

    _tptz__GetPresets *GetPresets = soap_new__tptz__GetPresets(_m_soap);
    _tptz__GetPresetsResponse GetPresetsResponse;
    GetPresets->ProfileToken = _ptzProfile.strMediaProfileToken;
    if (!setCredentials()) {
        return false;
    }
    if (_proxyPTZ->GetPresets(GetPresets, GetPresetsResponse)) {
        reportError();
        return false;
    }

    _ptzProfile.isPresetEnable = true;
    _ptzProfile.presetMap.clear();

    bool isHomePresetEnable = false;
    string homePresetToken;

    // note: reset preset list every time when get capabilities since preset token may change after device reboot
    if (!GetPresetsResponse.Preset.empty()) {

        for (const auto &preset : GetPresetsResponse.Preset) {
            if (!preset || !preset->token || preset->token->empty() || !preset->Name || preset->Name->empty() || !preset->PTZPosition) {
                continue;
            }
            string pToken = *preset->token;
            string pName = *preset->Name;
            auto pAbsPan = preset->PTZPosition->PanTilt ? preset->PTZPosition->PanTilt->x : 0.0f;
            auto pAbsTilt = preset->PTZPosition->PanTilt ? preset->PTZPosition->PanTilt->y : 0.0f;
            auto pAbsZoom = preset->PTZPosition->Zoom ? preset->PTZPosition->Zoom->x : 0.0f;

            DebugL << "Preset token: " << pToken << " name: " << pName << " pan: " << pAbsPan << " tilt: " << pAbsTilt << " zoom: " << pAbsZoom;
            OnvifPTZProfile::PTZPreset p;
            p.Token = pToken;
            p.Name = pName;
            p.absPan = pAbsPan;
            p.absTilt = pAbsTilt;
            p.absZoom = pAbsZoom;
            _ptzProfile.presetMap.emplace(pToken, std::move(p));

            if (pToken == "home" || pToken == "Home" || pToken == "1") {
                isHomePresetEnable = true;
                homePresetToken = pToken;
            }
        }
    } else {
        // float pan = 0.0f, tilt = 0.0f, zoom = 0.0f;
        // string homePresetToken = "1";
        // string homePresetName = "home";
        // if (PTZ_SetPreset(homePresetName, homePresetToken, pan, tilt, zoom)) {
        //     _ptzProfile.isPresetEnable = true;
        //     _ptzProfile.isHomePresetEnable = true;
        //     OnvifPTZProfile::PTZPreset homePreset;
        //     homePreset.Token = homePresetToken;
        //     homePreset.Name = homePresetName;
        //     homePreset.absPan = pan;
        //     homePreset.absTilt = tilt;
        //     homePreset.absZoom = zoom;
        //     _ptzProfile.homePresetToken = homePresetToken;
        //     _ptzProfile.presetMap.emplace(homePresetToken, std::move(homePreset));
        //     DebugL << "Home preset is supported by setting preset with token: " << homePresetToken << " and name: " << homePresetName;
        // }
    }

    _ptzProfile.isHomePresetEnable = isHomePresetEnable;
    _ptzProfile.homePresetToken = homePresetToken;
    DebugL << (isHomePresetEnable ? "Home preset is supported" : "Home preset can be not found");
    return true;
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