#!/usr/bin/env python3
"""Mock camera PSIA (REST/XML dưới /PSIA, xác thực HTTP Digest) cho PsiaControl và IsapiControl.

Một camera ảo trên một cổng. Mọi đường dẫn ngoài /PSIA trả 404, nên ONVIF
(/onvif/device_service) báo "không phải giao thức này" và chế độ AUTO chuyển sang PSIA.
--onvif-port thì mock không phục vụ ONVIF — dùng onvif_mock.py cho camera hai giao thức.

Phủ những gì PsiaControl gọi:
    GET  /PSIA/System/deviceInfo
    GET  /PSIA/System/time,  PUT /PSIA/System/time
    GET  /PSIA/Streaming/channels
    GET  /PSIA/Streaming/channels/<id>,  PUT (ghi encoder, giữ nguyên node lạ)
    GET  /PSIA/Streaming/channels/<id>/capabilities
    GET  /PSIA/PTZ/channels,  GET /PSIA/PTZ/channels/<ch>/capabilities
    PUT  /PSIA/PTZ/channels/<ch>/continuous|relative|absolute
    GET  /PSIA/PTZ/channels/<ch>/status
    GET  /PSIA/PTZ/channels/<ch>/presets,  PUT|DELETE .../presets/<id>,  PUT .../presets/<id>/goto
    PUT  /PSIA/PTZ/channels/<ch>/homeposition/goto
    PUT  /PSIA/System/Video/inputs/channels/<ch>/focus|iris
    GET  /PSIA/IO/outputs,  PUT /PSIA/IO/outputs/<id>/trigger

--dialect isapi: camera Hikvision ISAPI thay cho PSIA. Phục vụ cùng các tài nguyên dưới /ISAPI
(PTZ ở /ISAPI/PTZCtrl, relay ở /ISAPI/System/IO/outputs), namespace Hikvision, DeviceInfo không
có manufacturer, StreamingChannel không có rtspPortNo (cổng RTSP đọc ở /ISAPI/Security/adminAccesses).
Thêm two-way audio:
    GET  /ISAPI/System/TwoWayAudio/channels
    PUT  /ISAPI/System/TwoWayAudio/channels/<id>/open|close
    PUT  /ISAPI/System/TwoWayAudio/channels/<id>/audioData  (byte ghi nối vào --audio-out;
         Content-Length: 0 thì trả 200 ngay rồi đọc audio thô tới khi client đóng kết nối)
Đường /PSIA trả 404 ở dialect isapi và ngược lại.

--onvif-401: /onvif/device_service trả 401 + Digest challenge thay cho 404, giống Hikvision tắt
ONVIF hoặc chưa tạo user ONVIF (ONVIF báo AUTH_FAILED lúc dò AUTO).

Dùng:
    psia_mock.py [--port 18680] [--user admin --password admin123] [--basic]
                 [--dialect psia|isapi] [--onvif-401] [--audio-out PATH]
                 [--rtsp-port 554] [--no-ptz] [--state PATH] [--verbose]

--state ghi JSON (mỗi lần có request): cấu hình encoder hiện tại, vị trí PTZ, trạng thái
relay, giờ đã set và danh sách request (method, path) để script kiểm tra đọc lại.
"""
import argparse
import hashlib
import json
import os
import re
import threading
import time
import xml.etree.ElementTree as ET
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

NS = "urn:psialliance-org"
NS_ISAPI = "http://www.hikvision.com/ver20/XMLSchema"
REALM = "psia-mock"


def to_psia_path(path, dialect):
    """Đường ISAPI → đường PSIA tương ứng để dùng chung bảng route; None nếu sai dialect."""
    if dialect == "psia":
        return path if path.startswith("/PSIA/") else None
    if not path.startswith("/ISAPI/"):
        return None
    rest = path[len("/ISAPI"):]
    if rest.startswith("/PTZCtrl/"):
        rest = "/PTZ/" + rest[len("/PTZCtrl/"):]
    elif rest.startswith("/System/IO/"):
        rest = "/IO/" + rest[len("/System/IO/"):]
    return "/PSIA" + rest


def md5(s):
    return hashlib.md5(s.encode()).hexdigest()


def local(tag):
    return tag.split("}", 1)[-1]


def find(node, name):
    for child in node.iter():
        if local(child.tag) == name:
            return child
    return None


def text(node, name, default=""):
    n = find(node, name)
    return n.text.strip() if n is not None and n.text else default


