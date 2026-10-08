#!/usr/bin/env python3
"""Mock thiết bị ONVIF cho stress test: phục vụ N camera ảo trên một cổng duy nhất.

Mỗi camera ảo có một địa chỉ loopback riêng (127.a.b.c). Toàn bộ 127.0.0.0/8 đều là
loopback trên Linux nên một listener trên 0.0.0.0 phục vụ được cả 300 thiết bị; danh
tính thiết bị lấy từ địa chỉ local của socket.

Phủ các operation mà OnvifControl::connect() và nhánh điều khiển PTZ thực sự gọi.
Các operation còn lại trả SOAP Fault ActionNotSupported — đúng như camera đời thấp,
và nhờ vậy vẫn kiểm được nhánh xử lý lỗi của server.

Dùng:
    onvif_mock.py [--port 18580] [--fault-spec PATH] [--state PATH] [--verbose]

--fault-spec là file JSON được đọc lại mỗi giây:
    {
      "default": {"latency_ms": 0},
      "devices": {
        "127.0.2.5":  {"latency_ms": 9000},            # ONVIF chậm -> kiểm pool device()
        "127.0.2.6":  {"offline": true},               # rớt kết nối
        "127.0.2.7":  {"fail_ops": ["ContinuousMove"]} # lỗi riêng lệnh PTZ
      }
    }

Cấu hình encoder được lưu theo từng thiết bị: SetVideoEncoderConfiguration (Media2) ghi
lại, GetProfiles và GetVideoEncoderConfigurations trả đúng giá trị đã set — đủ để kiểm
luồng video config rule (áp, xác nhận, rollback). --state ghi thêm cấu hình hiện tại
của từng thiết bị dưới khoá "encoders".
"""
import argparse
import base64
import calendar
import hashlib
import json
import os
import re
import socket
import sys
import threading
import time
from collections import defaultdict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

NS = (
    'xmlns:SOAP-ENV="http://www.w3.org/2003/05/soap-envelope" '
    'xmlns:tds="http://www.onvif.org/ver10/device/wsdl" '
    'xmlns:trt="http://www.onvif.org/ver10/media/wsdl" '
    'xmlns:tr2="http://www.onvif.org/ver20/media/wsdl" '
    'xmlns:tptz="http://www.onvif.org/ver20/ptz/wsdl" '
    'xmlns:timg="http://www.onvif.org/ver20/imaging/wsdl" '
    'xmlns:tse="http://www.onvif.org/ver10/search/wsdl" '
    'xmlns:trp="http://www.onvif.org/ver10/replay/wsdl" '
    'xmlns:wsnt="http://docs.oasis-open.org/wsn/b-2" '
    'xmlns:tt="http://www.onvif.org/ver10/schema"'
)

OP_RE = re.compile(rb"<(?:[A-Za-z0-9_.-]+:)?Body[^>]*>\s*<(?:([A-Za-z0-9_.-]+):)?([A-Za-z0-9_.-]+)")


def envelope(body):
    return ('<?xml version="1.0" encoding="UTF-8"?>'
            '<SOAP-ENV:Envelope %s><SOAP-ENV:Body>%s</SOAP-ENV:Body></SOAP-ENV:Envelope>'
            % (NS, body))


def fault(code, reason):
    return envelope(
        '<SOAP-ENV:Fault><SOAP-ENV:Code><SOAP-ENV:Value>SOAP-ENV:Receiver</SOAP-ENV:Value>'
        '<SOAP-ENV:Subcode><SOAP-ENV:Value>ter:%s</SOAP-ENV:Value></SOAP-ENV:Subcode>'
        '</SOAP-ENV:Code><SOAP-ENV:Reason><SOAP-ENV:Text xml:lang="en">%s</SOAP-ENV:Text>'
        '</SOAP-ENV:Reason></SOAP-ENV:Fault>' % (code, reason))


MEDIA2_NS = b"http://www.onvif.org/ver20/media/wsdl"


def is_media2(body):
    """Lệnh gửi tới Media2 (ver20) hay Media1 (ver10): cùng tên nhưng khác schema trả về."""
    m = OP_RE.search(body)
    if not m:
        return False
    prefix = m.group(1)
    if prefix:
        return re.search(rb'xmlns:' + re.escape(prefix) + rb'="' + re.escape(MEDIA2_NS) + rb'"', body) is not None
    return re.search(rb'xmlns="' + re.escape(MEDIA2_NS) + rb'"', body) is not None


def xml_text(body, tag):
    m = re.search(rb"<(?:[A-Za-z0-9_.-]+:)?" + tag.encode() + rb"(?:\s[^>]*)?>([^<]*)<", body)
    return m.group(1).decode("utf-8", "replace").strip() if m else None


def profile_of_token(token):
    """"VEC-Profile_1" hoặc "Profile_1" -> "Profile_1"."""
    if not token:
        return None
    return token[4:] if token.startswith("VEC-") else token


class Device(object):
    """Trạng thái của một camera ảo: preset PTZ và vị trí hiện tại."""

    def __init__(self, ip, index):
        self.ip = ip
        self.index = index
        self.lock = threading.Lock()
        self.pan = 0.0
        self.tilt = 0.0
        self.zoom = 0.0
        self.moves = 0
        self.presets = {
            "Preset1": {"name": "Home", "pan": 0.0, "tilt": 0.0, "zoom": 0.0},
            "Preset2": {"name": "Gate", "pan": 0.4, "tilt": -0.2, "zoom": 0.1},
        }
        # Cấu hình encoder theo profile token, và số lệnh set mỗi profile đã nhận.
        self.encoders = {
            "Profile_1": {"encoding": "H264", "width": 1920, "height": 1080, "fps": 25, "bitrate": 4096},
            "Profile_2": {"encoding": "H264", "width": 704, "height": 576, "fps": 15, "bitrate": 1024},
        }
        self.encoder_sets = defaultdict(int)


