// =============================================================================
// s3pro-player.js — S3Pro FMP4 live-stream player (UMD)
//
// Script-tag usage:
//   <script src="s3pro-player.js"></script>
//   <script>
//     const player = new S3ProPlayer({
//       videojs:      window.videojs,
//       videoElId:    'vjs-player',
//       onLog:        (level, msg) => console.log(level, msg),
//       onFatalError: (msg) => showOverlay(msg),
//       onTimeUpdate: (ct, fmt, codec, bufLen) => { ... },
//     });
//     player.on('statechange', s => console.log('state:', s));
//     player.play('http://host/stream.live.mp4');
//   </script>
//
// Module usage:
//   import S3ProPlayer from './s3pro-player.js';
//   const player = new S3ProPlayer({ videojs, videoElId: 'vjs-player' });
// =============================================================================

(function (root, factory) {
    if (typeof define === 'function' && define.amd) {
        define([], factory);
    } else if (typeof module === 'object' && module.exports) {
        module.exports = factory();
    } else {
        root.S3ProPlayer = factory();
    }
}(typeof self !== 'undefined' ? self : this, function () {
    'use strict';

    // =========================================================================
    // Module-level registries
    //   _playerRegistry  : vjsPlayer instance  -> S3ProPlayer instance
    //   _registeredTechs : Html5Tech constructor -> registered (avoid double-reg)
    // =========================================================================
    const _playerRegistry  = typeof WeakMap !== 'undefined' ? new WeakMap() : null;
    const _registeredTechs = typeof WeakSet !== 'undefined' ? new WeakSet() : null;

    // =========================================================================
    // Private helpers
    // =========================================================================
    function concat(a, b) {
        const out = new Uint8Array(a.byteLength + b.byteLength);
        out.set(a, 0);
        out.set(b, a.byteLength);
        return out;
    }

    function fmtTime(sec) {
        if (!isFinite(sec) || sec < 0) return '00:00:00';
        const s  = Math.floor(sec);
        const h  = Math.floor(s / 3600);
        const m  = Math.floor((s % 3600) / 60);
        const ss = s % 60;
        return String(h).padStart(2, '0') + ':' +
               String(m).padStart(2, '0') + ':' +
               String(ss).padStart(2, '0');
    }

    // readTfdt -- extract baseMediaDecodeTime from moof box
    function readTfdt(data) {
        const v = new DataView(data.buffer || data, data.byteOffset || 0, data.byteLength);
        function scan(pos, end) {
            while (pos + 8 <= end) {
                const size = v.getUint32(pos);
                if (size < 8 || pos + size > end) break;
                const type = String.fromCharCode(
                    v.getUint8(pos + 4), v.getUint8(pos + 5),
                    v.getUint8(pos + 6), v.getUint8(pos + 7));
                if (type === 'moof' || type === 'traf') {
                    const r = scan(pos + 8, pos + size);
                    if (r !== null) return r;
                } else if (type === 'tfdt') {
                    const version = v.getUint8(pos + 8);
                    if (version === 1) {
                        const hi = v.getUint32(pos + 12);
                        const lo = v.getUint32(pos + 16);
                        return (hi * 4294967296 + lo) / 90;
                    } else {
                        return v.getUint32(pos + 12) / 90;
                    }
                }
                pos += size;
            }
            return null;
        }
        return scan(0, data.byteLength);
    }

    // parseMimeFromInitSegment -- detect video + audio codec from moov box
    function parseMimeFromInitSegment(buf) {
        const bytes = new Uint8Array(buf);
        const dv    = new DataView(
            buf instanceof ArrayBuffer ? buf : buf.buffer,
            buf.byteOffset || 0,
            buf.byteLength || buf.byteLength);
        const str4  = off => String.fromCharCode(
            bytes[off], bytes[off + 1], bytes[off + 2], bytes[off + 3]);
        let videoPart = '', audioPart = '';

        function walk(start, end) {
            let pos = start;
            while (pos + 8 <= end) {
                let size = dv.getUint32(pos);
                if (size < 8 || pos + size > end) break;
                const type = str4(pos + 4);

                if (['moov', 'trak', 'mdia', 'minf', 'stbl'].includes(type)) {
                    walk(pos + 8, pos + size);
                } else if (type === 'stsd') {
                    walk(pos + 16, pos + size);
                } else if (['avc1', 'avc3'].includes(type)) {
                    // AVC: read profile/compat/level from avcC box
                    // Layout: box header(8) + SampleEntry(8) + VisualSampleEntry(70) = 86
                    let inner = pos + 86;
                    while (inner + 8 <= pos + size) {
                        const isize = dv.getUint32(inner);
                        const itype = str4(inner + 4);
                        if (isize >= 11 && itype === 'avcC') {
                            const profile = bytes[inner + 9].toString(16).padStart(2, '0');
                            const compat  = bytes[inner + 10].toString(16).padStart(2, '0');
                            const level   = bytes[inner + 11].toString(16).padStart(2, '0');
                            videoPart = type + '.' + profile + compat + level;
                            break;
                        }
                        if (isize < 8) break;
                        inner += isize;
                    }
                    if (!videoPart) videoPart = type;
                } else if (['hev1', 'hvc1'].includes(type)) {
                    // HEVC: read full codec string from hvcC box
                    // Layout: box header(8) + SampleEntry(8) + VisualSampleEntry(70) = 86
                    let inner = pos + 86;
                    while (inner + 8 <= pos + size) {
                        const isize = dv.getUint32(inner);
                        const itype = str4(inner + 4);
                        if (isize >= 23 && itype === 'hvcC') {
                            const base          = inner + 8;
                            const genProfSpace  = bytes[base + 1];
                            const profileSpace  = (genProfSpace >> 6) & 0x3;
                            const tierFlag      = (genProfSpace >> 5) & 0x1;
                            const profileIdc    = genProfSpace & 0x1f;
                            const profileCompat = dv.getUint32(base + 2);
                            const constraintHi  = bytes[base + 6];
                            const levelIdc      = bytes[base + 12];
                            const spaceStr      = ['', 'A', 'B', 'C'][profileSpace] || '';
                            const tierStr       = tierFlag ? 'H' : 'L';
                            const compatHex     = profileCompat.toString(16).toUpperCase().replace(/0+$/, '') || '0';
                            const constraintStr = constraintHi ? '.' + constraintHi.toString(16).toUpperCase() : '';
                            videoPart = `${type}.${spaceStr}${profileIdc}.${compatHex}.${tierStr}${levelIdc}${constraintStr}`;
                            break;
                        }
                        if (isize < 8) break;
                        inner += isize;
                    }
                    if (!videoPart) videoPart = type;
                } else if (type === 'mp4a') {
                    // AAC: read Audio Object Type from esds box
                    let inner = pos + 8 + 28;
                    while (inner + 8 <= pos + size) {
                        const isize = dv.getUint32(inner);
                        const itype = str4(inner + 4);
                        if (isize >= 13 && itype === 'esds') {
                            let p     = inner + 12; // skip version+flags
                            const end2    = inner + isize;
                            const skipLen = () => { while (p < end2 && (bytes[p++] & 0x80)); };
                            if (p < end2 && bytes[p++] === 0x03) { // ES_Descriptor
                                skipLen();
                                p += 2; // ES_ID
                                const flags = bytes[p++];
                                if (flags & 0x80) p += 2;
                                if (flags & 0x40) p += bytes[p] + 1;
                                if (flags & 0x20) p += 2;
                                if (p < end2 && bytes[p++] === 0x04) { // DecoderConfigDescriptor
                                    skipLen();
                                    p += 13;
                                    if (p < end2 && bytes[p++] === 0x05) { // DecoderSpecificInfo
                                        skipLen();
                                        if (p + 1 < end2) {
                                            const aot = (bytes[p] >> 3) & 0x1f;
                                            audioPart = 'mp4a.40.' + aot;
                                        }
                                    }
                                }
                            }
                            break;
                        }
                        if (isize < 8) break;
                        inner += isize;
                    }
                    if (!audioPart) audioPart = 'mp4a.40.2';
                } else if (type === 'Opus') {
                    audioPart = 'opus';
                }
                pos += size;
            }
        }

        walk(0, bytes.byteLength);
        if (!videoPart) return '';
        const codecs = audioPart ? `${videoPart},${audioPart}` : videoPart;
        return `video/mp4; codecs="${codecs}"`;
    }

    // Default error message strings (English). Override via opts.messages.
    const DEFAULT_MESSAGES = {
        noMSE:            'Browser does not support MediaSource Extensions (MSE).',
        hevcUnsupported:  'Browser does not support H.265/HEVC decoding. ' +
                          'Use Chrome/Edge on Windows or macOS with hardware decoding, ' +
                          'or request an H.264 camera stream.',
        av1Unsupported:   'Browser does not support AV1 codec: ',
        vp9Unsupported:   'Browser does not support VP9 codec: ',
        vp8Unsupported:   'Browser does not support VP8 codec: ',
        codecUnsupported: 'Codec not supported: ',
        connectionFailed: 'Connection failed: ',
        readError:        'Stream read error: ',
    };

    // checkCodecSupport -- returns null if OK, or an error string if not supported.
    // messages: optional object with keys matching DEFAULT_MESSAGES to override.
    function checkCodecSupport(mime, messages) {
        const msg = messages ? Object.assign({}, DEFAULT_MESSAGES, messages) : DEFAULT_MESSAGES;
        if (!mime) return null;
        if (typeof MediaSource === 'undefined') return msg.noMSE;
        if (MediaSource.isTypeSupported(mime)) return null;
        const codecs = (mime.match(/codecs="([^"]+)"/) || [])[1] || mime;
        if (/hvc1|hev1/i.test(codecs)) return msg.hevcUnsupported;
        if (/av01/i.test(codecs))      return msg.av1Unsupported  + codecs;
        if (/vp09|vp9/i.test(codecs))  return msg.vp9Unsupported  + codecs;
        if (/vp8/i.test(codecs))       return msg.vp8Unsupported  + codecs;
        return msg.codecUnsupported + codecs;
    }

    // =========================================================================
    // Constants
    // =========================================================================
    const STAGE_SEGMENTS   = 2;
    const STAGE_TIMEOUT_MS = 2000;

    // =========================================================================
    // S3ProLiveSourceHandler -- MSE-based streaming source handler for Video.js
    //
    // cb = { info(msg), warn(msg), error(msg), showError(msg), messages }
    //   info/warn/error : diagnostic log callbacks
    //   showError       : fatal error callback (stops playback, surface to user)
    //   messages        : optional message overrides (passed to checkCodecSupport)
    // =========================================================================
    class S3ProLiveSourceHandler {
        constructor(source, tech, options, cb) {
            this._cb      = cb;
            this._tech    = tech;
            this._videoEl = tech.el();
            this._url     = source.src;

            this._running     = false;
            this._fetchCtrl   = null;
            this._ms          = null;
            this._sb          = null;
            this._objUrl      = null;
            this._pendingQ    = [];
            this._moofBuf     = null;
            this._ftypBuf     = null;
            this._currentMime = '';
            this._onUpdateEnd = null;

            this._stallSince           = 0;
            this._stagingCodec         = null;
            this._stagingTimer         = null;
            this._seekAfterCodecChange = false;
            this._afterCodecChangeTick = 0;
            this._httpEnded            = false;

            this._connect(this._url);
        }

        dispose() { this._stop(); }

        async _connect(url) {
            this._running = true;
            this._cb.info('[vjs] Connecting: ' + url);

            let response;
            try {
                this._fetchCtrl = new AbortController();
                response = await fetch(url, { signal: this._fetchCtrl.signal });
            } catch (e) {
                if (e.name !== 'AbortError') {
                    this._cb.error('[vjs] fetch: ' + e.message);
                    this._cb.showError(this._msg('connectionFailed') + e.message);
                }
                return;
            }
            if (!response.ok) {
                const msg = 'HTTP ' + response.status + ' ' + response.statusText;
                this._cb.error('[vjs] ' + msg);
                this._cb.showError(msg);
                return;
            }
            this._cb.info('[vjs] Connected -- reading stream');

            const reader = response.body.getReader();
            let partial  = new Uint8Array(0);

            try {
                while (this._running) {
                    const { value, done } = await reader.read();
                    if (done) {
                        this._cb.info('[vjs] Stream ended by server -- ' + this._pendingQ.length + ' seg(s) queued');
                        this._httpEnded = true;
                        this._maybeEndStream();
                        break;
                    }

                    const chunk  = partial.byteLength > 0 ? concat(partial, value) : value;
                    partial      = new Uint8Array(0);
                    let offset   = 0;

                    while (offset < chunk.byteLength) {
                        if (chunk.byteLength - offset < 8) {
                            partial = chunk.slice(offset); break;
                        }
                        const dv = new DataView(chunk.buffer,
                            chunk.byteOffset + offset, chunk.byteLength - offset);
                        let boxSize = dv.getUint32(0);
                        if (boxSize === 1) {
                            if (chunk.byteLength - offset < 16) {
                                partial = chunk.slice(offset); break;
                            }
                            boxSize = dv.getUint32(12);
                        }
                        if (boxSize < 8) { partial = new Uint8Array(0); break; }
                        if (chunk.byteLength - offset < boxSize) {
                            partial = chunk.slice(offset); break;
                        }
                        const boxType = String.fromCharCode(
                            chunk[offset + 4], chunk[offset + 5],
                            chunk[offset + 6], chunk[offset + 7]);
                        const boxBuf = chunk.buffer.slice(
                            chunk.byteOffset + offset,
                            chunk.byteOffset + offset + boxSize);
                        this._processBox(boxBuf, boxType);
                        offset += boxSize;
                    }
                }
            } catch (e) {
                if (e.name !== 'AbortError') {
                    this._cb.error('[vjs] Stream read error: ' + e.message);
                    this._cb.showError(this._msg('readError') + e.message);
                }
            } finally {
                reader.releaseLock();
            }
        }

        // Resolve a message key against cb.messages override or DEFAULT_MESSAGES
        _msg(key) {
            const m = this._cb.messages;
            return (m && m[key] != null) ? m[key] : DEFAULT_MESSAGES[key];
        }

        // Called whenever the queue empties or the HTTP stream ends.
        // Signals MSE that no more data is coming so the browser can
        // properly schedule and render the remaining buffered frames.
        _maybeEndStream() {
            if (!this._httpEnded) return;
            if (!this._ms || this._ms.readyState !== 'open') return;
            if (this._pendingQ.length > 0) return;
            if (this._sb && this._sb.updating) return;
            try {
                this._cb.info('[vjs] MediaSource.endOfStream()');
                this._ms.endOfStream();
            } catch (e) {
                this._cb.warn('[vjs] endOfStream: ' + e.message);
            }
        }

        _stop() {
            this._running = false;
            if (this._fetchCtrl) { this._fetchCtrl.abort(); this._fetchCtrl = null; }
            this._pendingQ = [];
            this._moofBuf  = null;
            this._ftypBuf  = null;
            if (this._stagingTimer) { clearTimeout(this._stagingTimer); this._stagingTimer = null; }
            this._stagingCodec         = null;
            this._seekAfterCodecChange = false;
            this._teardownMS();
        }

        _teardownMS() {
            if (this._sb && this._onUpdateEnd) {
                try { this._sb.removeEventListener('updateend', this._onUpdateEnd); } catch (_) {}
            }
            try {
                if (this._ms && this._ms.readyState === 'open') this._ms.endOfStream();
            } catch (_) {}
            if (this._objUrl) { URL.revokeObjectURL(this._objUrl); this._objUrl = null; }
            this._ms          = null;
            this._sb          = null;
            this._currentMime = '';
            this._onUpdateEnd = null;
        }

        _setupMS(mime, initBuf) {
            this._ms     = new MediaSource();
            this._objUrl = URL.createObjectURL(this._ms);
            this._videoEl.src = this._objUrl;

            this._ms.addEventListener('sourceopen', () => {
                if (!this._ms) return;
                // Safety-net for codec-change path (early check in _processBox covers first setup)
                const codecErr = checkCodecSupport(mime, this._cb.messages);
                if (codecErr) {
                    this._cb.error('[vjs] ' + codecErr);
                    this._cb.showError(codecErr);
                    return;
                }
                try {
                    this._currentMime = mime;
                    this._sb = this._ms.addSourceBuffer(mime);
                    this._sb.mode = 'segments';
                    this._onUpdateEnd = () => this._handleUpdateEnd();
                    this._sb.addEventListener('updateend', this._onUpdateEnd);
                    this._sb.addEventListener('error',
                        e => this._cb.error('[vjs] SB error: ' + e.type));
                    this._cb.info('[vjs] SourceBuffer created: ' + mime +
                                  ' | sb.mode=' + this._sb.mode);
                    this._pendingQ.unshift({ type: 'init', buf: initBuf });
                    this._drainQueue();
                } catch (e) {
                    this._cb.error('[vjs] addSourceBuffer failed: ' + e.message);
                }
            }, { once: true });
        }

        _handleUpdateEnd() {
            const v  = this._videoEl;
            const sb = this._sb;
            if (!sb || sb.updating) return;

            if (v.currentTime > 60 && sb.buffered.length > 0) {
                try {
                    const removeEnd = v.currentTime - 30;
                    const start = sb.buffered.start(0);
                    if (removeEnd > start + 1) {
                        sb.remove(start, removeEnd);
                        return;
                    }
                } catch (_) {}
            }

            // -- diagnostic dump --
            {
                const ct  = v.currentTime.toFixed(3);
                const rs  = v.readyState;
                let   buf = '(empty)';
                if (v.buffered.length)
                    buf = v.buffered.start(0).toFixed(3) + '..' +
                          v.buffered.end(v.buffered.length - 1).toFixed(3);
                this._cb.info('[vjs] updateend rs=' + rs + ' ct=' + ct +
                              ' buf=[' + buf + '] q=' + this._pendingQ.length);
            }

            if (v.readyState >= 3) {
                if (v.paused) {
                    const vjsPlayer = this._tech.player_;
                    this._cb.info('[vjs] play() -- rs=' + v.readyState +
                                  ' ct=' + v.currentTime.toFixed(3));
                    if (vjsPlayer) vjsPlayer.play().catch(() => {});
                    else           v.play().catch(() => {});
                }
            } else if (v.buffered.length > 0 && v.paused) {
                const bStart = v.buffered.start(0);
                this._cb.info('[vjs] play()+seek -- rs=' + v.readyState +
                              ' ct=' + v.currentTime.toFixed(3) +
                              ' bStart=' + bStart.toFixed(3));
                v.play().catch(() => {});
                if (v.currentTime < bStart - 0.05) v.currentTime = bStart;
            }

            if (this._seekAfterCodecChange && sb.buffered.length > 0) {
                const bStart = sb.buffered.start(0);
                const bEnd   = sb.buffered.end(sb.buffered.length - 1);
                if (v.currentTime < bStart - 0.1) {
                    this._cb.info('[vjs] post-codec seek: ' + v.currentTime.toFixed(2) +
                                  ' -> ' + bStart.toFixed(2) + 's');
                    v.currentTime = bStart;
                } else if (v.currentTime > bEnd + 0.02) {
                    const target = Math.max(bEnd - 0.1, bStart);
                    this._cb.info('[vjs] post-codec seek (past-end): ' +
                                  v.currentTime.toFixed(2) + ' -> ' + target.toFixed(2) + 's');
                    v.currentTime = target;
                }
                this._seekAfterCodecChange = false;
            }

            this._drainQueue();
            this._maybeEndStream();
        }

        _drainQueue() {
            if (!this._pendingQ.length) return;
            if (!this._ms || this._ms.readyState !== 'open') return;
            if (!this._sb || this._sb.updating) return;

            const item = this._pendingQ.shift();

            try {
                if (item.type === 'init' && !item.mimeResolved) {
                    const mime = parseMimeFromInitSegment(item.buf) || this._currentMime;
                    if (mime && mime !== this._currentMime) {
                        this._handleCodecChange(mime, item.buf);
                        return;
                    }
                } else if (item.type === 'data') {
                    // Log baseMediaDecodeTime of first few segments for diagnostics
                    if (this._pendingQ.length <= 2) {
                        const tfdt = readTfdt(new Uint8Array(item.buf));
                        if (tfdt !== null)
                            this._cb.info('[vjs] seg bMDT=' + (tfdt / 1000).toFixed(3) +
                                          's q=' + this._pendingQ.length);
                    }
                }
                this._sb.appendBuffer(item.buf);
            } catch (e) {
                this._cb.error('[vjs] appendBuffer: ' + e.message);
                this._maybeEndStream();
            }
            if (!this._pendingQ.length) this._maybeEndStream();
        }

        _handleCodecChange(mime, initBuf, preSegments = []) {
            this._cb.info('[vjs] Codec change -> ' + mime +
                          (preSegments.length ? ' (+' + preSegments.length + ' pre-staged)' : ''));

            if (this._sb && typeof this._sb.changeType === 'function') {
                try {
                    try { this._sb.abort(); } catch (_) {}
                    this._sb.changeType(mime);
                    this._currentMime = mime;
                    this._stallSince  = 0;
                    this._afterCodecChangeTick = 20;
                    this._cb.info('[vjs] changeType() -> ' + mime + ' | sb.mode=' + this._sb.mode);

                    this._pendingQ = [
                        { type: 'init', buf: initBuf, mimeResolved: true },
                        ...preSegments
                    ];

                    if (this._sb.buffered.length > 0) {
                        this._cb.info('[vjs] evict all pre-change data (ct=' +
                                      this._videoEl.currentTime.toFixed(2) + ')');
                        this._seekAfterCodecChange = true;
                        this._sb.remove(0, Infinity);
                    } else {
                        this._drainQueue();
                    }
                    return;
                } catch (e) {
                    this._cb.warn('[vjs] changeType failed: ' + e.message + ' -- will recreate MS');
                }
            }

            this._cb.warn('[vjs] Recreating MediaSource -> ' + mime);
            this._pendingQ = [
                { type: 'init', buf: initBuf, mimeResolved: true },
                ...preSegments
            ];
            this._moofBuf = null;
            this._teardownMS();
            this._setupMS(mime, initBuf);
        }

        _flushStaged() {
            if (!this._stagingCodec) return;
            clearTimeout(this._stagingTimer);
            this._stagingTimer = null;
            const { mime, initBuf, segments } = this._stagingCodec;
            this._stagingCodec = null;
            this._cb.info('[vjs] Flush pre-staged: ' + segments.length + ' seg(s) -> ' + mime);
            this._handleCodecChange(mime, initBuf, segments);
        }

        _processBox(boxBuf, boxType) {
            if (boxType === 'ftyp') {
                this._ftypBuf = boxBuf;
                return;
            }

            if (boxType === 'moov') {
                const mime = parseMimeFromInitSegment(boxBuf)
                    || 'video/mp4; codecs="avc1.42E01E,mp4a.40.2"';
                this._cb.info('[vjs] moov -> codec: ' + mime +
                              (this._ftypBuf ? ' (ftyp present)' : ' (no ftyp)'));

                // Check browser decode support as early as possible
                const codecErr = checkCodecSupport(mime, this._cb.messages);
                if (codecErr) {
                    this._cb.error('[vjs] ' + codecErr);
                    this._cb.showError(codecErr);
                    this._stop();
                    return;
                }

                const initBuf = this._ftypBuf
                    ? concat(new Uint8Array(this._ftypBuf), new Uint8Array(boxBuf)).buffer
                    : boxBuf;
                this._ftypBuf = null;

                if (!this._ms) {
                    if (this._moofBuf) {
                        this._pendingQ.push({ type: 'data', buf: this._moofBuf.buffer });
                        this._moofBuf = null;
                    }
                    this._setupMS(mime, initBuf);
                    return;
                }

                if (this._moofBuf) {
                    this._pendingQ.push({ type: 'data', buf: this._moofBuf.buffer });
                    this._moofBuf = null;
                }

                if (mime !== this._currentMime) {
                    if (this._stagingCodec) {
                        this._cb.info('[vjs] rapid re-codec during staging -- restart');
                        clearTimeout(this._stagingTimer);
                    }
                    this._stagingCodec = { mime, initBuf, segments: [] };
                    this._cb.info('[vjs] Start pre-staging for: ' + mime);
                    this._stagingTimer = setTimeout(() => this._flushStaged(), STAGE_TIMEOUT_MS);
                } else {
                    this._pendingQ.push({ type: 'init', buf: initBuf });
                    this._drainQueue();
                }
                return;
            }

            if (boxType === 'moof') {
                if (this._moofBuf) {
                    this._pendingQ.push({ type: 'data', buf: this._moofBuf.buffer });
                }
                this._moofBuf = new Uint8Array(boxBuf);
                return;
            }

            if (boxType === 'mdat') {
                if (this._moofBuf) {
                    const combined = concat(this._moofBuf, new Uint8Array(boxBuf));
                    this._moofBuf  = null;
                    if (this._stagingCodec) {
                        if (this._stagingCodec.segments.length === 0) {
                            const bmdt = readTfdt(combined);
                            this._cb.info('[vjs] first staged moof bMDT=' + bmdt + 'ms (offset check)');
                        }
                        this._stagingCodec.segments.push({ type: 'data', buf: combined.buffer });
                        if (this._stagingCodec.segments.length >= STAGE_SEGMENTS) {
                            this._flushStaged();
                        }
                        return;
                    }
                    this._pendingQ.push({ type: 'data', buf: combined.buffer });
                    this._drainQueue();
                }
            }
        }
    } // end S3ProLiveSourceHandler

    // =========================================================================
    // S3ProPlayer -- public API
    //
    // Constructor options:
    //   videojs       {Function} videojs library (falls back to window.videojs)
    //   videoElId     {string}   id of <video>/<div> element (default: 'vjs-player')
    //   vjsPlayer     {Object}   pre-created videojs player instance (skips creation)
    //   messages      {Object}   override default error message strings
    //
    // Callback shortcuts (all optional -- or use player.on(event, fn)):
    //   onLog         {Function} (level: 'info'|'warn'|'error', msg: string)
    //   onInfo        {Function} (msg) -- info-level log shortcut
    //   onWarn        {Function} (msg) -- warn-level log shortcut
    //   onError       {Function} (msg) -- error-level log shortcut (non-fatal)
    //   onFatalError  {Function} (msg) -- fatal error; surface to user, stop playback
    //   onTimeUpdate  {Function} (currentTime, formatted, codec, bufferSec)
    //   onStateChange {Function} (state)
    //
    // Events via player.on(event, fn) / player.off(event, fn):
    //   'log'         (level, msg)
    //   'error'       (msg)                -- fatal error (null clears previous error)
    //   'timeupdate'  (ct, fmt, codec, bufLen)
    //   'statechange' (state: 'idle'|'connecting'|'playing'|'stopped'|'error')
    // =========================================================================
    class S3ProPlayer {
        constructor(opts) {
            opts = opts || {};
            this._opts      = opts;
            this._state     = 'idle';
            this._listeners = {};
            this._activeHandler = null;
            this._pollTimer     = null;

            // Register opts callback shortcuts as event listeners
            if (opts.onLog)         this.on('log',         opts.onLog);
            if (opts.onInfo)        this.on('log',         (lv, m) => { if (lv === 'info')  opts.onInfo(m);  });
            if (opts.onWarn)        this.on('log',         (lv, m) => { if (lv === 'warn')  opts.onWarn(m);  });
            if (opts.onError)       this.on('log',         (lv, m) => { if (lv === 'error') opts.onError(m); });
            if (opts.onFatalError)  this.on('error',       opts.onFatalError);
            if (opts.onTimeUpdate)  this.on('timeupdate',  opts.onTimeUpdate);
            if (opts.onStateChange) this.on('statechange', opts.onStateChange);

            // Resolve videojs library
            const vjsLib = opts.videojs ||
                           (typeof videojs !== 'undefined' ? videojs : null); // eslint-disable-line no-undef
            if (!vjsLib) throw new Error('S3ProPlayer: videojs library is required. Pass it via opts.videojs.');
            this._vjsLib = vjsLib;

            // Create or adopt a videojs player instance
            if (opts.vjsPlayer) {
                this._vjsPlayer = opts.vjsPlayer;
            } else {
                const elId = opts.videoElId || 'vjs-player';
                this._vjsPlayer = vjsLib(elId, {
                    controls:      false,
                    fluid:         true,
                    muted:         true,
                    preload:       'none',
                    playbackRates: [1],
                    html5: {
                        nativeVideoTracks: false,
                        nativeAudioTracks: false,
                        nativeTextTracks:  false,
                    },
                    techOrder: ['html5'],
                });
            }

            // Register in module-level registry so source handler can find us via tech.player_
            if (_playerRegistry) _playerRegistry.set(this._vjsPlayer, this);

            // Register the fmp4 source handler with Html5Tech (once per tech class)
            this._registerSourceHandler();

            // Wire Video.js player events for state tracking and logging
            this._setupVjsEvents();

            // Start stall-detection + info-bar poll (200 ms)
            this._startPoll();
        }

        // -- Public API -------------------------------------------------------

        play(url) {
            this._emit('log', 'info', '=== Play: ' + url + ' ===');
            this._emit('error', null); // clear previous fatal error
            if (this._activeHandler) { this._activeHandler.dispose(); this._activeHandler = null; }
            this._setState('connecting');
            this._vjsPlayer.src({ src: url, type: 'application/x-fmp4live' });
        }

        stop() {
            this._emit('log', 'info', '=== Stop ===');
            this._vjsPlayer.pause();
            if (this._activeHandler) { this._activeHandler.dispose(); this._activeHandler = null; }
            this._vjsPlayer.reset();
            this._setState('stopped');
        }

        dispose() {
            if (this._pollTimer) { clearInterval(this._pollTimer); this._pollTimer = null; }
            if (this._activeHandler) { this._activeHandler.dispose(); this._activeHandler = null; }
            if (_playerRegistry && this._vjsPlayer) _playerRegistry.delete(this._vjsPlayer);
            try { this._vjsPlayer.dispose(); } catch (_) {}
            this._setState('disposed');
        }

        getState() {
            const h       = this._activeHandler;
            const el      = this._vjsPlayer.el && this._vjsPlayer.el();
            const videoEl = el ? el.querySelector('video') : null;
            let   bufLen  = 0;
            if (videoEl && videoEl.buffered && videoEl.buffered.length) {
                for (let i = 0; i < videoEl.buffered.length; i++)
                    bufLen += videoEl.buffered.end(i) - videoEl.buffered.start(i);
            }
            return {
                state:       this._state,
                codec:       h ? h._currentMime : '',
                bufferLen:   bufLen,
                currentTime: videoEl ? videoEl.currentTime : 0,
            };
        }

        on(event, cb) {
            if (!this._listeners[event]) this._listeners[event] = [];
            this._listeners[event].push(cb);
            return this;
        }

        off(event, cb) {
            if (!this._listeners[event]) return this;
            if (!cb) { this._listeners[event] = []; return this; }
            this._listeners[event] = this._listeners[event].filter(f => f !== cb);
            return this;
        }

        // -- Private ----------------------------------------------------------

        _emit(event, ...args) {
            const cbs = this._listeners[event];
            if (!cbs) return;
            cbs.forEach(cb => { try { cb(...args); } catch (_) {} });
        }

        _setState(s) {
            if (this._state === s) return;
            this._state = s;
            this._emit('statechange', s);
        }

        // Build a cb object for S3ProLiveSourceHandler backed by this player's event system
        _makeCb() {
            return {
                info:      msg => this._emit('log', 'info',  msg),
                warn:      msg => this._emit('log', 'warn',  msg),
                error:     msg => this._emit('log', 'error', msg),
                showError: msg => { this._setState('error'); this._emit('error', msg); },
                messages:  this._opts.messages || null,
            };
        }

        _registerSourceHandler() {
            const vjsLib    = this._vjsLib;
            const Html5Tech = vjsLib.getTech && vjsLib.getTech('Html5');
            if (!Html5Tech) {
                this._emit('log', 'warn', 'Video.js Html5 tech not available -- source handler not registered');
                return;
            }
            // Avoid double-registration for the same Html5Tech class
            if (_registeredTechs && _registeredTechs.has(Html5Tech)) return;
            if (_registeredTechs) _registeredTechs.add(Html5Tech);

            Html5Tech.registerSourceHandler({
                canHandleSource(source) {
                    if (!source.src) return '';
                    if (source.type === 'application/x-fmp4live') return 'probably';
                    if (/\.live\.mp4(\?.*)?$/.test(source.src))   return 'maybe';
                    return '';
                },
                handleSource(source, tech, options) {
                    // Find the owning S3ProPlayer via the registry
                    const vjsPlayer = tech.player_;
                    const fp = _playerRegistry && vjsPlayer
                        ? _playerRegistry.get(vjsPlayer) : null;
                    const cb = fp ? fp._makeCb() : {
                        info: () => {}, warn: () => {}, error: () => {},
                        showError: () => {}, messages: null,
                    };
                    const handler = new S3ProLiveSourceHandler(source, tech, options, cb);
                    if (fp) fp._activeHandler = handler;
                    return handler;
                },
            }, 0);
        }

        _setupVjsEvents() {
            const player = this._vjsPlayer;
            ['loadstart', 'loadedmetadata', 'canplay', 'playing', 'waiting', 'stalled', 'error', 'ended']
                .forEach(evName => {
                    player.on(evName, () => {
                        const v  = player.el().querySelector('video');
                        const rs = v ? v.readyState : '?';
                        this._emit('log', 'info', '[vjs-event] ' + evName + ' readyState=' + rs);
                        if (evName === 'playing') this._setState('playing');
                    });
                });
            player.on('error', () => {
                const err = player.error();
                if (err) this._emit('log', 'error', '[vjs] ' + err.message);
            });
        }

        _startPoll() {
            this._pollTimer = setInterval(() => this._poll(), 200);
        }

        _poll() {
            const player  = this._vjsPlayer;
            const el      = player.el && player.el();
            const videoEl = el ? el.querySelector('video') : null;
            const h       = this._activeHandler;

            if (!videoEl || !h || !h._running) {
                this._emit('timeupdate', 0, '00:00:00', '', 0);
                return;
            }

            const mime = h._currentMime || '';
            let bufLen = 0;
            if (videoEl.buffered && videoEl.buffered.length) {
                for (let i = 0; i < videoEl.buffered.length; i++)
                    bufLen += videoEl.buffered.end(i) - videoEl.buffered.start(i);
            }
            const ct = videoEl.currentTime;
            this._emit('timeupdate', ct, fmtTime(ct), mime, bufLen);

            const rs = videoEl.readyState;
            if (rs >= 3) {
                h._stallSince = 0;
                if (h._afterCodecChangeTick > 0) h._afterCodecChangeTick--;
                if (videoEl.paused) player.play().catch(() => {});
            } else if (videoEl.buffered.length > 0) {
                if (!h._stallSince) h._stallSince = performance.now();
                const stallMs   = performance.now() - h._stallSince;
                const postCodec = h._afterCodecChangeTick > 0;
                const outsideMs = postCodec ?  80 : 300;
                const nudgeMs   = postCodec ? 350 : 800;
                const bStart    = videoEl.buffered.start(0);
                const bEnd      = videoEl.buffered.end(videoEl.buffered.length - 1);

                if (videoEl.currentTime < bStart - 0.1 || videoEl.currentTime > bEnd + 0.1) {
                    if (stallMs > outsideMs) {
                        this._emit('log', 'info',
                            '[vjs] stall-skip: ct=' + videoEl.currentTime.toFixed(2) +
                            ' outside [' + bStart.toFixed(2) + '..' + bEnd.toFixed(2) +
                            '] -> seek to start (stalled ' + Math.round(stallMs) + 'ms)');
                        videoEl.currentTime = bStart;
                        h._stallSince = 0;
                        if (videoEl.paused) player.play().catch(() => {});
                    }
                } else if (stallMs > nudgeMs) {
                    const prevCt = videoEl.currentTime;
                    const nudge  = Math.min(prevCt + 0.15, Math.max(bEnd - 0.05, bStart));
                    h._stallSince = 0;
                    if (nudge !== prevCt) {
                        this._emit('log', 'info',
                            '[vjs] stall-skip: micro-nudge ' + prevCt.toFixed(2) +
                            ' -> ' + nudge.toFixed(2) + 's (stalled ' + Math.round(stallMs) + 'ms)');
                        videoEl.currentTime = nudge;
                    }
                    if (videoEl.paused) player.play().catch(() => {});
                }
            } else {
                h._stallSince = 0;
            }
        }
    } // end S3ProPlayer

    return S3ProPlayer;
}));