def status(code=1, string="OK", sub="ok"):
    return ('<?xml version="1.0" encoding="UTF-8"?>'
            '<ResponseStatus version="1.0" xmlns="%s"><requestURL></requestURL>'
            '<statusCode>%d</statusCode><statusString>%s</statusString><subStatusCode>%s</subStatusCode>'
            '</ResponseStatus>' % (NS, code, string, sub))


def use_dialect(dialect):
    global NS
    if dialect == "isapi":
        NS = NS_ISAPI


class Camera:
    def __init__(self, args):
        self.args = args
        self.lock = threading.Lock()
        self.channels = {
            "101": {"codec": "H.264", "width": 1920, "height": 1080, "bitrate": 4096, "fps": 2500},
            "102": {"codec": "H.264", "width": 640, "height": 360, "bitrate": 512, "fps": 1500},
        }
        self.ptz = {"azimuth": 1800, "elevation": 0, "zoom": 10}
        self.presets = {"1": "Gate", "2": "Parking"}
        self.outputs = {"1": "low"}
        self.time_set = []
        self.requests = []
        self.nonces = set()
        self.isapi = args.dialect == "isapi"
        self.two_way_open = False
        self.audio_bytes = 0

    def dump(self):
        if not self.args.state:
            return
        data = {
            "channels": self.channels,
            "ptz": self.ptz,
            "presets": self.presets,
            "outputs": self.outputs,
            "time_set": self.time_set,
            "two_way_open": self.two_way_open,
            "audio_bytes": self.audio_bytes,
            "requests": self.requests,
        }
        tmp = self.args.state + ".tmp"
        with open(tmp, "w") as f:
            json.dump(data, f, indent=1)
        os.replace(tmp, self.args.state)

    # ── XML documents ────────────────────────────────────────────────────────────────────────
    def device_info(self):
        if self.isapi:
            # Hikvision: deviceName là tên người dùng đặt, không có manufacturer.
            return ('<?xml version="1.0" encoding="UTF-8"?>'
                    '<DeviceInfo version="2.0" xmlns="%s">'
                    '<deviceName>IP CAMERA</deviceName><deviceID>88a1c2d3-0000-0000-0000-000000000002</deviceID>'
                    '<model>DS-2DE4425IW-DE</model><serialNumber>DS-2DE4425IW-DE20261004AAWRISAPIMOCK</serialNumber>'
                    '<macAddress>02:00:00:00:00:02</macAddress><firmwareVersion>V5.7.3</firmwareVersion>'
                    '<firmwareReleasedDate>build 221122</firmwareReleasedDate><deviceType>IPCamera</deviceType>'
                    '</DeviceInfo>' % NS)
        return ('<?xml version="1.0" encoding="UTF-8"?>'
                '<DeviceInfo version="1.0" xmlns="%s">'
                '<deviceName>PSIA Mock</deviceName><deviceID>88a1c2d3-0000-0000-0000-000000000001</deviceID>'
                '<model>PSIA-MOCK-1</model><serialNumber>PSIAMOCK0001</serialNumber>'
                '<macAddress>02:00:00:00:00:01</macAddress><firmwareVersion>V1.0.0</firmwareVersion>'
                '<firmwareReleasedDate>2026-10-03</firmwareReleasedDate><manufacturer>MockVendor</manufacturer>'
                '</DeviceInfo>' % NS)

    def channel_xml(self, cid, caps=False):
        c = self.channels[cid]
        def opt(attr):
            return (" " + attr) if caps else ""
        rtsp = "" if self.isapi else "<rtspPortNo>%d</rtspPortNo>" % self.args.rtsp_port
        return ('<StreamingChannel version="1.0" xmlns="%s">'
                '<id>%s</id><channelName>Camera %s</channelName><enabled>true</enabled>'
                '<Transport>%s<maxPacketSize>1446</maxPacketSize>'
                '<ControlProtocolList><ControlProtocol><streamingTransport>RTSP</streamingTransport></ControlProtocol>'
                '</ControlProtocolList></Transport>'
                '<Video><enabled>true</enabled><videoInputChannelID>1</videoInputChannelID>'
                '<videoCodecType%s>%s</videoCodecType><videoScanType>progressive</videoScanType>'
                '<videoResolutionWidth%s>%d</videoResolutionWidth>'
                '<videoResolutionHeight%s>%d</videoResolutionHeight>'
                '<videoQualityControlType>CBR</videoQualityControlType>'
                '<constantBitRate%s>%d</constantBitRate><fixedQuality>60</fixedQuality>'
                '<maxFrameRate%s>%d</maxFrameRate><keyFrameInterval>2000</keyFrameInterval>'
                '<vendorExtension>keep-me</vendorExtension></Video>'
                '<Audio><enabled>false</enabled><audioCompressionType>G.711ulaw</audioCompressionType></Audio>'
                '</StreamingChannel>'
                % (NS, cid, cid, rtsp,
                   opt('opt="H.264,H.265,MJPEG"'), c["codec"],
                   opt('opt="1920,1280,640"'), c["width"],
                   opt('opt="1080,720,360"'), c["height"],
                   opt('min="32" max="16384"'), c["bitrate"],
                   opt('opt="2500,2000,1500,1000,500"'), c["fps"]))

    def channel_list(self):
        return ('<?xml version="1.0" encoding="UTF-8"?><StreamingChannelList version="1.0" xmlns="%s">%s'
                '</StreamingChannelList>' % (NS, "".join(self.channel_xml(c) for c in sorted(self.channels))))

    def ptz_caps(self):
        return ('<?xml version="1.0" encoding="UTF-8"?><PTZChanelCap version="1.0" xmlns="%s">'
                '<AbsolutePanTiltPositionSpace><XRange><Min>0</Min><Max>3600</Max></XRange>'
                '<YRange><Min>-900</Min><Max>2700</Max></YRange></AbsolutePanTiltPositionSpace>'
                '<AbsoluteZoomPositionSpace><ZRange><Min>10</Min><Max>40</Max></ZRange></AbsoluteZoomPositionSpace>'
                '<RelativePanTiltSpace><XRange><Min>-100</Min><Max>100</Max></XRange>'
                '<YRange><Min>-100</Min><Max>100</Max></YRange></RelativePanTiltSpace>'
                '<RelativeZoomSpace><ZRange><Min>-100</Min><Max>100</Max></ZRange></RelativeZoomSpace>'
                '<ContinuousPanTiltSpace><XRange><Min>-100</Min><Max>100</Max></XRange>'
                '<YRange><Min>-100</Min><Max>100</Max></YRange></ContinuousPanTiltSpace>'
                '<ContinuousZoomSpace><ZRange><Min>-100</Min><Max>100</Max></ZRange></ContinuousZoomSpace>'
                '<maxPresetNum>256</maxPresetNum></PTZChanelCap>' % NS)

    # ── request handling ─────────────────────────────────────────────────────────────────────
    def handle(self, method, path, body):
        """Returns (http_code, xml)."""
        path = path.split("?", 1)[0].rstrip("/")
        with self.lock:
            self.requests.append({"method": method, "path": path, "t": time.time()})
            try:
                return self._route(method, path, body)
            finally:
                self.dump()

    def _route(self, method, path, body):
        if self.isapi:
            special = self._isapi_only(method, path, body)
            if special is not None:
                return special
        path = to_psia_path(path, self.args.dialect)
        if path is None:
            return 404, ""
        if path == "/PSIA/System/deviceInfo" and method == "GET":
            return 200, self.device_info()
        if path == "/PSIA/System/time":
            if method == "GET":
                now = time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(time.time() - self.args.clock_skew))
                return 200, ('<?xml version="1.0" encoding="UTF-8"?><Time version="1.0" xmlns="%s">'
                             '<timeMode>NTP</timeMode><localTime>%s</localTime><timeZone>CST+0:00:00</timeZone>'
                             '</Time>' % (NS, now))
            if method == "PUT":
                doc = ET.fromstring(body)
                self.time_set.append({"timeMode": text(doc, "timeMode"), "localTime": text(doc, "localTime"),
                                      "timeZone": text(doc, "timeZone")})
                self.args.clock_skew = 0
                return 200, status()
        if path == "/PSIA/Streaming/channels" and method == "GET":
            return 200, self.channel_list()
        m = re.match(r"^/PSIA/Streaming/channels/(\d+)(/capabilities)?$", path)
        if m:
            cid = m.group(1)
            if cid not in self.channels:
                return 404, status(4, "Invalid Operation", "invalidID")
            if m.group(2):
                return (200, '<?xml version="1.0" encoding="UTF-8"?>' + self.channel_xml(cid, True)) \
                    if method == "GET" else (405, "")
            if method == "GET":
                return 200, '<?xml version="1.0" encoding="UTF-8"?>' + self.channel_xml(cid)
            if method == "PUT":
                doc = ET.fromstring(body)
                if text(doc, "vendorExtension") != "keep-me":
                    return 400, status(6, "Invalid XML Content", "badXmlContent")
                c = self.channels[cid]
                c["codec"] = text(doc, "videoCodecType", c["codec"])
                c["width"] = int(text(doc, "videoResolutionWidth", c["width"]))
                c["height"] = int(text(doc, "videoResolutionHeight", c["height"]))
                c["bitrate"] = int(text(doc, "constantBitRate", c["bitrate"]))
                c["fps"] = int(text(doc, "maxFrameRate", c["fps"]))
                return 200, status()
        if path.startswith("/PSIA/PTZ/") and self.args.no_ptz:
            return 404, ""
        if path == "/PSIA/PTZ/channels" and method == "GET":
            return 200, ('<?xml version="1.0" encoding="UTF-8"?><PTZChannelList version="1.0" xmlns="%s">'
                         '<PTZChannel><id>1</id><enabled>true</enabled><videoInputID>1</videoInputID></PTZChannel>'
                         '</PTZChannelList>' % NS)
        m = re.match(r"^/PSIA/PTZ/channels/1/(.+)$", path)
        if m:
            return self._ptz(method, m.group(1), body)
        m = re.match(r"^/PSIA/System/Video/inputs/channels/1/(focus|iris)$", path)
        if m and method == "PUT":
            doc = ET.fromstring(body)
            int(text(doc, m.group(1), "0"))
            return 200, status()
        if path == "/PSIA/IO/outputs" and method == "GET":
            return 200, ('<?xml version="1.0" encoding="UTF-8"?><IOOutputPortList version="1.0" xmlns="%s">%s'
                         '</IOOutputPortList>' % (NS, "".join(
                             '<IOOutputPort><id>%s</id><name>Alarm out %s</name>'
                             '<PowerOnState><defaultState>low</defaultState><outputState>high</outputState>'
                             '</PowerOnState></IOOutputPort>' % (i, i) for i in sorted(self.outputs))))
        m = re.match(r"^/PSIA/IO/outputs/(\w+)/trigger$", path)
        if m and method == "PUT":
            if m.group(1) not in self.outputs:
                return 404, status(4, "Invalid Operation", "invalidID")
            self.outputs[m.group(1)] = text(ET.fromstring(body), "outputState")
            return 200, status()
        return 404, status(4, "Invalid Operation", "notSupport")

    def _isapi_only(self, method, path, body):
        """Tài nguyên chỉ ISAPI có; None nếu không phải."""
        if path == "/ISAPI/Security/adminAccesses" and method == "GET":
            return 200, ('<?xml version="1.0" encoding="UTF-8"?><AdminAccessProtocolList version="2.0" xmlns="%s">'
                         '<AdminAccessProtocol><id>1</id><enabled>true</enabled><protocol>HTTP</protocol>'
                         '<portNo>%d</portNo></AdminAccessProtocol>'
                         '<AdminAccessProtocol><id>2</id><enabled>true</enabled><protocol>HTTPS</protocol>'
                         '<portNo>443</portNo></AdminAccessProtocol>'
                         '<AdminAccessProtocol><id>4</id><enabled>true</enabled><protocol>RTSP</protocol>'
                         '<portNo>%d</portNo></AdminAccessProtocol>'
                         '</AdminAccessProtocolList>' % (NS, self.args.port, self.args.rtsp_port))
        if path == "/ISAPI/System/TwoWayAudio/channels" and method == "GET":
            return 200, ('<?xml version="1.0" encoding="UTF-8"?><TwoWayAudioChannelList version="2.0" xmlns="%s">'
                         '<TwoWayAudioChannel><id>1</id><enabled>%s</enabled>'
                         '<audioCompressionType>G.711ulaw</audioCompressionType>'
                         '<speakerVolume>50</speakerVolume><noisereduce>false</noisereduce>'
                         '</TwoWayAudioChannel></TwoWayAudioChannelList>'
                         % (NS, "true" if self.two_way_open else "false"))
        m = re.match(r"^/ISAPI/System/TwoWayAudio/channels/(\w+)/(open|close|audioData)$", path)
        if m and method == "PUT":
            if m.group(1) != "1":
                return 404, status(4, "Invalid Operation", "invalidID")
            if m.group(2) == "open":
                if self.two_way_open:
                    return 403, status(4, "Invalid Operation", "deviceBusy")
                self.two_way_open = True
                return 200, ('<?xml version="1.0" encoding="UTF-8"?><TwoWayAudioSession version="2.0" xmlns="%s">'
                             '<sessionId>1</sessionId></TwoWayAudioSession>' % NS)
            if m.group(2) == "close":
                self.two_way_open = False
                return 200, status()
            if not self.two_way_open:
                return 403, status(4, "Invalid Operation", "notOpen")
            return 200, status()
        return None

    def _ptz(self, method, sub, body):
        if sub == "capabilities" and method == "GET":
            return 200, self.ptz_caps()
        if sub == "status" and method == "GET":
            return 200, ('<?xml version="1.0" encoding="UTF-8"?><PTZStatus version="1.0" xmlns="%s"><AbsoluteHigh>'
                         '<elevation>%d</elevation><azimuth>%d</azimuth><absoluteZoom>%d</absoluteZoom>'
                         '</AbsoluteHigh></PTZStatus>' % (NS, self.ptz["elevation"], self.ptz["azimuth"],
                                                          self.ptz["zoom"]))
        if method == "PUT" and sub in ("continuous", "momentary"):
            doc = ET.fromstring(body)
            self.ptz["last_continuous"] = [int(text(doc, "pan", "0")), int(text(doc, "tilt", "0")),
                                           int(text(doc, "zoom", "0"))]
            return 200, status()
        if method == "PUT" and sub == "relative":
            doc = ET.fromstring(body)
            self.ptz["azimuth"] = (self.ptz["azimuth"] + int(text(doc, "positionX", "0"))) % 3600
            self.ptz["elevation"] += int(text(doc, "positionY", "0"))
            self.ptz["last_relative"] = [int(text(doc, "positionX", "0")), int(text(doc, "positionY", "0")),
                                         int(text(doc, "relativeZoom", "0"))]
            return 200, status()
        if method == "PUT" and sub == "absolute":
            doc = ET.fromstring(body)
            self.ptz["elevation"] = int(text(doc, "elevation", "0"))
            self.ptz["azimuth"] = int(text(doc, "azimuth", "0"))
            self.ptz["zoom"] = int(text(doc, "absoluteZoom", "10"))
            return 200, status()
        if sub == "presets" and method == "GET":
            return 200, ('<?xml version="1.0" encoding="UTF-8"?><PTZPresetList version="1.0" xmlns="%s">%s'
                         '</PTZPresetList>' % (NS, "".join(
                             '<PTZPreset><id>%s</id><presetName>%s</presetName><enabled>true</enabled></PTZPreset>'
                             % (k, v) for k, v in sorted(self.presets.items()))))
        m = re.match(r"^presets/(\d+)(/goto)?$", sub)
        if m and method == "PUT":
            if m.group(2):
                if m.group(1) not in self.presets:
                    return 404, status(4, "Invalid Operation", "invalidID")
                self.ptz["last_preset"] = m.group(1)
                return 200, status()
            self.presets[m.group(1)] = text(ET.fromstring(body), "presetName", "Preset " + m.group(1))
            return 200, status()
        if m and method == "DELETE":
            self.presets.pop(m.group(1), None)
            return 200, status()
        if sub == "homeposition/goto" and method == "PUT":
            self.ptz["last_home"] = time.time()
            return 200, status()
        return 404, status(4, "Invalid Operation", "notSupport")