class Registry(object):
    def __init__(self, args):
        self.args = args
        self.lock = threading.Lock()
        self.devices = {}
        self.counts = defaultdict(int)
        self.faults = defaultdict(int)
        self.spec = {"default": {}, "devices": {}}
        self.spec_mtime = None
        self.started = time.time()

    def device(self, ip):
        with self.lock:
            dev = self.devices.get(ip)
            if dev is None:
                dev = Device(ip, len(self.devices) + 1)
                self.devices[ip] = dev
            return dev

    def policy(self, ip):
        merged = dict(self.spec.get("default", {}))
        merged.update(self.spec.get("devices", {}).get(ip, {}))
        return merged

    def reload(self):
        path = self.args.fault_spec
        if not path:
            return
        try:
            st = os.stat(path)
        except OSError:
            return
        if self.spec_mtime == st.st_mtime:
            return
        self.spec_mtime = st.st_mtime
        try:
            self.spec = json.load(open(path))
        except (ValueError, OSError):
            pass


REG = None


# ----------------------------------------------------------------- operations
def op_GetDeviceInformation(dev, body, port):
    return envelope(
        '<tds:GetDeviceInformationResponse>'
        '<tds:Manufacturer>S3MediaKit</tds:Manufacturer>'
        '<tds:Model>StressCam-%04d</tds:Model>'
        '<tds:FirmwareVersion>1.0.0</tds:FirmwareVersion>'
        '<tds:SerialNumber>SN%08d</tds:SerialNumber>'
        '<tds:HardwareId>HW-STRESS</tds:HardwareId>'
        '</tds:GetDeviceInformationResponse>' % (dev.index, dev.index))


def op_GetCapabilities(dev, body, port):
    base = "http://%s:%d" % (dev.ip, port)
    return envelope(
        '<tds:GetCapabilitiesResponse><tds:Capabilities>'
        '<tt:Device><tt:XAddr>%s/onvif/device_service</tt:XAddr>'
        '<tt:System><tt:DiscoveryResolve>false</tt:DiscoveryResolve>'
        '<tt:DiscoveryBye>false</tt:DiscoveryBye><tt:RemoteDiscovery>false</tt:RemoteDiscovery>'
        '<tt:SystemBackup>false</tt:SystemBackup><tt:SystemLogging>false</tt:SystemLogging>'
        '<tt:FirmwareUpgrade>false</tt:FirmwareUpgrade>'
        '<tt:SupportedVersions><tt:Major>2</tt:Major><tt:Minor>60</tt:Minor></tt:SupportedVersions>'
        '</tt:System></tt:Device>'
        '<tt:Media><tt:XAddr>%s/onvif/media_service</tt:XAddr>'
        '<tt:StreamingCapabilities><tt:RTPMulticast>false</tt:RTPMulticast>'
        '<tt:RTP_TCP>true</tt:RTP_TCP><tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP>'
        '</tt:StreamingCapabilities></tt:Media>'
        '<tt:PTZ><tt:XAddr>%s/onvif/ptz_service</tt:XAddr></tt:PTZ>'
        '<tt:Imaging><tt:XAddr>%s/onvif/imaging_service</tt:XAddr></tt:Imaging>'
        '%s'
        '</tds:Capabilities></tds:GetCapabilitiesResponse>'
        % (base, base, base, base, _profile_g_capabilities(base)))


def _profile_g_capabilities(base):
    if not REG.args.profile_g:
        return ""
    return ('<tt:Extension>'
            '<tt:Search><tt:XAddr>%s/onvif/search_service</tt:XAddr>'
            '<tt:MetadataSearch>false</tt:MetadataSearch></tt:Search>'
            '<tt:Replay><tt:XAddr>%s/onvif/replay_service</tt:XAddr></tt:Replay>'
            '</tt:Extension>' % (base, base))


def op_GetNetworkInterfaces(dev, body, port):
    mac = "02:00:%02x:%02x:%02x:%02x" % tuple(int(x) for x in dev.ip.split("."))
    return envelope(
        '<tds:GetNetworkInterfacesResponse><tds:NetworkInterfaces token="eth0">'
        '<tt:Enabled>true</tt:Enabled>'
        '<tt:Info><tt:Name>eth0</tt:Name><tt:HwAddress>%s</tt:HwAddress><tt:MTU>1500</tt:MTU></tt:Info>'
        '<tt:IPv4><tt:Enabled>true</tt:Enabled><tt:Config>'
        '<tt:Manual><tt:Address>%s</tt:Address><tt:PrefixLength>8</tt:PrefixLength></tt:Manual>'
        '<tt:DHCP>false</tt:DHCP></tt:Config></tt:IPv4>'
        '</tds:NetworkInterfaces></tds:GetNetworkInterfacesResponse>' % (mac, dev.ip))


# Đồng hồ riêng từng thiết bị (giây lệch so với giờ máy): --clock-offset-sec đặt giá trị đầu,
# SetSystemDateAndTime ghi đè như camera thật.
CLOCK = {}
CLOCK_LOCK = threading.Lock()


def dev_now(ip):
    with CLOCK_LOCK:
        return time.time() + CLOCK.get(ip, REG.args.clock_offset_sec)


def op_GetSystemDateAndTime(dev, body, port):
    t = time.gmtime(dev_now(dev.ip))
    return envelope(
        '<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime>'
        '<tt:DateTimeType>Manual</tt:DateTimeType><tt:DaylightSavings>false</tt:DaylightSavings>'
        '<tt:TimeZone><tt:TZ>UTC0</tt:TZ></tt:TimeZone>'
        '<tt:UTCDateTime><tt:Time><tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute><tt:Second>%d</tt:Second></tt:Time>'
        '<tt:Date><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month><tt:Day>%d</tt:Day></tt:Date></tt:UTCDateTime>'
        '</tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>'
        % (t.tm_hour, t.tm_min, t.tm_sec, t.tm_year, t.tm_mon, t.tm_mday))


def op_SetSystemDateAndTime(dev, body, port):
    text = body.decode("utf-8", "replace")
    utc = re.search(r"UTCDateTime>(.*?)</(?:\w+:)?UTCDateTime", text, re.S)
    if utc:
        part = utc.group(1)
        get = lambda name: int(re.search(r"<(?:\w+:)?%s>(\d+)<" % name, part).group(1))
        t = calendar.timegm((get("Year"), get("Month"), get("Day"), get("Hour"), get("Minute"), get("Second")))
        with CLOCK_LOCK:
            CLOCK[dev.ip] = t - time.time()
        with REG.lock:
            REG.counts["_clock_set"] += 1
            REG.counts["_clock_set@" + dev.ip] += 1
    return envelope('<tds:SetSystemDateAndTimeResponse/>')


