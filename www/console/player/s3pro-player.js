// =============================================================================
// s3pro-player.js  —  S3Pro FMP4 streaming plugin for Video.js  (UMD)
//
// Architecture
// ============
//   • Extends Video.js via the official plugin API  (videojs.registerPlugin)
//   • Adds a custom SourceHandler for type 'application/x-fmp4live' that owns
//     the HTTP / WebSocket fetch loop and feeds raw FMP4 boxes into MSE.
//   • In-band codec / resolution changes are handled seamlessly:
//       1. When a new moov arrives, pre-stage STAGE_SEGS media segments.
//       2. After STAGE_SEGS segments (or STAGE_TIMEOUT_MS timeout), call
//          SourceBuffer.changeType() for same-family codec changes, otherwise
//          tear-down & recreate the MediaSource for cross-family changes.
//       3. SourceBuffer runs in 'sequence' mode — timestamps are managed by
//          the browser in append order, so no post-change seek is needed and
//          DTS discontinuities (e.g. server restart) are handled transparently.
//   • DTS metadata: readTfdt() extracts baseMediaDecodeTime from every moof
//     and forwards it via cb.onDts().  In sequence mode currentTime (not tfdt)
//     reflects actual playback position.
//
// Script-tag usage:
//   <script src="video.js"></script>
//   <script src="s3pro-player.js"></script>
//   const vp = videojs('my-video', { controls: false, muted: true });
//   vp.s3pro().play('http://host/cam.live.mp4');
//   vp.s3pro().on('timeupdate', (dts, fmt, codec, bufLen) => { ... });
//
// Standalone usage (no plugin):
//   const handler = new S3ProFmp4Handler(source, tech, options, cb);
//   handler.dispose();
//
// Exported symbols
//   S3ProFmp4Handler  — raw MSE source handler (class)
//   S3ProPlugin       — Video.js plugin class
//   registerPlugin(vjs) — register plugin with a vjs instance
// =============================================================================
(function (root, factory) {
    if (typeof define === 'function' && define.amd) {
        define([], factory);
    } else if (typeof module === 'object' && module.exports) {
        module.exports = factory();
    } else {
        root.S3ProPlayer = factory();   // legacy compat name
        root.S3ProFmp4   = factory();
    }
}(typeof self !== 'undefined' ? self : this, function () {
    'use strict';

    // =========================================================================
    // § 1 — Low-level helpers (pure functions, no state)
    // =========================================================================

    /** Concatenate two Uint8Arrays. */
    function concat(a, b) {
        const out = new Uint8Array(a.byteLength + b.byteLength);
        out.set(a, 0);
        out.set(b, a.byteLength);
        return out;
    }

    /** Format seconds as HH:MM:SS. */
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

    /**
     * Extract baseMediaDecodeTime from the first tfdt box inside a moof.
     * Returns time in **milliseconds** (divides 90 kHz ticks by 90).
     * Returns null if no tfdt found.
     */
    function readTfdt(data) {
        const v = new DataView(
            data.buffer ? data.buffer : data,
            data.byteOffset || 0,
            data.byteLength);
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
                    }
                    return v.getUint32(pos + 12) / 90;
                }
                pos += size;
            }
            return null;
        }
        return scan(0, data.byteLength);
    }

    /**
     * Walk moov / stsd to build a MIME-type string.
     * Returns '' if not enough info to determine codec.
     */
    function parseMimeFromInitSegment(buf) {
        const bytes = new Uint8Array(buf instanceof ArrayBuffer ? buf : buf.buffer,
                                     buf.byteOffset || 0,
                                     buf.byteLength);
        const dv    = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        const s4    = off => String.fromCharCode(bytes[off], bytes[off+1], bytes[off+2], bytes[off+3]);
        let   videoPart = '', audioPart = '';

        function walk(start, end) {
            let pos = start;
            while (pos + 8 <= end) {
                const size = dv.getUint32(pos);
                if (size < 8 || pos + size > end) break;
                const type = s4(pos + 4);

                if (['moov','trak','mdia','minf','stbl'].includes(type)) {
                    walk(pos + 8, pos + size);
                } else if (type === 'stsd') {
                    walk(pos + 16, pos + size);
                } else if (['avc1','avc3'].includes(type)) {
                    let inner = pos + 86;
                    while (inner + 8 <= pos + size) {
                        const is = dv.getUint32(inner);
                        if (is >= 11 && s4(inner + 4) === 'avcC') {
                            const p = bytes[inner + 9].toString(16).padStart(2,'0');
                            const c = bytes[inner + 10].toString(16).padStart(2,'0');
                            const l = bytes[inner + 11].toString(16).padStart(2,'0');
                            videoPart = type + '.' + p + c + l;
                            break;
                        }
                        if (is < 8) break;
                        inner += is;
                    }
                    if (!videoPart) videoPart = type;
                } else if (['hev1','hvc1'].includes(type)) {
                    let inner = pos + 86;
                    while (inner + 8 <= pos + size) {
                        const is = dv.getUint32(inner);
                        if (is >= 23 && s4(inner + 4) === 'hvcC') {
                            const b          = inner + 8;
                            const gps        = bytes[b + 1];
                            const profileSp  = (gps >> 6) & 0x3;
                            const tierF      = (gps >> 5) & 0x1;
                            const profileIdc = gps & 0x1f;
                            const compatU32  = dv.getUint32(b + 2);
                            const constrHi   = bytes[b + 6];
                            const levelIdc   = bytes[b + 12];
                            const spStr      = ['','A','B','C'][profileSp] || '';
                            const tierStr    = tierF ? 'H' : 'L';
                            const compatHex  = compatU32.toString(16).toUpperCase().replace(/0+$/,'') || '0';
                            const constrStr  = constrHi ? '.' + constrHi.toString(16).toUpperCase() : '';
                            videoPart = `${type}.${spStr}${profileIdc}.${compatHex}.${tierStr}${levelIdc}${constrStr}`;
                            break;
                        }
                        if (is < 8) break;
                        inner += is;
                    }
                    if (!videoPart) videoPart = type;
                } else if (type === 'mp4a') {
                    let inner = pos + 8 + 28;
                    while (inner + 8 <= pos + size) {
                        const is = dv.getUint32(inner);
                        if (is >= 13 && s4(inner + 4) === 'esds') {
                            let p = inner + 12;
                            const e2 = inner + is;
                            const skipLen = () => { while (p < e2 && (bytes[p++] & 0x80)); };
                            if (p < e2 && bytes[p++] === 0x03) {
                                skipLen(); p += 2;
                                const fl = bytes[p++];
                                if (fl & 0x80) p += 2;
                                if (fl & 0x40) p += bytes[p] + 1;
                                if (fl & 0x20) p += 2;
                                if (p < e2 && bytes[p++] === 0x04) {
                                    skipLen(); p += 13;
                                    if (p < e2 && bytes[p++] === 0x05) {
                                        skipLen();
                                        if (p + 1 < e2) audioPart = 'mp4a.40.' + ((bytes[p] >> 3) & 0x1f);
                                    }
                                }
                            }
                            break;
                        }
                        if (is < 8) break;
                        inner += is;
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

    // =========================================================================
    // § 2 — Default user-facing error messages (English)
    // =========================================================================
    const DEFAULT_MESSAGES = {
        noMSE:            'Browser does not support MediaSource Extensions (MSE).',
        hevcUnsupported:  'Browser does not support H.265/HEVC. Use Chrome/Edge on Windows or macOS with hardware decoding, or request an H.264 stream.',
        av1Unsupported:   'Browser does not support AV1: ',
        vp9Unsupported:   'Browser does not support VP9: ',
        vp8Unsupported:   'Browser does not support VP8: ',
        codecUnsupported: 'Codec not supported: ',
        connectionFailed: 'Connection failed: ',
        readError:        'Stream read error: ',
    };

    /**
     * Check whether the browser can decode the given MIME string.
     * Returns null if OK, or a human-readable error string.
     */
    function checkCodecSupport(mime, overrides) {
        const msg = overrides ? Object.assign({}, DEFAULT_MESSAGES, overrides) : DEFAULT_MESSAGES;
        if (!mime) return null;
        if (typeof MediaSource === 'undefined') return msg.noMSE;
        if (MediaSource.isTypeSupported(mime))  return null;
        const codecs = (mime.match(/codecs="([^"]+)"/) || [])[1] || mime;
        if (/hvc1|hev1/i.test(codecs)) return msg.hevcUnsupported;
        if (/av01/i.test(codecs))      return msg.av1Unsupported  + codecs;
        if (/vp09|vp9/i.test(codecs))  return msg.vp9Unsupported  + codecs;
        if (/vp8/i.test(codecs))       return msg.vp8Unsupported  + codecs;
        return msg.codecUnsupported + codecs;
    }

    // =========================================================================
    // § 3 — Constants
    // =========================================================================
    const STAGE_SEGS       = 1;     // moof+mdat pairs to pre-buffer before codec switch
    const STAGE_TIMEOUT_MS = 2000;  // max wait for pre-staging

    /**
     * Return a short string identifying the VIDEO codec family from a MIME type.
     * Used to decide whether changeType() is safe (same family) or we must
     * recreate the MediaSource (cross-family: e.g. hvc1 → avc1).
     */
    function _videoCodecFamily(mime) {
        if (!mime) return '';
        const m = mime.match(/codecs="([^"]+)"/);
        const codecs = m ? m[1] : mime;
        // Find the first video codec (skip audio-only codecs)
        const vc = codecs.split(',').map(s => s.trim().toLowerCase())
            .find(c => !/^mp4a|^opus|^ac-3|^ec-3|^flac|^vorbis/.test(c));
        if (!vc) return '';
        if (/^avc1|^avc3/.test(vc)) return 'avc';
        if (/^hvc1|^hev1/.test(vc)) return 'hevc';
        if (/^av01/.test(vc))       return 'av1';
        if (/^vp09/.test(vc))       return 'vp9';
        if (/^vp08/.test(vc))       return 'vp8';
        return vc.slice(0, 4); // unknown family — use first 4 chars as key
    }

    // =========================================================================
    // § 4 — S3ProFmp4Handler
    //
    // Responsibilities:
    //   * HTTP fetch loop (chunked streaming) or WebSocket transport (ws://)
    //   * FMP4 box parser: ftyp, moov, moof, mdat (other boxes discarded)
    //   * MediaSource / SourceBuffer lifecycle
    //   * Seamless in-band codec change via SourceBuffer.changeType() with
    //     graceful fallback to MediaSource recreation
    //   * DTS timebar: every moof's baseMediaDecodeTime forwarded to cb.onDts()
    //
    // cb = {
    //   log(level, msg)         level: 'info'|'warn'|'error'
    //   showError(msg)          fatal, stop playback
    //   onState(state)          'connecting'|'buffering'|'playing'|'ended'|'error'
    //   onDts(dtsMs, mime)      called for each moof with DTS in ms
    //   messages                optional override map for DEFAULT_MESSAGES
    // }
    // =========================================================================
    class S3ProFmp4Handler {

        constructor(source, tech, options, cb) {
            this._cb      = cb;
            this._tech    = tech;
            this._videoEl = tech ? tech.el() : null;
            this._url     = source.src;
            this._opts    = options || {};

            // --- HTTP / WS fetch ---
            this._running   = false;
            this._fetchCtrl = null;
            this._ws        = null;

            // --- MSE ---
            this._ms          = null;
            this._sb          = null;
            this._objUrl      = null;
            this._onUpdateEnd = null;
            this._pendingQ    = [];
            this._msOpen      = false;  // true after MediaSource.sourceopen fires
            this._pendingInit = null;   // {mime, initBuf} waiting for sourceopen

            // --- Box assembly ---
            this._ftypBuf = null;
            this._moofBuf = null;

            // --- Codec state ---
            this._currentMime  = '';
            this._stagingCodec = null;
            this._stagingTimer = null;

            // --- Diagnostics ---
            this._segmentsReceived = 0;
            this._moovReceived     = false;
            this._httpEnded        = false;
            this._noDataSince      = 0;
            this._waitLogSince     = 0;
            this._stallSince       = 0;

            this._start();
        }

        // -----------------------------------------------------------------------
        // Public
        // -----------------------------------------------------------------------

        dispose() { this._stop(); }

        get running()          { return this._running; }
        get currentMime()      { return this._currentMime; }
        get moovReceived()     { return this._moovReceived; }
        get segmentsReceived() { return this._segmentsReceived; }
        get httpEnded()        { return this._httpEnded; }

        // -----------------------------------------------------------------------
        // Transport
        // -----------------------------------------------------------------------

        _start() {
            this._running = true;
            // Create MediaSource immediately so videoEl.src is replaced right
            // away — clears any stale frame from the previous stream and gets
            // the browser into MSE mode before moov arrives.
            // sourceopen fires in a few ms, well before moov arrives (~500ms+).
            const ms     = new MediaSource();
            this._ms     = ms;
            this._objUrl = URL.createObjectURL(ms);
            this._videoEl.src = this._objUrl;
            // Setting src to the new blob URL fires 'emptied' which clears the last
            // decoded frame immediately.  Do NOT call load() here — it resets the
            // media timeline and causes a gap (currentTime=0 vs bStart≈0.09s) that
            // keeps readyState at 1 forever in sequence mode.
            ms.addEventListener('sourceopen', () => {
                if (this._ms !== ms) return; // stale: handler was replaced
                this._msOpen = true;
                this._log('info', '[fmp4] sourceopen fired, ms.readyState=' + ms.readyState);
                if (this._pendingInit) {
                    const { mime, initBuf } = this._pendingInit;
                    this._pendingInit = null;
                    this._doAddSourceBuffer(ms, mime, initBuf);
                }
            }, { once: true });
            if (/^wss?:\/\//i.test(this._url)) {
                this._connectWS(this._url);
            } else {
                this._connectHTTP(this._url);
            }
        }

        async _connectHTTP(url) {
            this._log('info', '[fmp4] HTTP connect: ' + url);
            let response;
            try {
                this._fetchCtrl = new AbortController();
                response = await fetch(url, {
                    signal:  this._fetchCtrl.signal,
                    headers: { 'Range': 'bytes=0-' },
                });
            } catch (e) {
                if (e.name !== 'AbortError') {
                    this._log('error', '[fmp4] fetch: ' + e.message);
                    this._cb.showError(this._msg('connectionFailed') + e.message);
                    this._cb.onState('error');
                }
                return;
            }
            if (!response.ok && response.status !== 206) {
                const m = 'HTTP ' + response.status + ' ' + response.statusText;
                this._log('error', '[fmp4] ' + m);
                this._cb.showError(m);
                this._cb.onState('error');
                return;
            }
            this._log('info', '[fmp4] HTTP connected');

            const reader = response.body.getReader();
            let partial  = new Uint8Array(0);

            try {
                while (this._running) {
                    const { value, done } = await reader.read();
                    if (done) {
                        this._log('info', '[fmp4] server closed stream');
                        this._httpEnded = true;
                        this._maybeEndStream();
                        this._cb.onState('ended');
                        break;
                    }
                    partial = this._parseChunk(partial, value);
                }
            } catch (e) {
                if (e.name !== 'AbortError') {
                    this._log('error', '[fmp4] read: ' + e.message);
                    this._cb.showError(this._msg('readError') + e.message);
                    this._cb.onState('error');
                }
            } finally {
                reader.releaseLock();
            }
        }

        _connectWS(url) {
            this._log('info', '[fmp4] WS connect: ' + url);
            let partial = new Uint8Array(0);
            const ws = new WebSocket(url);
            ws.binaryType = 'arraybuffer';
            this._ws = ws;

            ws.onopen = () => this._log('info', '[fmp4] WS connected');

            ws.onmessage = (ev) => {
                if (!this._running) return;
                partial = this._parseChunk(partial, new Uint8Array(ev.data));
            };

            ws.onerror = () => {
                this._log('error', '[fmp4] WS error');
                this._cb.showError(this._msg('connectionFailed') + 'WebSocket error');
                this._cb.onState('error');
            };

            ws.onclose = (ev) => {
                if (!this._running) return;
                this._log('info', '[fmp4] WS closed code=' + ev.code);
                this._httpEnded = true;
                this._maybeEndStream();
                this._cb.onState('ended');
            };
        }

        /**
         * Incrementally parse FMP4 boxes from a streamed byte sequence.
         * @param {Uint8Array} partial  leftover bytes from previous call
         * @param {Uint8Array} chunk    new bytes from transport
         * @returns {Uint8Array}        new leftover (< 8 bytes or incomplete box)
         */
        _parseChunk(partial, chunk) {
            const buf    = partial.byteLength > 0 ? concat(partial, chunk) : chunk;
            let   offset = 0;

            while (offset < buf.byteLength) {
                if (buf.byteLength - offset < 8) break;

                const dv = new DataView(buf.buffer, buf.byteOffset + offset, buf.byteLength - offset);
                let boxSize = dv.getUint32(0);

                if (boxSize === 1) {
                    if (buf.byteLength - offset < 16) break;
                    boxSize = dv.getUint32(12); // high 32 bits ignored (streams < 4 GB)
                }

                if (boxSize < 8)                       { break; }
                if (buf.byteLength - offset < boxSize) { break; }

                const boxType = String.fromCharCode(
                    buf[offset + 4], buf[offset + 5],
                    buf[offset + 6], buf[offset + 7]);

                const boxBuf = buf.buffer.slice(
                    buf.byteOffset + offset,
                    buf.byteOffset + offset + boxSize);

                this._onBox(boxBuf, boxType);
                offset += boxSize;
            }

            return buf.slice(offset);
        }

        // -----------------------------------------------------------------------
        // Box dispatch
        // -----------------------------------------------------------------------

        _onBox(boxBuf, type) {
            switch (type) {
                case 'ftyp': this._ftypBuf = boxBuf;  break;
                case 'moov': this._onMoov(boxBuf);    break;
                case 'moof': this._onMoof(boxBuf);    break;
                case 'mdat': this._onMdat(boxBuf);    break;
                // sidx, styp, emsg, etc. silently ignored
            }
        }

        _onMoov(boxBuf) {
            const mime = parseMimeFromInitSegment(boxBuf) || 'video/mp4; codecs="avc1.42E01E,mp4a.40.2"';
            this._log('info', '[fmp4] moov codec=' + mime);

            const codecErr = checkCodecSupport(mime, this._cb.messages);
            if (codecErr) {
                this._log('error', '[fmp4] ' + codecErr);
                this._cb.showError(codecErr);
                this._stop();
                return;
            }

            this._moovReceived = true;
            this._cb.onState('buffering');

            // Build init segment: ftyp (if any) + moov
            const initBuf = this._ftypBuf
                ? concat(new Uint8Array(this._ftypBuf), new Uint8Array(boxBuf)).buffer
                : boxBuf;
            this._ftypBuf = null;

            // Discard any orphaned moof from the previous codec segment
            this._moofBuf = null;

            if (!this._sb) {
                // First moov: SourceBuffer not yet created.
                // MS was created eagerly in _start(); add SB now (or queue if
                // sourceopen hasn't fired yet — very unlikely but safe).
                this._currentMime = mime;
                if (this._msOpen) {
                    this._doAddSourceBuffer(this._ms, mime, initBuf);
                } else {
                    this._pendingInit = { mime, initBuf };
                }
                return;
            }

            // Subsequent moov = codec / resolution change
            if (mime === this._currentMime) {
                // Same codec, different params (resolution change or re-init after seek)
                this._pendingQ.push({ buf: initBuf });
                this._drainQueue();
                return;
            }

            // Different MIME: pre-stage N segments before switching
            if (this._stagingCodec) {
                this._log('info', '[fmp4] rapid re-codec, restart staging');
                clearTimeout(this._stagingTimer);
                this._stagingTimer = null;
            }
            this._stagingCodec = { mime, initBuf, segments: [] };
            this._log('info', '[fmp4] pre-staging for ' + mime);
            this._stagingTimer = setTimeout(() => this._flushStaged(), STAGE_TIMEOUT_MS);
        }

        _onMoof(boxBuf) {
            // Discard any orphaned moof — moof without mdat is malformed stream data
            this._moofBuf = new Uint8Array(boxBuf);
        }

        _onMdat(boxBuf) {
            if (!this._moofBuf) return;

            const segment = concat(this._moofBuf, new Uint8Array(boxBuf));
            this._moofBuf = null;

            // Emit DTS for timebar
            const dtsMs = readTfdt(segment);
            if (dtsMs !== null) {
                this._cb.onDts(dtsMs,
                    this._currentMime || (this._stagingCodec && this._stagingCodec.mime) || '');
            }

            if (this._stagingCodec) {
                this._stagingCodec.segments.push({ buf: segment.buffer });
                if (this._stagingCodec.segments.length >= STAGE_SEGS) {
                    this._flushStaged();
                }
                return;
            }

            this._segmentsReceived++;
            this._log('info', '[fmp4] segment #' + this._segmentsReceived
                + ' size=' + segment.buffer.byteLength
                + ' dts=' + (dtsMs !== null ? dtsMs.toFixed(0) + 'ms' : 'n/a'));
            this._pendingQ.push({ buf: segment.buffer });
            this._drainQueue();
        }

        // -----------------------------------------------------------------------
        // Codec-change helpers
        // -----------------------------------------------------------------------

        _flushStaged() {
            if (!this._stagingCodec) return;
            clearTimeout(this._stagingTimer);
            this._stagingTimer = null;
            const { mime, initBuf, segments } = this._stagingCodec;
            this._stagingCodec = null;
            this._log('info', '[fmp4] flush staged ' + segments.length + ' seg(s) -> ' + mime);
            this._doCodecSwitch(mime, initBuf, segments);
        }

        /**
         * Switch the active SourceBuffer to a new MIME type.
         *
         * Strategy (sequence mode):
         *   1. Same codec family → SourceBuffer.changeType() then append init +
         *      pre-staged segments immediately.  No remove() or seek needed because
         *      'sequence' mode assigns timestamps in append order.
         *   2. Cross codec family (e.g. hvc1 → avc1) → recreate MediaSource.
         *      Chrome silently corrupts the decoder on cross-family changeType().
         *   3. If changeType() throws → recreate MediaSource as fallback.
         */
        _doCodecSwitch(mime, initBuf, preSegments) {
            this._log('info', '[fmp4] codec switch -> ' + mime);

            // changeType() is only safe within the same video codec family.
            // Cross-family (e.g. hvc1 → avc1) silently corrupts the decoder
            // in Chrome — treat it as a hard MS recreation instead.
            const curFamily   = _videoCodecFamily(this._currentMime);
            const newFamily   = _videoCodecFamily(mime);
            const crossFamily = curFamily && newFamily && curFamily !== newFamily;

            if (!crossFamily && this._sb && typeof this._sb.changeType === 'function') {
                try {
                    try { this._sb.abort(); } catch (_) {}
                    this._sb.changeType(mime);
                    this._currentMime = mime;
                    this._stallSince  = 0;

                    // sequence mode: timestampOffset resets to 0 after changeType().
                    // Set it to currentTime so the new segments are placed starting
                    // at the current playhead — no gap, no backward jump.
                    const ct = (this._videoEl && this._videoEl.currentTime > 0)
                        ? this._videoEl.currentTime : 0;
                    try { this._sb.timestampOffset = ct; } catch (_) {}

                    // Remove old-codec frames ahead of the playhead.
                    // Without this, the buffer still contains old Lo frames from
                    // [ct + duration(preSegments)] to bufferEnd, which play back
                    // between the pre-staged segments and the next live segments —
                    // causing visible alternating hi/lo frames at the switch point.
                    this._pendingQ = [{ buf: initBuf }, ...preSegments];
                    if (this._sb.buffered.length > 0) {
                        const bufEnd = this._sb.buffered.end(this._sb.buffered.length - 1);
                        if (bufEnd > ct + 0.1) {
                            // Async remove: _handleUpdateEnd → _drainQueue picks up pendingQ
                            this._sb.remove(ct, Infinity);
                            return;
                        }
                    }
                    this._drainQueue();
                    return;
                } catch (e) {
                    this._log('warn', '[fmp4] changeType failed: ' + e.message + ' — recreating MS');
                }
            }

            // Fallback: full MediaSource recreation
            if (crossFamily) {
                this._log('info', '[fmp4] cross-family codec switch (' +
                    curFamily + ' -> ' + newFamily + ') — recreating MediaSource');
            } else {
                this._log('warn', '[fmp4] recreating MediaSource -> ' + mime);
            }
            this._pendingQ = [...preSegments];
            this._moofBuf = null;
            this._teardownMS();
            this._setupMS(mime, initBuf);
        }

        // -----------------------------------------------------------------------
        // MSE management
        // -----------------------------------------------------------------------

        /**
         * Add a SourceBuffer to ms for the given codec, then drain the pending
         * queue.  Called from the _start() sourceopen handler (first moov) and
         * from the _setupMS() sourceopen handler (cross-family codec switch).
         */
        _doAddSourceBuffer(ms, mime, initBuf) {
            const err = checkCodecSupport(mime, this._cb.messages);
            if (err) {
                this._log('error', '[fmp4] ' + err);
                this._cb.showError(err);
                return;
            }
            try {
                this._sb = ms.addSourceBuffer(mime);
                this._sb.mode = 'sequence';

                this._onUpdateEnd = () => this._handleUpdateEnd();
                this._sb.addEventListener('updateend', this._onUpdateEnd);

                this._sb.addEventListener('error', () => {
                    this._log('error', '[fmp4] SourceBuffer decode error (' + mime + ')');
                    this._stop();
                    this._cb.showError('Media decode error (' + this._currentMime + ')');
                    this._cb.onState('error');
                });

                this._log('info', '[fmp4] SourceBuffer created: ' + mime);
                this._pendingQ.unshift({ buf: initBuf });
                this._log('info', '[fmp4] init segment queued, size=' + initBuf.byteLength);
                this._drainQueue();
            } catch (e) {
                this._log('error', '[fmp4] addSourceBuffer: ' + e.message);
            }
        }

        /**
         * Called only from _doCodecSwitch (cross-family fallback):
         * create a brand-new MediaSource, replace videoEl.src, then add SB.
         */
        _setupMS(mime, initBuf) {
            this._currentMime = mime;
            this._msOpen      = false;
            this._pendingInit = null;
            const ms     = new MediaSource();
            this._ms     = ms;
            this._objUrl = URL.createObjectURL(ms);
            this._videoEl.src = this._objUrl;
            ms.addEventListener('sourceopen', () => {
                if (this._ms !== ms) return;  // stale: a newer MS replaced this one
                this._msOpen = true;
                this._doAddSourceBuffer(ms, mime, initBuf);
            }, { once: true });
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
            this._msOpen      = false;
            this._pendingInit = null;
        }

        _handleUpdateEnd() {
            const v  = this._videoEl;
            const sb = this._sb;
            if (!sb || sb.updating) return;

            // Buffer diagnostic
            if (sb.buffered.length > 0) {
                const bStart = sb.buffered.start(0);
                const bEnd   = sb.buffered.end(sb.buffered.length - 1);
                this._log('info', '[fmp4] updateend ct=' + v.currentTime.toFixed(2)
                    + ' buffered=[' + bStart.toFixed(2) + '..' + bEnd.toFixed(2) + ']'
                    + ' readyState=' + v.readyState
                    + ' paused=' + v.paused);
            } else {
                this._log('info', '[fmp4] updateend — buffer empty, ct=' + v.currentTime.toFixed(2)
                    + ' readyState=' + v.readyState);
            }

            // Trim old frames: keep 30 s behind currentTime.
            // In sequence mode the buffered range grows linearly from 0, so
            // trimming by wall position keeps memory bounded.
            if (v.currentTime > 60 && sb.buffered.length > 0) {
                try {
                    const trimTo = v.currentTime - 30;
                    const bStart = sb.buffered.start(0);
                    if (trimTo > bStart + 1) {
                        sb.remove(bStart, trimTo);
                        return;
                    }
                } catch (_) {}
            }

            // Autoplay: once buffered data is available, resume playback.
            // sequence mode guarantees currentTime is always inside the buffer,
            // so no seek correction is needed before calling play().
            if (sb.buffered.length > 0 && v.paused) {
                const vp = this._tech && this._tech.player_;
                const playPromise = vp ? vp.play() : v.play();
                if (playPromise && playPromise.catch) playPromise.catch(() => {});
            }

            // Notify playing state once the element has enough data.
            // VJS 'playing' event may be delayed on some configs — emit here so
            // the loading overlay is hidden as soon as the decoder can start.
            if (sb.buffered.length > 0 && v.readyState >= 3) {
                this._cb.onState('playing');
            }

            this._drainQueue();
            this._maybeEndStream();
        }

        _drainQueue() {
            if (!this._pendingQ.length) return;
            if (!this._ms || this._ms.readyState !== 'open') {
                if (this._ms && this._ms.readyState === 'ended') this._stop();
                return;
            }
            if (!this._sb || this._sb.updating) return;

            const item = this._pendingQ.shift();

            try {
                const bStr = this._sb.buffered.length > 0
                    ? this._sb.buffered.start(0).toFixed(2) + '..' + this._sb.buffered.end(this._sb.buffered.length - 1).toFixed(2)
                    : 'empty';
                this._log('info', '[fmp4] appendBuffer size=' + item.buf.byteLength
                    + ' buffered=' + bStr + ' queueLen=' + this._pendingQ.length);
                this._sb.appendBuffer(item.buf);
            } catch (e) {
                this._log('error', '[fmp4] appendBuffer: ' + e.message);
                // If the browser has set MediaElement.error (e.g. MEDIA_ERR_DECODE)
                // all further appends will also throw — stop immediately.
                if (this._videoEl && this._videoEl.error) {
                    this._log('error', '[fmp4] mediaElement.error set — stopping handler');
                    this._stop();
                    this._cb.showError('Media decode error (code ' +
                        this._videoEl.error.code + '): ' + this._videoEl.error.message);
                    this._cb.onState('error');
                    return;
                }
                this._maybeEndStream();
            }

            if (!this._pendingQ.length) this._maybeEndStream();
        }

        _maybeEndStream() {
            if (!this._httpEnded)                            return;
            if (!this._ms || this._ms.readyState !== 'open') return;
            if (this._pendingQ.length > 0)                   return;
            if (this._sb && this._sb.updating)               return;
            try {
                this._log('info', '[fmp4] MediaSource.endOfStream()');
                this._ms.endOfStream();
            } catch (e) {
                this._log('warn', '[fmp4] endOfStream: ' + e.message);
            }
        }

        // -----------------------------------------------------------------------
        // Cleanup
        // -----------------------------------------------------------------------

        _stop() {
            this._running = false;
            if (this._fetchCtrl) { this._fetchCtrl.abort(); this._fetchCtrl = null; }
            if (this._ws)        { this._ws.close();        this._ws = null;        }
            if (this._stagingTimer) { clearTimeout(this._stagingTimer); this._stagingTimer = null; }
            this._pendingQ         = [];
            this._moofBuf          = null;
            this._ftypBuf          = null;
            this._stagingCodec     = null;
            this._moovReceived     = false;
            this._httpEnded        = false;
            this._noDataSince      = 0;
            this._waitLogSince     = 0;
            this._segmentsReceived = 0;
            this._teardownMS();
        }

        // -----------------------------------------------------------------------
        // Stall detection (called by plugin poll every 200 ms)
        // -----------------------------------------------------------------------

        /**
         * Poll tick: handle stall nudging and no-data timeout.
         * Returns true if a reconnect is recommended.
         */
        tickPoll() {
            const v = this._videoEl;
            if (!v || !this._running) return false;

            const rs = v.readyState;

            // No-data timeout (10 s with no buffered frames)
            if (!this._httpEnded && rs <= 1 && v.buffered.length === 0) {
                if (!this._noDataSince) this._noDataSince = performance.now();
                if (performance.now() - this._noDataSince > 10000) {
                    this._log('warn', '[fmp4] no data 10s — reconnect needed');
                    this._noDataSince = 0;
                    return true;
                }
            } else {
                this._noDataSince = 0;
            }

            // Periodic "waiting for first segment" diagnostic
            if (!this._httpEnded && this._segmentsReceived === 0 && rs <= 1) {
                const now = performance.now();
                if (!this._waitLogSince) this._waitLogSince = now;
                if (now - this._waitLogSince >= 3000) {
                    this._log('warn', '[fmp4] waiting for first segment...');
                    this._waitLogSince = now;
                }
            } else if (this._segmentsReceived > 0) {
                this._waitLogSince = 0;
            }

            // Stall detection & nudge
            if (v.buffered.length > 0) {
                const bStart = v.buffered.start(0);
                const bEnd   = v.buffered.end(v.buffered.length - 1);
                // ct past buffer end (sequence-mode quirk: readyState may stay 4
                // even though decoder has no frames left) — reset to buffer start
                if (v.currentTime > bEnd + 0.1) {
                    if (!this._stallSince) this._stallSince = performance.now();
                    if (performance.now() - this._stallSince > 300) {
                        this._log('info', '[fmp4] stall-skip ct=' + v.currentTime.toFixed(2) +
                            ' past bEnd=' + bEnd.toFixed(2) + ', reset to bStart=' + bStart.toFixed(2));
                        v.currentTime    = bStart;
                        this._stallSince = 0;
                        const vp = this._tech && this._tech.player_;
                        if (v.paused) (vp ? vp.play() : v.play()).catch(() => {});
                    }
                } else if (rs >= 3) {
                    this._stallSince = 0;
                } else {
                    if (!this._stallSince) this._stallSince = performance.now();
                    const stallMs = performance.now() - this._stallSince;
                    if (v.currentTime < bStart - 0.01) {
                        // ct before buffer start
                        if (stallMs > 300) {
                            this._log('info', '[fmp4] stall-skip ct=' + v.currentTime.toFixed(2) +
                                ' before bStart=' + bStart.toFixed(2));
                            v.currentTime    = bStart;
                            this._stallSince = 0;
                            const vp = this._tech && this._tech.player_;
                            if (v.paused) (vp ? vp.play() : v.play()).catch(() => {});
                        }
                    } else if (stallMs > 800) {
                        // ct within buffer but decoder stalled — nudge forward
                        const nudge = Math.min(v.currentTime + 0.15, Math.max(bEnd - 0.05, bStart));
                        this._log('info', '[fmp4] stall-nudge ' + v.currentTime.toFixed(2) +
                            ' -> ' + nudge.toFixed(2));
                        v.currentTime    = nudge;
                        this._stallSince = 0;
                        const vp = this._tech && this._tech.player_;
                        if (v.paused) (vp ? vp.play() : v.play()).catch(() => {});
                    }
                }
            } else {
                this._stallSince = 0;
            }

            return false;
        }

        // -----------------------------------------------------------------------
        _log(level, msg) { this._cb.log(level, msg); }
        _msg(key) {
            const m = this._cb.messages;
            return (m && m[key] != null) ? m[key] : DEFAULT_MESSAGES[key];
        }

    } // end S3ProFmp4Handler

    // =========================================================================
    // § 5 — SourceHandler registration (once per Html5Tech class)
    // =========================================================================

    // WeakMap: vjsPlayer -> S3ProPlugin
    const _pluginRegistry  = typeof WeakMap !== 'undefined' ? new WeakMap() : null;
    // WeakSet: Html5Tech -> registered
    const _registeredTechs = typeof WeakSet !== 'undefined' ? new WeakSet() : null;
    // Synchronous slot: set while vjsPlayer.src() is executing
    let _pendingPlugin = null;

    function _registerSourceHandler(vjsLib) {
        const Html5Tech = vjsLib.getTech && vjsLib.getTech('Html5');
        if (!Html5Tech) return;
        if (_registeredTechs && _registeredTechs.has(Html5Tech)) return;
        if (_registeredTechs) _registeredTechs.add(Html5Tech);

        Html5Tech.registerSourceHandler({
            canHandleSource(source) {
                if (!source.src) return '';
                if (source.type === 'application/x-fmp4live')     return 'probably';
                if (/\.(live|live2)\.mp4(\?.*)?$/.test(source.src)) return 'maybe';
                return '';
            },
            handleSource(source, tech, options) {
                const vjsPlayer = tech.player_;
                const plugin = _pendingPlugin
                    || (_pluginRegistry && vjsPlayer ? _pluginRegistry.get(vjsPlayer) : null);

                const cb      = plugin ? plugin._makeHandlerCb() : _makeFallbackCb();
                const handler = new S3ProFmp4Handler(source, tech, options, cb);
                if (plugin) plugin._handler = handler;
                return handler;
            },
        }, 0);
    }

    function _makeFallbackCb() {
        return {
            log:      (lv, msg) => (console[lv] || console.log)('[s3pro]', msg),
            showError:(msg) => console.error('[s3pro] fatal:', msg),
            onState:  () => {},
            onDts:    () => {},
            messages: null,
        };
    }

    // =========================================================================
    // § 6 — S3ProPlugin  (Video.js plugin class)
    //
    // Register once with registerPlugin(videojs), then each player gets its
    // own instance on first call to vp.s3pro().
    //
    // Usage:
    //   const vp = videojs('my-video', { muted: true });
    //   const sp = vp.s3pro();
    //   sp.play('http://host/cam.live.mp4');
    //   sp.on('dts',         (dtsMs, codec) => updateTimebar(dtsMs));
    //   sp.on('timeupdate',  (dts, fmt, codec, bufLen) => updateHUD(fmt, codec));
    //   sp.on('statechange', state => updateUI(state));
    //   sp.on('error',       msg => showFatal(msg));
    //   sp.stop();
    //   sp.dispose();
    //
    // States: 'idle' | 'connecting' | 'buffering' | 'playing' | 'stopped' | 'error'
    // =========================================================================
    class S3ProPlugin {

        constructor(player, opts) {
            this._player    = player;
            this._opts      = opts || {};
            this._handler   = null;
            this._state     = 'idle';
            this._listeners = {};
            this._pollTimer = null;

            // Convenience shorthand wiring
            if (opts.onLog)         this.on('log',         opts.onLog);
            if (opts.onFatalError)  this.on('error',       opts.onFatalError);
            if (opts.onTimeUpdate)  this.on('timeupdate',  opts.onTimeUpdate);
            if (opts.onStateChange) this.on('statechange', opts.onStateChange);
            if (opts.onDts)         this.on('dts',         opts.onDts);

            // Register source handler
            const vjsLib = player.constructor
                || (typeof videojs !== 'undefined' ? videojs : null); // eslint-disable-line no-undef
            if (vjsLib) _registerSourceHandler(vjsLib);

            // Track native vjs state
            player.on('playing', () => this._setState('playing'));
            player.on('ended',   () => this._setState('stopped'));
            player.on('error',   () => {
                const err = player.error();
                if (err) {
                    this._emit('log', 'error', '[vjs] ' + err.message);
                    // VJS-level error (e.g. MEDIA_ERR_DECODE from the tech) must
                    // transition state to 'error' so the loading overlay is hidden.
                    this._setState('error');
                    this._emit('error', err.message);
                }
            });

            if (_pluginRegistry) _pluginRegistry.set(player, this);

            this._pollTimer = setInterval(() => this._poll(), 200);
        }

        // -----------------------------------------------------------------------
        // Public API
        // -----------------------------------------------------------------------

        /**
         * Start streaming. url may begin with http(s):// or ws(s)://.
         */
        play(url) {
            this._emit('log', 'info', '=== play: ' + url + ' ===');
            this._emit('error', null);
            this._player.pause();
            this._stopHandler();
            // Clear any VJS error overlay from the previous stream without
            // triggering a full reset (which can race with the new handler's
            // eager MediaSource creation and delay sourceopen by ~15 s).
            try { this._player.error(null); } catch (_) {}
            this._setState('connecting');

            const type = /\.m3u8(\?.*)?$/i.test(url)
                ? 'application/x-mpegURL'
                : 'application/x-fmp4live';

            _pendingPlugin = this;
            this._player.src({ src: url, type });
            _pendingPlugin = null;

            this._player.play().catch(() => {});
        }

        stop() {
            this._emit('log', 'info', '=== stop ===');
            this._player.pause();
            this._stopHandler();
            this._player.reset();
            this._setState('stopped');
        }

        dispose() {
            if (this._pollTimer) { clearInterval(this._pollTimer); this._pollTimer = null; }
            this._stopHandler();
            if (_pluginRegistry && this._player) _pluginRegistry.delete(this._player);
        }

        getState() {
            const h  = this._handler;
            const el = this._player.el && this._player.el();
            const v  = el ? el.querySelector('video') : null;
            let   bufLen = 0;
            if (v && v.buffered) {
                for (let i = 0; i < v.buffered.length; i++)
                    bufLen += v.buffered.end(i) - v.buffered.start(i);
            }
            return {
                state:       this._state,
                codec:       h ? h.currentMime : '',
                bufferLen:   bufLen,
                currentTime: v ? v.currentTime : 0,
            };
        }

        on(event, cb) {
            (this._listeners[event] = this._listeners[event] || []).push(cb);
            return this;
        }

        off(event, cb) {
            if (!this._listeners[event]) return this;
            if (!cb) { this._listeners[event] = []; return this; }
            this._listeners[event] = this._listeners[event].filter(f => f !== cb);
            return this;
        }

        // -----------------------------------------------------------------------
        // Private
        // -----------------------------------------------------------------------

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

        _stopHandler() {
            if (this._handler) { this._handler.dispose(); this._handler = null; }
        }

        /** Build the cb object for S3ProFmp4Handler. */
        _makeHandlerCb() {
            return {
                log:      (level, msg) => this._emit('log', level, msg),
                showError:(msg) => { this._setState('error'); this._emit('error', msg); },
                onState:  (s) => {
                    if (s === 'playing') {
                        this._setState('playing');
                    } else if (s === 'buffering' && this._state === 'connecting') {
                        this._setState('buffering');
                    } else if (s === 'ended') {
                        this._setState('stopped');
                    } else if (s === 'error') { 
                        this._setState('error');
                    }
                },
                onDts:    (dtsMs, codec) => this._emit('dts', dtsMs, codec),
                messages: this._opts.messages || null,
            };
        }

        /** 200 ms poll: emit timeupdate, drive stall detection. */
        _poll() {
            const el = this._player.el && this._player.el();
            const v  = el ? el.querySelector('video') : null;
            if (!v) return;

            const h      = this._handler;
            const mime   = h ? h.currentMime : '';
            let   bufLen = 0;
            if (v.buffered) {
                for (let i = 0; i < v.buffered.length; i++)
                    bufLen += v.buffered.end(i) - v.buffered.start(i);
            }
            this._emit('timeupdate', v.currentTime, fmtTime(v.currentTime), mime, bufLen);

            if (!h || !h.running) return;

            const needReconnect = h.tickPoll();
            if (needReconnect) {
                this._setState('error');
                this._emit('error', 'No data received from server (timeout)');
            }
        }

    } // end S3ProPlugin

    // =========================================================================
    // § 7 — registerPlugin helper
    // =========================================================================

    /**
     * Register the 's3pro' plugin with a videojs library instance.
     * Idempotent — safe to call multiple times.
     *
     *   registerPlugin(videojs);
     *   const vp = videojs('my-video');
     *   vp.s3pro().play('http://host/cam.live.mp4');
     */
    function registerPlugin(vjsLib) {
        if (!vjsLib || !vjsLib.registerPlugin) {
            console.warn('[s3pro] videojs.registerPlugin not available');
            return;
        }
        if (vjsLib.getPlugin && vjsLib.getPlugin('s3pro')) return;

        vjsLib.registerPlugin('s3pro', function s3proPluginFactory(opts) {
            const player = this;
            if (player._s3proInstance) return player._s3proInstance;
            const instance = new S3ProPlugin(player, opts || {});
            player._s3proInstance = instance;
            return instance;
        });

        _registerSourceHandler(vjsLib);
    }

    // =========================================================================
    // § 8 — Auto-register when videojs is present on window
    // =========================================================================
    if (typeof videojs !== 'undefined') { // eslint-disable-line no-undef
        registerPlugin(videojs); // eslint-disable-line no-undef
    }

    // =========================================================================
    // § 9 — Exports
    // =========================================================================
    return {
        S3ProFmp4Handler,
        S3ProPlugin,
        registerPlugin,
        // helpers (exposed for unit tests)
        parseMimeFromInitSegment,
        readTfdt,
        checkCodecSupport,
        fmtTime,
        // Legacy compat: old code used `new S3ProPlayer({ videojs, videoElId })`
        S3ProPlayer: S3ProPlugin,
    };
}));