def make_handler(cam, args):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *a):
            if args.verbose:
                super().log_message(fmt, *a)

        def _authorized(self):
            auth = self.headers.get("Authorization", "")
            if args.basic:
                import base64
                want = "Basic " + base64.b64encode(("%s:%s" % (args.user, args.password)).encode()).decode()
                return auth == want
            if not auth.startswith("Digest "):
                return False
            fields = dict(re.findall(r'(\w+)="?([^",]*)"?', auth[7:]))
            if fields.get("username") != args.user or fields.get("nonce") not in cam.nonces:
                return False
            ha1 = md5("%s:%s:%s" % (args.user, REALM, args.password))
            ha2 = md5("%s:%s" % (self.command, fields.get("uri", "")))
            if fields.get("qop"):
                expect = md5("%s:%s:%s:%s:%s:%s" % (ha1, fields["nonce"], fields.get("nc", ""),
                                                    fields.get("cnonce", ""), fields["qop"], ha2))
            else:
                expect = md5("%s:%s:%s" % (ha1, fields["nonce"], ha2))
            return fields.get("response") == expect

        def _send(self, code, body, extra=None):
            data = body.encode()
            self.send_response(code)
            if data:
                self.send_header("Content-Type", "application/xml; charset=UTF-8")
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def _challenge(self):
            nonce = md5("%f%d" % (time.time(), id(self)))
            cam.nonces.add(nonce)
            challenge = ('Basic realm="%s"' % REALM) if args.basic else \
                ('Digest realm="%s", qop="auth", nonce="%s", opaque="0123"' % (REALM, nonce))
            return self._send(401, status(4, "Invalid Operation", "unAuthorized"),
                              {"WWW-Authenticate": challenge})

        def _audio_data(self, raw=False):
            # Dữ liệu G.711 gửi liên tục trên một kết nối; không có Content-Length (hoặc raw) thì đọc
            # tới khi đóng.
            length = None if raw else self.headers.get("Content-Length")
            out = open(args.audio_out, "ab") if args.audio_out else None
            total = 0
            try:
                remaining = int(length) if length is not None else None
                while remaining is None or remaining > 0:
                    chunk = self.rfile.read1(4096) if remaining is None else self.rfile.read(min(4096, remaining))
                    if not chunk:
                        break
                    total += len(chunk)
                    if remaining is not None:
                        remaining -= len(chunk)
                    if out:
                        out.write(chunk)
                    with cam.lock:
                        cam.audio_bytes += len(chunk)
            finally:
                if out:
                    out.close()
                with cam.lock:
                    cam.dump()
            return total

        def _serve(self):
            if self.path.startswith("/onvif/") and args.onvif_401:
                self.rfile.read(int(self.headers.get("Content-Length") or 0))
                return self._challenge()
            if to_psia_path(self.path.split("?", 1)[0], args.dialect) is None:
                self.rfile.read(int(self.headers.get("Content-Length") or 0))
                return self._send(404, "")
            streaming = cam.isapi and self.command == "PUT" and self.path.endswith("/audioData")
            if streaming and self._authorized():
                with cam.lock:
                    is_open = cam.two_way_open
                if not is_open:
                    self.rfile.read(int(self.headers.get("Content-Length") or 0))
                    return self._send(403, status(4, "Invalid Operation", "notOpen"))
                if self.headers.get("Content-Length") == "0":
                    # Kiểu Hikvision: header kết thúc request, thiết bị trả 200 ngay rồi đọc audio thô
                    # trên cùng kết nối tới khi client đóng.
                    code, xml = cam.handle(self.command, self.path, "")
                    self._send(code, xml)
                    self.wfile.flush()
                    self._audio_data(raw=True)
                    self.close_connection = True
                    return
                self._audio_data()
                code, xml = cam.handle(self.command, self.path, "")
                return self._send(code, xml)
            length = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(length).decode("utf-8", "replace") if length else ""
            if not self._authorized():
                return self._challenge()
            try:
                code, xml = cam.handle(self.command, self.path, body)
            except ET.ParseError:
                code, xml = 400, status(5, "Invalid XML Format", "badXmlFormat")
            self._send(code, xml)

        do_GET = _serve
        do_PUT = _serve
        do_POST = _serve
        do_DELETE = _serve

    return Handler


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=18680)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", default="admin123")
    ap.add_argument("--basic", action="store_true", help="Basic thay cho Digest")
    ap.add_argument("--dialect", choices=["psia", "isapi"], default="psia")
    ap.add_argument("--onvif-401", action="store_true", help="ONVIF trả 401 thay cho 404 (Hikvision tắt ONVIF)")
    ap.add_argument("--audio-out", default=None, help="file nhận byte two-way audio (dialect isapi)")
    ap.add_argument("--rtsp-port", type=int, default=554)
    ap.add_argument("--no-ptz", action="store_true")
    ap.add_argument("--clock-skew", type=int, default=0, help="giây camera chậm hơn đồng hồ thật")
    ap.add_argument("--state", default=None)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    use_dialect(args.dialect)
    cam = Camera(args)
    server = ThreadingHTTPServer((args.host, args.port), make_handler(cam, args))
    print("psia_mock listening on %s:%d (%s, %s)" % (args.host, args.port, args.dialect,
                                                     "Basic" if args.basic else "Digest"), flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