def _profile(token, name, width, height, fps, bitrate, ip):
    return (
        '<trt:Profiles token="%s" fixed="true"><tt:Name>%s</tt:Name>'
        '<tt:VideoSourceConfiguration token="VSC0"><tt:Name>VSC0</tt:Name>'
        '<tt:UseCount>2</tt:UseCount><tt:SourceToken>VS0</tt:SourceToken>'
        '<tt:Bounds x="0" y="0" width="%d" height="%d"/></tt:VideoSourceConfiguration>'
        '<tt:VideoEncoderConfiguration token="VEC-%s"><tt:Name>VEC-%s</tt:Name>'
        '<tt:UseCount>1</tt:UseCount><tt:Encoding>H264</tt:Encoding>'
        '<tt:Resolution><tt:Width>%d</tt:Width><tt:Height>%d</tt:Height></tt:Resolution>'
        '<tt:Quality>4</tt:Quality>'
        '<tt:RateControl><tt:FrameRateLimit>%d</tt:FrameRateLimit>'
        '<tt:EncodingInterval>1</tt:EncodingInterval><tt:BitrateLimit>%d</tt:BitrateLimit></tt:RateControl>'
        '<tt:H264><tt:GovLength>10</tt:GovLength><tt:H264Profile>Baseline</tt:H264Profile></tt:H264>'
        '<tt:SessionTimeout>PT60S</tt:SessionTimeout></tt:VideoEncoderConfiguration>'
        '<tt:PTZConfiguration token="PTZ0"><tt:Name>PTZ0</tt:Name><tt:UseCount>1</tt:UseCount>'
        '<tt:NodeToken>PTZNODE0</tt:NodeToken>'
        '<tt:DefaultContinuousPanTiltVelocitySpace>'
        'http://www.onvif.org/ver10/tptz/PanTiltSpaces/VelocityGenericSpace'
        '</tt:DefaultContinuousPanTiltVelocitySpace>'
        '</tt:PTZConfiguration></trt:Profiles>'
        % (token, name, width, height, token, token, width, height, fps, bitrate))


def op_GetProfiles(dev, body, port):
    with dev.lock:
        main = dict(dev.encoders["Profile_1"])
        sub = dict(dev.encoders["Profile_2"])
    return envelope(
        '<trt:GetProfilesResponse>%s%s</trt:GetProfilesResponse>'
        % (_profile("Profile_1", "MainStream", main["width"], main["height"], main["fps"],
                    main["bitrate"], dev.ip),
           _profile("Profile_2", "SubStream", sub["width"], sub["height"], sub["fps"],
                    sub["bitrate"], dev.ip)))


def op_GetStreamUri(dev, body, port):
    token = "Profile_1"
    m = re.search(rb"<(?:[A-Za-z0-9_.-]+:)?ProfileToken>([^<]*)<", body)
    if m:
        token = m.group(1).decode("utf-8", "replace")
    stream = "main" if token.endswith("_1") else "sub"
    uri = stream_uri(dev, stream)
    if is_media2(body):
        return envelope(
            '<tr2:GetStreamUriResponse><tr2:Uri>%s</tr2:Uri></tr2:GetStreamUriResponse>' % uri)
    return envelope(
        '<trt:GetStreamUriResponse><trt:MediaUri>'
        '<tt:Uri>%s</tt:Uri><tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>'
        '<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot><tt:Timeout>PT60S</tt:Timeout>'
        '</trt:MediaUri></trt:GetStreamUriResponse>' % uri)


def stream_uri(dev, stream):
    """URI luồng của thiết bị. Với --stream-base thì trùng URL camera mà setup_stress.py
    khai báo (cam-NNNN-main/sub, suy ngược từ địa chỉ 127.1.x.y), như một camera thật: server
    nhận ra profile chính bằng cách so URI này với URL luồng chính của camera."""
    base = REG.args.stream_base
    parts = dev.ip.split(".")
    if not base or len(parts) != 4 or parts[1] != "1":
        return "rtsp://%s:554/%s" % (dev.ip, stream)
    index = (int(parts[2]) - 1) * 250 + int(parts[3]) - 1
    return "%s/cam-%04d-%s" % (base.rstrip("/"), index, stream)


def _encoder2(token, cfg):
    return (
        '<tr2:Configurations token="VEC-%s" GovLength="10" Profile="Baseline">'
        '<tt:Name>VEC-%s</tt:Name><tt:UseCount>1</tt:UseCount><tt:Encoding>%s</tt:Encoding>'
        '<tt:Resolution><tt:Width>%d</tt:Width><tt:Height>%d</tt:Height></tt:Resolution>'
        '<tt:RateControl ConstantBitRate="false"><tt:FrameRateLimit>%s</tt:FrameRateLimit>'
        '<tt:BitrateLimit>%d</tt:BitrateLimit></tt:RateControl>'
        '<tt:Quality>4</tt:Quality></tr2:Configurations>'
        % (token, token, cfg["encoding"], cfg["width"], cfg["height"], _fps_text(cfg["fps"]),
           cfg["bitrate"]))


def _fps_text(fps):
    return ("%d" % fps) if float(fps) == int(fps) else ("%g" % fps)


def _requested_profiles(dev, body):
    token = profile_of_token(xml_text(body, "ConfigurationToken") or xml_text(body, "ProfileToken"))
    if token in dev.encoders:
        return [token]
    return sorted(dev.encoders)


def op_GetVideoEncoderConfigurations(dev, body, port):
    with dev.lock:
        tokens = _requested_profiles(dev, body)
        configs = [(t, dict(dev.encoders[t])) for t in tokens]
    if is_media2(body):
        return envelope(
            '<tr2:GetVideoEncoderConfigurationsResponse>%s</tr2:GetVideoEncoderConfigurationsResponse>'
            % "".join(_encoder2(t, c) for t, c in configs))
    token, cfg = configs[0]
    return envelope(
        '<trt:GetVideoEncoderConfigurationsResponse>'
        '<trt:Configurations token="VEC-%s"><tt:Name>VEC-%s</tt:Name>'
        '<tt:UseCount>1</tt:UseCount><tt:Encoding>H264</tt:Encoding>'
        '<tt:Resolution><tt:Width>%d</tt:Width><tt:Height>%d</tt:Height></tt:Resolution>'
        '<tt:Quality>4</tt:Quality><tt:RateControl><tt:FrameRateLimit>%d</tt:FrameRateLimit>'
        '<tt:EncodingInterval>1</tt:EncodingInterval><tt:BitrateLimit>%d</tt:BitrateLimit></tt:RateControl>'
        '<tt:H264><tt:GovLength>10</tt:GovLength><tt:H264Profile>Baseline</tt:H264Profile></tt:H264>'
        '<tt:SessionTimeout>PT60S</tt:SessionTimeout></trt:Configurations>'
        '</trt:GetVideoEncoderConfigurationsResponse>'
        % (token, token, cfg["width"], cfg["height"], int(cfg["fps"]), cfg["bitrate"]))


RESOLUTIONS = [(1920, 1080), (1280, 720), (704, 576), (640, 360)]


def op_GetVideoEncoderConfigurationOptions(dev, body, port):
    resolutions = "".join(
        '<tt:ResolutionsAvailable><tt:Width>%d</tt:Width><tt:Height>%d</tt:Height></tt:ResolutionsAvailable>'
        % wh for wh in RESOLUTIONS)
    if is_media2(body):
        return envelope(
            '<tr2:GetVideoEncoderConfigurationOptionsResponse>'
            '<tr2:Options GovLengthRange="1 60" FrameRatesSupported="30 25 20 15 12 10 8 5 1" '
            'ProfilesSupported="Baseline Main High" ConstantBitRateSupported="false">'
            '<tt:Encoding>H264</tt:Encoding>'
            '<tt:QualityRange><tt:Min>1</tt:Min><tt:Max>6</tt:Max></tt:QualityRange>%s'
            '<tt:BitrateRange><tt:Min>256</tt:Min><tt:Max>8192</tt:Max></tt:BitrateRange>'
            '</tr2:Options></tr2:GetVideoEncoderConfigurationOptionsResponse>' % resolutions)
    return envelope(
        '<trt:GetVideoEncoderConfigurationOptionsResponse><trt:Options>'
        '<tt:QualityRange><tt:Min>1</tt:Min><tt:Max>6</tt:Max></tt:QualityRange>'
        '<tt:H264>%s'
        '<tt:GovLengthRange><tt:Min>1</tt:Min><tt:Max>60</tt:Max></tt:GovLengthRange>'
        '<tt:FrameRateRange><tt:Min>1</tt:Min><tt:Max>30</tt:Max></tt:FrameRateRange>'
        '<tt:EncodingIntervalRange><tt:Min>1</tt:Min><tt:Max>1</tt:Max></tt:EncodingIntervalRange>'
        '<tt:BitrateRange><tt:Min>256</tt:Min><tt:Max>8192</tt:Max></tt:BitrateRange>'
        '</tt:H264></trt:Options></trt:GetVideoEncoderConfigurationOptionsResponse>' % resolutions)


def op_SetVideoEncoderConfiguration(dev, body, port):
    m = re.search(rb'<(?:[A-Za-z0-9_.-]+:)?Configuration\s[^>]*token="([^"]*)"', body)
    token = profile_of_token(m.group(1).decode("utf-8", "replace") if m else None)
    with dev.lock:
        if token not in dev.encoders:
            raise ValueError("unknown configuration token %r" % token)
        cfg = dev.encoders[token]
        for key, tag, conv in (("encoding", "Encoding", str), ("width", "Width", int),
                               ("height", "Height", int), ("fps", "FrameRateLimit", float),
                               ("bitrate", "BitrateLimit", int)):
            value = xml_text(body, tag)
            if value:
                cfg[key] = conv(value)
        if float(cfg["fps"]) == int(cfg["fps"]):
            cfg["fps"] = int(cfg["fps"])
        dev.encoder_sets[token] += 1
    if is_media2(body):
        return envelope('<tr2:SetVideoEncoderConfigurationResponse/>')
    return envelope('<trt:SetVideoEncoderConfigurationResponse/>')


def op_GetAudioOutputs(dev, body, port):
    return envelope('<trt:GetAudioOutputsResponse/>')


def op_GetAudioSources(dev, body, port):
    return envelope('<trt:GetAudioSourcesResponse/>')


def op_GetRelayOutputs(dev, body, port):
    return envelope('<tds:GetRelayOutputsResponse/>')


def op_GetPresets(dev, body, port):
    with dev.lock:
        items = "".join(
            '<tptz:Preset token="%s"><tt:Name>%s</tt:Name>'
            '<tt:PTZPosition><tt:PanTilt x="%.3f" y="%.3f"/><tt:Zoom x="%.3f"/></tt:PTZPosition>'
            '</tptz:Preset>' % (tok, p["name"], p["pan"], p["tilt"], p["zoom"])
            for tok, p in sorted(dev.presets.items()))
    return envelope('<tptz:GetPresetsResponse>%s</tptz:GetPresetsResponse>' % items)


def op_SetPreset(dev, body, port):
    m = re.search(rb"<(?:[A-Za-z0-9_.-]+:)?PresetName>([^<]*)<", body)
    name = m.group(1).decode("utf-8", "replace") if m else "Preset"
    m = re.search(rb"<(?:[A-Za-z0-9_.-]+:)?PresetToken>([^<]*)<", body)
    with dev.lock:
        token = m.group(1).decode("utf-8", "replace") if m else "Preset%d" % (len(dev.presets) + 1)
        dev.presets[token] = {"name": name, "pan": dev.pan, "tilt": dev.tilt, "zoom": dev.zoom}
    return envelope('<tptz:SetPresetResponse><tptz:PresetToken>%s</tptz:PresetToken>'
                    '</tptz:SetPresetResponse>' % token)


def op_RemovePreset(dev, body, port):
    m = re.search(rb"<(?:[A-Za-z0-9_.-]+:)?PresetToken>([^<]*)<", body)
    if m:
        with dev.lock:
            dev.presets.pop(m.group(1).decode("utf-8", "replace"), None)
    return envelope('<tptz:RemovePresetResponse/>')


def op_GotoPreset(dev, body, port):
    m = re.search(rb"<(?:[A-Za-z0-9_.-]+:)?PresetToken>([^<]*)<", body)
    with dev.lock:
        preset = dev.presets.get(m.group(1).decode("utf-8", "replace")) if m else None
        if preset:
            dev.pan, dev.tilt, dev.zoom = preset["pan"], preset["tilt"], preset["zoom"]
        dev.moves += 1
    return envelope('<tptz:GotoPresetResponse/>')


def _clamp(v):
    return max(-1.0, min(1.0, v))


def _apply_move(dev, body, relative):
    px = re.search(rb'PanTilt[^>]*\bx="([-0-9.]+)"', body)
    py = re.search(rb'PanTilt[^>]*\by="([-0-9.]+)"', body)
    pz = re.search(rb'Zoom[^>]*\bx="([-0-9.]+)"', body)
    with dev.lock:
        dx = float(px.group(1)) if px else 0.0
        dy = float(py.group(1)) if py else 0.0
        dz = float(pz.group(1)) if pz else 0.0
        if relative:
            dev.pan, dev.tilt, dev.zoom = _clamp(dev.pan + dx), _clamp(dev.tilt + dy), _clamp(dev.zoom + dz)
        else:
            dev.pan, dev.tilt, dev.zoom = _clamp(dx), _clamp(dy), _clamp(dz)
        dev.moves += 1


def op_ContinuousMove(dev, body, port):
    _apply_move(dev, body, True)
    return envelope('<tptz:ContinuousMoveResponse/>')


def op_RelativeMove(dev, body, port):
    _apply_move(dev, body, True)
    return envelope('<tptz:RelativeMoveResponse/>')


def op_AbsoluteMove(dev, body, port):
    _apply_move(dev, body, False)
    return envelope('<tptz:AbsoluteMoveResponse/>')


def op_Stop(dev, body, port):
    return envelope('<tptz:StopResponse/>')


def op_GotoHomePosition(dev, body, port):
    with dev.lock:
        dev.pan = dev.tilt = dev.zoom = 0.0
        dev.moves += 1
    return envelope('<tptz:GotoHomePositionResponse/>')


def op_GetStatus(dev, body, port):
    with dev.lock:
        pan, tilt, zoom = dev.pan, dev.tilt, dev.zoom
    return envelope(
        '<tptz:GetStatusResponse><tptz:PTZStatus>'
        '<tt:Position><tt:PanTilt x="%.3f" y="%.3f"/><tt:Zoom x="%.3f"/></tt:Position>'
        '<tt:MoveStatus><tt:PanTilt>IDLE</tt:PanTilt><tt:Zoom>IDLE</tt:Zoom></tt:MoveStatus>'
        '<tt:UtcTime>%s</tt:UtcTime></tptz:PTZStatus></tptz:GetStatusResponse>'
        % (pan, tilt, zoom, time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())))


def _pan_tilt_space(tag, uri):
    return ('<tt:%s><tt:URI>%s</tt:URI>'
            '<tt:XRange><tt:Min>-1.0</tt:Min><tt:Max>1.0</tt:Max></tt:XRange>'
            '<tt:YRange><tt:Min>-1.0</tt:Min><tt:Max>1.0</tt:Max></tt:YRange>'
            '</tt:%s>' % (tag, uri, tag))


def _zoom_space(tag, uri):
    return ('<tt:%s><tt:URI>%s</tt:URI>'
            '<tt:XRange><tt:Min>-1.0</tt:Min><tt:Max>1.0</tt:Max></tt:XRange>'
            '</tt:%s>' % (tag, uri, tag))


PT_SPACES = [
    ("AbsolutePanTiltPositionSpace", "AbsoluteZoomPositionSpace",
     "http://www.onvif.org/ver10/tptz/PanTiltSpaces/PositionGenericSpace",
     "http://www.onvif.org/ver10/tptz/ZoomSpaces/PositionGenericSpace"),
    ("ContinuousPanTiltVelocitySpace", "ContinuousZoomVelocitySpace",
     "http://www.onvif.org/ver10/tptz/PanTiltSpaces/VelocityGenericSpace",
     "http://www.onvif.org/ver10/tptz/ZoomSpaces/VelocityGenericSpace"),
    ("RelativePanTiltTranslationSpace", "RelativeZoomTranslationSpace",
     "http://www.onvif.org/ver10/tptz/PanTiltSpaces/TranslationGenericSpace",
     "http://www.onvif.org/ver10/tptz/ZoomSpaces/TranslationGenericSpace"),
]


def op_GetConfigurationOptions(dev, body, port):
    # `ptz_omit_zoom_spaces` mô phỏng camera chỉ khai báo pan/tilt mà không khai báo
    # zoom — hợp lệ theo ONVIF. CẢNH BÁO: bật cờ này làm MediaServer segfault tại
    # OnvifDeviceControl.cpp:401 (xem README, mục "Lỗi đã phát hiện"). Mặc định tắt.
    omit_zoom = REG.policy(dev.ip).get("ptz_omit_zoom_spaces", False)
    spaces = []
    for pt_tag, zoom_tag, pt_uri, zoom_uri in PT_SPACES:
        spaces.append(_pan_tilt_space(pt_tag, pt_uri))
        if not omit_zoom:
            spaces.append(_zoom_space(zoom_tag, zoom_uri))
    return envelope(
        '<tptz:GetConfigurationOptionsResponse><tptz:PTZConfigurationOptions>'
        '<tt:Spaces>%s</tt:Spaces>'
        '<tt:PTZTimeout><tt:Min>PT1S</tt:Min><tt:Max>PT10S</tt:Max></tt:PTZTimeout>'
        '</tptz:PTZConfigurationOptions></tptz:GetConfigurationOptionsResponse>'
        % "".join(spaces))


# ------------------------------------------------------------ Profile G (thẻ SD)
# Chỉ bật với --profile-g. Mỗi camera có hai bản ghi trên thẻ (REC_main, REC_sub, đúng thứ tự
# server sắp theo token), ghi liên tục từ --sd-from-sec trước lúc mock khởi động tới hiện tại.
# Token tìm kiếm mang luôn tham số (không giữ trạng thái), nên mock đa luồng không cần khoá.
PROFILE_G_OPS = ("GetRecordingSummary", "FindRecordings", "GetRecordingSearchResults", "EndSearch",
                 "GetReplayUri", "GetMediaAttributes", "FindEvents", "GetEventSearchResults")


def _iso(t):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t))


def _parse_iso(text):
    if not text:
        return None
    text = text.rstrip("Z").split(".")[0]
    try:
        return calendar.timegm(time.strptime(text, "%Y-%m-%dT%H:%M:%S"))
    except ValueError:
        return None


def _sd_window(dev):
    return REG.started - REG.args.sd_from_sec, dev_now(dev.ip)


def op_GetRecordingSummary(dev, body, port):
    start, until = _sd_window(dev)
    return envelope(
        '<tse:GetRecordingSummaryResponse><tse:Summary>'
        '<tt:DataFrom>%s</tt:DataFrom><tt:DataUntil>%s</tt:DataUntil>'
        '<tt:NumberRecordings>2</tt:NumberRecordings>'
        '</tse:Summary></tse:GetRecordingSummaryResponse>' % (_iso(start), _iso(until)))


def op_FindRecordings(dev, body, port):
    return envelope('<tse:FindRecordingsResponse><tse:SearchToken>REC-SEARCH</tse:SearchToken>'
                    '</tse:FindRecordingsResponse>')


def _recording_info(dev, token):
    start, until = _sd_window(dev)
    return (
        '<tt:RecordingInformation><tt:RecordingToken>%s</tt:RecordingToken>'
        '<tt:Source><tt:SourceId>%s</tt:SourceId><tt:Name>%s</tt:Name><tt:Location>sd</tt:Location>'
        '<tt:Description>sd</tt:Description><tt:Address>%s</tt:Address></tt:Source>'
        '<tt:EarliestRecording>%s</tt:EarliestRecording><tt:LatestRecording>%s</tt:LatestRecording>'
        '<tt:Content>continuous</tt:Content>'
        '<tt:Track><tt:TrackToken>VIDEO001</tt:TrackToken><tt:TrackType>Video</tt:TrackType>'
        '<tt:Description>video</tt:Description><tt:DataFrom>%s</tt:DataFrom><tt:DataTo>%s</tt:DataTo>'
        '</tt:Track><tt:RecordingStatus>Recording</tt:RecordingStatus></tt:RecordingInformation>'
        % (token, token, token, dev.ip, _iso(start), _iso(until), _iso(start), _iso(until)))


def op_GetRecordingSearchResults(dev, body, port):
    return envelope(
        '<tse:GetRecordingSearchResultsResponse><tse:ResultList>'
        '<tt:SearchState>Completed</tt:SearchState>%s%s'
        '</tse:ResultList></tse:GetRecordingSearchResultsResponse>'
        % (_recording_info(dev, "REC_main"), _recording_info(dev, "REC_sub")))


def op_EndSearch(dev, body, port):
    return envelope('<tse:EndSearchResponse><tse:Endpoint>%s</tse:Endpoint></tse:EndSearchResponse>'
                    % _iso(dev_now(dev.ip)))


def op_GetReplayUri(dev, body, port):
    token = xml_text(body, "RecordingToken") or "REC_main"
    stream = "sub" if token.endswith("sub") else "main"
    live = stream_uri(dev, stream)
    base, name = live.rsplit("/", 1)
    # rtsp_source.py phát hữu hạn cho path "replay-*"; "?rec=1" để server nối "&starttime=".
    with REG.lock:
        REG.counts["_replay_uri"] += 1
    return envelope('<trp:GetReplayUriResponse><trp:Uri>%s/replay-%s?rec=1</trp:Uri>'
                    '</trp:GetReplayUriResponse>' % (base, name))


def op_GetMediaAttributes(dev, body, port):
    token = xml_text(body, "RecordingTokens") or "REC_main"
    cfg = dev.encoders["Profile_2" if token.endswith("sub") else "Profile_1"]
    start, until = _sd_window(dev)
    return envelope(
        '<tse:GetMediaAttributesResponse><tse:MediaAttributes>'
        '<tt:RecordingToken>%s</tt:RecordingToken><tt:TrackAttributes>'
        '<tt:TrackInformation><tt:TrackToken>VIDEO001</tt:TrackToken><tt:TrackType>Video</tt:TrackType>'
        '<tt:Description>video</tt:Description><tt:DataFrom>%s</tt:DataFrom><tt:DataTo>%s</tt:DataTo>'
        '</tt:TrackInformation><tt:VideoAttributes><tt:Bitrate>%d</tt:Bitrate>'
        '<tt:Width>%d</tt:Width><tt:Height>%d</tt:Height><tt:Encoding>%s</tt:Encoding>'
        '<tt:Framerate>%d</tt:Framerate></tt:VideoAttributes></tt:TrackAttributes>'
        '<tt:From>%s</tt:From><tt:Until>%s</tt:Until></tse:MediaAttributes>'
        '</tse:GetMediaAttributesResponse>'
        % (token, _iso(start), _iso(until), cfg["bitrate"], cfg["width"], cfg["height"],
           cfg["encoding"], cfg["fps"], _iso(start), _iso(until)))


def op_FindEvents(dev, body, port):
    start = _parse_iso(xml_text(body, "StartPoint")) or int(dev_now(dev.ip))
    end = _parse_iso(xml_text(body, "EndPoint")) or int(dev_now(dev.ip))
    token = xml_text(body, "IncludedRecordings") or "REC_main"
    with REG.lock:
        REG.counts["_find_events"] += 1
    return envelope('<tse:FindEventsResponse><tse:SearchToken>EV|%s|%d|%d</tse:SearchToken>'
                    '</tse:FindEventsResponse>' % (token, start, end))


def _track_event(token, when, start_state, present):
    return (
        '<tt:Result><tt:RecordingToken>%s</tt:RecordingToken><tt:TrackToken>VIDEO001</tt:TrackToken>'
        '<tt:Time>%s</tt:Time><tt:Event><wsnt:Message><tt:Message UtcTime="%s">'
        '<tt:Source><tt:SimpleItem Name="RecordingToken" Value="%s"/></tt:Source>'
        '<tt:Data><tt:SimpleItem Name="IsDataPresent" Value="%s"/></tt:Data>'
        '</tt:Message></wsnt:Message></tt:Event>'
        '<tt:StartStateEvent>%s</tt:StartStateEvent></tt:Result>'
        % (token, _iso(when), _iso(when), token, "true" if present else "false",
           "true" if start_state else "false"))


def op_GetEventSearchResults(dev, body, port):
    parts = (xml_text(body, "SearchToken") or "EV|REC_main|0|0").split("|")
    token, start, end = parts[1], int(parts[2]), int(parts[3])
    # Thẻ ghi liên tục: có dữ liệu từ đầu khoảng tìm, tới cuối khoảng.
    events = _track_event(token, start, True, True) + _track_event(token, end, False, False)
    return envelope(
        '<tse:GetEventSearchResultsResponse><tse:ResultList>'
        '<tt:SearchState>Completed</tt:SearchState>%s'
        '</tse:ResultList></tse:GetEventSearchResultsResponse>' % events)


HANDLERS = {name[3:]: fn for name, fn in list(globals().items()) if name.startswith("op_")}


# ------------------------------------------------------------ HTTP auth
# --auth digest/basic mô phỏng camera đặt xác thực dịch vụ ONVIF là "digest" (hoặc chỉ Basic):
# bỏ qua WS-Security, request thiếu/sai Authorization bị 401 kèm challenge. Mặc định "none"
# giữ hành vi cũ (không kiểm tra gì). GetSystemDateAndTime luôn không cần xác thực (chuẩn ONVIF).
# Nonce riêng cho từng thiết bị (theo IP đích) như camera thật; dùng chung thì camera này làm
# hết hạn nonce của camera khác ngay giữa lúc nhận challenge và gửi lại.
class DigestNonces:
    def __init__(self):
        self.lock = threading.Lock()
        self.gen = defaultdict(lambda: 1)
        self.used = defaultdict(int)

    def current(self, ip):
        with self.lock:
            return "s3mk%08x" % self.gen[ip]

    def consume(self, ip, ttl_uses):
        with self.lock:
            self.used[ip] += 1
            if ttl_uses > 0 and self.used[ip] >= ttl_uses:
                self.gen[ip] += 1
                self.used[ip] = 0


NONCES = DigestNonces()


def _auth_params(s):
    out = {}
    for m in re.finditer(r'([A-Za-z0-9_-]+)\s*=\s*("([^"]*)"|[^,\s]+)', s):
        out[m.group(1).lower()] = m.group(3) if m.group(3) is not None else m.group(2)
    return out


def check_http_auth(ip, header, path):
    """Trả (ok, stale)."""
    a = REG.args
    if a.auth == "basic":
        expect = "Basic " + base64.b64encode(("%s:%s" % (a.auth_user, a.auth_pass)).encode()).decode()
        return header == expect, False
    if not header.lower().startswith("digest "):
        return False, False
    p = _auth_params(header[7:])
    if p.get("nonce") != NONCES.current(ip):
        return False, True
    md5 = lambda x: hashlib.md5(x.encode()).hexdigest()
    ha1 = md5("%s:%s:%s" % (a.auth_user, "s3mk-onvif-mock", a.auth_pass))
    ha2 = md5("POST:%s" % p.get("uri", ""))
    if p.get("qop"):
        expect = md5(":".join([ha1, p["nonce"], p.get("nc", ""), p.get("cnonce", ""), p["qop"], ha2]))
    else:
        expect = md5(":".join([ha1, p["nonce"], ha2]))
    if p.get("uri") != path or p.get("response") != expect or p.get("username") != a.auth_user:
        return False, False
    NONCES.consume(ip, a.nonce_uses)
    return True, False


def check_wsse(ip, body):
    """WS-UsernameToken PasswordDigest = Base64(SHA1(nonce + Created + password)), Created phải nằm
    trong --wsse-window-sec quanh đồng hồ của thiết bị (như camera thật)."""
    text = body.decode("utf-8", "replace")
    tok = re.search(r"<(?:\w+:)?UsernameToken[^>]*>(.*?)</(?:\w+:)?UsernameToken>", text, re.S)
    if not tok:
        return False
    tag = lambda name: (re.search(r"<(?:\w+:)?%s[^>]*>(.*?)</" % name, tok.group(1), re.S) or [None, ""])[1]
    user, pw, nonce, created = tag("Username"), tag("Password"), tag("Nonce"), tag("Created")
    a = REG.args
    if user != a.auth_user or not created:
        return False
    try:
        raw_nonce = base64.b64decode(nonce)
        ts = calendar.timegm(time.strptime(created.split(".")[0].rstrip("Z"), "%Y-%m-%dT%H:%M:%S"))
    except Exception:
        return False
    expect = base64.b64encode(hashlib.sha1(raw_nonce + created.encode() + a.auth_pass.encode()).digest()).decode()
    if pw != expect:
        return False
    return abs(ts - dev_now(ip)) <= a.wsse_window_sec


def fault_sender(code, reason):
    return envelope(
        '<SOAP-ENV:Fault><SOAP-ENV:Code><SOAP-ENV:Value>SOAP-ENV:Sender</SOAP-ENV:Value>'
        '<SOAP-ENV:Subcode><SOAP-ENV:Value>ter:%s</SOAP-ENV:Value></SOAP-ENV:Subcode>'
        '</SOAP-ENV:Code><SOAP-ENV:Reason><SOAP-ENV:Text xml:lang="en">%s</SOAP-ENV:Text>'
        '</SOAP-ENV:Reason></SOAP-ENV:Fault>' % (code, reason))


def auth_challenge(ip, stale):
    if REG.args.auth == "basic":
        return 'Basic realm="s3mk-onvif-mock"'
    return ('Digest realm="s3mk-onvif-mock", qop="auth", nonce="%s", opaque="0f0f"%s'
            % (NONCES.current(ip), ", stale=TRUE" if stale else ""))


# -------------------------------------------------------------------- HTTP
class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "s3mk-onvif-mock"

    def date_time_string(self, timestamp=None):
        # Header Date theo đồng hồ của thiết bị: client dùng nó khi không đọc được giờ camera.
        ip = getattr(self, "_dev_ip", None)
        if timestamp is None and ip:
            timestamp = dev_now(ip)
        return BaseHTTPRequestHandler.date_time_string(self, timestamp)

    def log_message(self, fmt, *a):
        if REG.args.verbose:
            sys.stderr.write("[onvif] %s\n" % (fmt % a))

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0) or 0)
        body = self.rfile.read(length) if length else b""
        ip = self.connection.getsockname()[0]
        dev = REG.device(ip)
        policy = REG.policy(ip)

        if policy.get("offline"):
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.close_connection = True
            return

        latency = float(policy.get("latency_ms", 0)) / 1000.0
        if latency > 0:
            time.sleep(latency)

        m = OP_RE.search(body)
        op = m.group(2).decode("utf-8", "replace") if m else ""
        with REG.lock:
            REG.counts[op] += 1
        if REG.args.verbose:
            sys.stderr.write("[onvif] %s %s %s\n" % (ip, self.path, op or "<?>"))
            sys.stderr.flush()

        self._dev_ip = ip
        a = REG.args
        need_auth = op != "GetSystemDateAndTime" or a.time_requires_auth
        if need_auth and (a.auth != "none" or a.wsse_check):
            # Camera "digest&ws-username token": một trong hai là đủ.
            http_ok, stale = (check_http_auth(ip, self.headers.get("Authorization", ""), self.path)
                              if a.auth != "none" else (False, False))
            wsse_ok = a.wsse_check and check_wsse(ip, body)
            if not (http_ok or wsse_ok):
                if a.auth != "none":
                    with REG.lock:
                        REG.counts["_http_401"] += 1
                    self.send_response(401)
                    self.send_header("WWW-Authenticate", auth_challenge(ip, stale))
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                with REG.lock:
                    REG.counts["_wsse_reject"] += 1
                return self.reply(fault_sender("NotAuthorized", "Sender not Authorized"), 400)

        if op in policy.get("fail_ops", []):
            with REG.lock:
                REG.faults[op] += 1
            return self.reply(fault("ActionNotSupported", "injected failure"), 500)

        handler = HANDLERS.get(op)
        if op in PROFILE_G_OPS and not a.profile_g:
            handler = None
        if handler is None:
            with REG.lock:
                REG.faults[op] += 1
            return self.reply(fault("ActionNotSupported", "operation %s not supported" % op), 500)
        try:
            xml = handler(dev, body, REG.args.port)
        except Exception as exc:  # mock phải sống sót mọi payload lạ
            with REG.lock:
                REG.faults[op] += 1
            return self.reply(fault("Action", "mock error: %s" % exc), 500)
        self.reply(xml, 200)

    def do_GET(self):
        # Cho kịch bản kiểm tra đọc cấu hình encoder hiện tại, không đi qua SOAP.
        if self.path.split("?", 1)[0] != "/_mock/encoders":
            return self.reply(fault("ActionNotSupported", "not found"), 404)
        with REG.lock:
            device_list = list(REG.devices.values())
        out = {}
        for dev in device_list:
            with dev.lock:
                out[dev.ip] = {t: dict(c, sets=dev.encoder_sets[t]) for t, c in dev.encoders.items()}
        data = json.dumps(out).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def reply(self, xml, status):
        data = xml.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", 'application/soap+xml; charset=utf-8')
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def spec_reloader():
    # Tách khỏi reporter: kịch bản đổi fault spec giữa chừng (rút mạng camera) cần mock
    # thấy ngay, không phải chờ hết chu kỳ in thống kê.
    while True:
        time.sleep(1.0)
        REG.reload()


def reporter():
    last = 0
    while True:
        time.sleep(REG.args.stats_sec)
        with REG.lock:
            total = sum(REG.counts.values())
            top = sorted(REG.counts.items(), key=lambda kv: -kv[1])[:6]
            faults = sum(REG.faults.values())
            snapshot = dict(REG.counts)
            fault_snapshot = dict(REG.faults)
            devices = len(REG.devices)
            device_list = list(REG.devices.values())
        encoders = {}
        for dev in device_list:
            with dev.lock:
                encoders[dev.ip] = {t: dict(c, sets=dev.encoder_sets[t]) for t, c in dev.encoders.items()}
        print("[onvif] devices=%d calls=%d (+%d) faults=%d top=%s"
              % (devices, total, total - last, faults,
                 ", ".join("%s:%d" % kv for kv in top)), flush=True)
        last = total
        if REG.args.state:
            tmp = REG.args.state + ".tmp"
            with open(tmp, "w") as fh:
                json.dump({"devices": devices, "calls": total, "faults": faults,
                           "by_op": snapshot, "fault_by_op": fault_snapshot,
                           "encoders": encoders,
                           "uptime_sec": time.time() - REG.started}, fh, indent=1)
            os.replace(tmp, REG.args.state)


def main():
    global REG
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=18580)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--fault-spec", default=None)
    ap.add_argument("--state", default=None)
    ap.add_argument("--stats-sec", type=float, default=15.0)
    ap.add_argument("--stream-base", default=None,
                    help="vd rtsp://127.0.0.1:18554: GetStreamUri trả URL luồng của harness")
    ap.add_argument("--auth", choices=["none", "digest", "basic"], default="none",
                    help="digest/basic: bắt buộc xác thực HTTP như camera đặt chế độ \"digest\"")
    ap.add_argument("--auth-user", default="admin")
    ap.add_argument("--auth-pass", default="admin")
    ap.add_argument("--wsse-check", action="store_true",
                    help="kiểm WS-UsernameToken thật (PasswordDigest + Created trong cửa sổ quanh giờ thiết bị)")
    ap.add_argument("--wsse-window-sec", type=float, default=10.0)
    ap.add_argument("--clock-offset-sec", type=float, default=0.0,
                    help="đồng hồ ban đầu của mọi thiết bị lệch N giây (SetSystemDateAndTime sửa lại)")
    ap.add_argument("--time-requires-auth", action="store_true",
                    help="GetSystemDateAndTime cũng bắt xác thực (camera không theo chuẩn)")
    ap.add_argument("--nonce-uses", type=int, default=0,
                    help="digest: nonce hết hạn sau N request hợp lệ (0 = không hết hạn)")
    ap.add_argument("--profile-g", action="store_true",
                    help="khai báo Search/Replay: mỗi camera có thẻ SD ghi liên tục (gap fill)")
    ap.add_argument("--sd-from-sec", type=float, default=86400.0,
                    help="thẻ SD có dữ liệu từ N giây trước lúc mock khởi động")
    ap.add_argument("--verbose", action="store_true")
    REG = Registry(ap.parse_args())
    REG.reload()

    srv = ThreadingHTTPServer((REG.args.host, REG.args.port), Handler)
    srv.daemon_threads = True
    srv.request_queue_size = 256
    threading.Thread(target=reporter, daemon=True).start()
    threading.Thread(target=spec_reloader, daemon=True).start()
    print("[onvif] nghe tại %s:%d, %d operation được hỗ trợ"
          % (REG.args.host, REG.args.port, len(HANDLERS)), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
