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

    /**
     * Check whether a moov box contains an mvex child (required for FMP4 / MSE).
     * A moov without mvex is a non-fragmented MP4 — Chrome MSE rejects it.
     */
    function hasMvex(buf) {
        const bytes = new Uint8Array(buf instanceof ArrayBuffer ? buf : buf.buffer,
                                     buf.byteOffset || 0, buf.byteLength);
        const dv    = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        let pos = 8; // skip moov box header
        while (pos + 8 <= bytes.byteLength) {
            const size = dv.getUint32(pos);
            if (size < 8 || pos + size > bytes.byteLength) break;
            const type = String.fromCharCode(bytes[pos+4], bytes[pos+5],
                                             bytes[pos+6], bytes[pos+7]);
            if (type === 'mvex') return true;
            pos += size;
        }
        return false;
    }

    /**
     * For AVC (H.264) codecs with level > 4.0 (0x28), return a new MIME string
     * with level clamped to High Profile Level 4.0 (avc1.640028).
     * Chrome picks a hardware decoder based on the MIME codec string; Level 4.0
     * has broad hardware support. Level 5.1 can cause immediate SourceBuffer
     * decode errors on platforms where the GPU driver doesn't support it, even
     * when isTypeSupported() returns true.
     * Returns null if no permissive alternative is needed.
     */
    function _avcPermissiveMime(mime) {
        const m = mime.match(/codecs="([^"]+)"/);
        if (!m) return null;
        const parts = m[1].split(',').map(c => c.trim());
        let changed = false;
        const newParts = parts.map(c => {
            if (/^(avc1|avc3)\./i.test(c) && c.length >= 11) {
                const levelHex = c.slice(-2);
                if (parseInt(levelHex, 16) > 0x28) { // > Level 4.0
                    changed = true;
                    // Preserve original profile+constraint bytes, clamp only the level.
                    // e.g. avc1.4d0033 (Main 5.1) → avc1.4d0028 (Main 4.0)
                    //      avc1.640033 (High 5.1) → avc1.640028 (High 4.0)
                    // Switching to a different profile (e.g. forced High) causes a
                    // profile mismatch and the SourceBuffer decode error repeats.
                    return c.slice(0, -2) + '28';
                }
            }
            return c;
        });
        return changed ? 'video/mp4; codecs="' + newParts.join(',') + '"' : null;
    }

    /**
     * Build a permissive HEVC MIME string for SourceBuffer / VideoDecoder fallback.
     * Some cameras advertise a too-low level (e.g. L60 = Level 2.0) while the
     * actual bitstream is encoded at Level 4.0+.  Chrome's hardware decoder
     * rejects the NAL units because they exceed the declared level.
     * Strategy: bump to L153 (Level 5.1, covers 1080p60 / 4K30); if already
     * ≥ L153 bump to L183 (Level 6.1).  Returns null if no change is needed.
     */
    function _hevcPermissiveMime(mime) {
        const m = mime.match(/codecs="([^"]+)"/);
        if (!m) return null;
        const parts = m[1].split(',').map(c => c.trim());
        let changed = false;
        const newParts = parts.map(c => {
            if (/^(hvc1|hev1)\./i.test(c)) {
                const lm = c.match(/\.L(\d+)\./);
                if (lm) {
                    const cur    = parseInt(lm[1]);
                    const target = cur < 153 ? 153 : 183;
                    if (target !== cur) {
                        changed = true;
                        return c.replace(/\.L\d+\./, '.L' + target + '.');
                    }
                }
            }
            return c;
        });
        return changed ? 'video/mp4; codecs="' + newParts.join(',') + '"' : null;
    }

    // =========================================================================
    // DtsTracker — monotonic DTS smoothing (owned by S3ProPlugin)
    //
    // Tracks the most recent decoded timestamp and interpolates elapsed time
    // between server packets using the real wall clock.
    // =========================================================================
    class DtsTracker {
        constructor() { this.reset(); }

        reset() {
            this._base   = null;  // first raw tfdt of current session (ms)
            this._last   = 0;     // most recent raw tfdt (ms)
            this._accum  = 0;     // accumulated time across tfdt resets (ms)
            this._lastMs = 0;     // monotonic ms at last feed() call
            this._realTs = 0;     // Date.now() at last feed() call
        }

        /** Ingest a raw tfdt value (ms). Returns the current monotonic offset. */
        feed(rawMs) {
            if (this._base === null) {
                this._base = rawMs; this._last = rawMs; this._accum = 0;
            } else if (rawMs < this._last - 1000) {
                // tfdt jumped backward by > 1 s — codec switch / server reset
                this._accum += this._last - this._base;
                this._base   = rawMs;
            }
            this._last   = rawMs;
            this._lastMs = this._accum + (rawMs - this._base);
            this._realTs = Date.now();
            return this._lastMs;
        }

        /**
         * Monotonic elapsed ms, interpolated at wall-clock speed since the last
         * feed() call. Capped at 5 s of extrapolation to avoid runaway when paused.
         */
        get elapsed() {
            if (this._realTs === 0) return 0;
            return this._lastMs + Math.min(Date.now() - this._realTs, 5000);
        }

        /** True once the first feed() has been called for this session. */
        get hasData() { return this._realTs > 0; }
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

    /**
     * Close VJS's ErrorDisplay modal and remove the vjs-error CSS class via
     * every available mechanism (VJS API + direct DOM) so the overlay hides
     * regardless of VJS version.
     */
    function _closeVjsErrorDisplay(player) {
        try {
            // VJS public API — works in most versions
            const ed = (player.errorDisplay) || (typeof player.getChild === 'function' && player.getChild('ErrorDisplay'));
            if (ed && typeof ed.close === 'function') ed.close();
        } catch (_) {}
        try {
            // Direct DOM: the ErrorDisplay ModalDialog uses vjs-modal-dialog-open
            const errEl = player.el().querySelector('.vjs-error-display');
            if (errEl) {
                errEl.classList.remove('vjs-modal-dialog-open');
                errEl.setAttribute('aria-hidden', 'true');
            }
        } catch (_) {}
    }

    /**
     * Walk moov/trak/.../avc1|avc3 and return the raw avcC box payload
     * (the bytes after the 8-byte box header) as a Uint8Array.
     * Used as the `description` field for VideoDecoder.configure().
     * Returns null if the initBuf contains no AVC track.
     */
    function _extractAvcDescription(initBuf) {
        if (!initBuf) return null;
        const bytes = new Uint8Array(initBuf instanceof ArrayBuffer ? initBuf : initBuf.buffer,
                                     initBuf.byteOffset || 0, initBuf.byteLength);
        const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        const s4 = off => String.fromCharCode(bytes[off], bytes[off+1], bytes[off+2], bytes[off+3]);
        function find(start, end) {
            let pos = start;
            while (pos + 8 <= end) {
                const size = dv.getUint32(pos);
                if (size < 8 || pos + size > end) break;
                const type = s4(pos + 4);
                if (['moov','trak','mdia','minf','stbl'].includes(type)) {
                    const r = find(pos + 8, pos + size);
                    if (r) return r;
                } else if (type === 'stsd') {
                    const r = find(pos + 16, pos + size);
                    if (r) return r;
                } else if (type === 'avc1' || type === 'avc3') {
                    let inner = pos + 86;
                    while (inner + 8 <= pos + size) {
                        const is = dv.getUint32(inner);
                        if (is >= 8 && s4(inner + 4) === 'avcC') {
                            return bytes.slice(inner + 8, inner + is);
                        }
                        if (is < 8) break;
                        inner += is;
                    }
                }
                pos += size;
            }
            return null;
        }
        return find(0, bytes.byteLength);
    }

    /**
     * Walk moov/trak/.../hvc1|hev1 and return the raw hvcC box payload
     * (the bytes after the 8-byte box header) as a Uint8Array.
     * Used as the `description` field for VideoDecoder.configure() with HEVC.
     * Returns null if the initBuf contains no HEVC track.
     */
    function _extractHevcDescription(initBuf) {
        if (!initBuf) return null;
        const bytes = new Uint8Array(initBuf instanceof ArrayBuffer ? initBuf : initBuf.buffer,
                                     initBuf.byteOffset || 0, initBuf.byteLength);
        const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
        const s4 = off => String.fromCharCode(bytes[off], bytes[off+1], bytes[off+2], bytes[off+3]);
        function find(start, end) {
            let pos = start;
            while (pos + 8 <= end) {
                const size = dv.getUint32(pos);
                if (size < 8 || pos + size > end) break;
                const type = s4(pos + 4);
                if (['moov','trak','mdia','minf','stbl'].includes(type)) {
                    const r = find(pos + 8, pos + size);
                    if (r) return r;
                } else if (type === 'stsd') {
                    const r = find(pos + 16, pos + size);
                    if (r) return r;
                } else if (type === 'hvc1' || type === 'hev1') {
                    let inner = pos + 86;
                    while (inner + 8 <= pos + size) {
                        const is = dv.getUint32(inner);
                        if (is >= 8 && s4(inner + 4) === 'hvcC') {
                            return bytes.slice(inner + 8, inner + is);
                        }
                        if (is < 8) break;
                        inner += is;
                    }
                }
                pos += size;
            }
            return null;
        }
        return find(0, bytes.byteLength);
    }

    /**
     * Scan moov to find the track ID of the first video (handler type 'vide') track.
     * Used to filter audio sample entries when `_parseMoofSamples` processes a
     * multiplexed FMP4 moof that contains both video and audio traf boxes.
     * Returns the track ID (integer ≥ 1), or 1 if no video trak is found.
     *
     * @param {ArrayBuffer|Uint8Array} initBuf  moov (or ftyp+moov) init segment
     * @returns {number}
     */
    function _findVideoTrackId(initBuf) {
        if (!initBuf) return 1;
        const b  = new Uint8Array(initBuf instanceof ArrayBuffer ? initBuf
                                  : initBuf.buffer, initBuf.byteOffset || 0, initBuf.byteLength);
        const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
        const s4 = o => String.fromCharCode(b[o], b[o+1], b[o+2], b[o+3]);

        // Scan children of a container box, invoking cb(pos, end) for each
        // matching box type; returns first non-null cb result.
        function scanFor(pos, end, targetType, cb) {
            while (pos + 8 <= end) {
                const sz = dv.getUint32(pos);
                if (sz < 8 || pos + sz > end) break;
                if (s4(pos + 4) === targetType) {
                    const r = cb(pos, pos + sz);
                    if (r !== null) return r;
                }
                pos += sz;
            }
            return null;
        }

        // Walk moov → each trak; return track ID of first 'vide' trak.
        const result = scanFor(0, b.byteLength, 'moov', (moovPos, moovEnd) =>
            scanFor(moovPos + 8, moovEnd, 'trak', (trakPos, trakEnd) => {
                let trackId = null, isVideo = false;
                let p = trakPos + 8;
                while (p + 8 <= trakEnd) {
                    const sz = dv.getUint32(p);
                    if (sz < 8 || p + sz > trakEnd) break;
                    const t = s4(p + 4);
                    if (t === 'tkhd') {
                        // tkhd FullBox: 8B hdr + 1B ver; track_ID at +20 (v0) or +28 (v1)
                        trackId = dv.getUint32(b[p + 8] === 1 ? p + 28 : p + 20);
                    } else if (t === 'mdia') {
                        // look for hdlr inside mdia to check handler_type
                        let mp = p + 8;
                        while (mp + 8 <= p + sz) {
                            const ms = dv.getUint32(mp);
                            if (ms < 8 || mp + ms > p + sz) break;
                            if (s4(mp + 4) === 'hdlr') {
                                // hdlr FullBox: 8B hdr + 4B ver/flags + 4B pre_defined + 4B handler_type
                                if (s4(mp + 16) === 'vide') isVideo = true;
                                break;
                            }
                            mp += ms;
                        }
                    }
                    p += sz;
                }
                return (isVideo && trackId !== null) ? trackId : null;
            })
        );
        return result !== null ? result : 1;
    }

    /**
     * Parse a moof+mdat pair and extract individual video samples.
     * Reads tfhd (default durations/sizes/flags), tfdt (base decode time),
     * and trun (per-sample list) from the moof box, then slices the mdat data.
     *
     * @param {Uint8Array|ArrayBuffer} moofBuf
     * @param {Uint8Array|ArrayBuffer} mdatBuf      full mdat box (includes 8-byte header)
     * @param {number|null}           videoTrackId  if non-null, only return samples from
     *                                              this track (skips audio traf entries)
     * @returns {{ data:Uint8Array, timestamp:number, duration:number, isKey:boolean }[]}
     *   timestamp and duration are in microseconds (assumes 90 kHz timescale).
     */
    function _parseMoofSamples(moofBuf, mdatBuf, videoTrackId = null) {
        const m   = (moofBuf instanceof Uint8Array) ? moofBuf
                  : new Uint8Array(moofBuf.buffer || moofBuf, moofBuf.byteOffset || 0, moofBuf.byteLength);
        const dv  = new DataView(m.buffer, m.byteOffset, m.byteLength);
        const s4  = off => String.fromCharCode(m[off], m[off+1], m[off+2], m[off+3]);
        const mdat = (mdatBuf instanceof Uint8Array) ? mdatBuf
                   : new Uint8Array(mdatBuf.buffer || mdatBuf, mdatBuf.byteOffset || 0, mdatBuf.byteLength);

        // Per-traf state — reset on each tfhd so that multi-track moof boxes
        // don't bleed values from one traf into another.
        let currentTrackId = 0;
        let defDuration    = 0;
        let defSize        = 0;
        let defFlags       = 0;
        let baseDecodeTime = 0;

        // Each run carries its own baseDecodeTime so multi-track boxes are correct.
        const runs = []; // { dataOffset, entries, baseDecodeTime }

        function walk(pos, end) {
            while (pos + 8 <= end) {
                const size = dv.getUint32(pos);
                if (size < 8 || pos + size > end) break;
                const type = s4(pos + 4);
                if (type === 'moof' || type === 'traf') {
                    walk(pos + 8, pos + size);
                } else if (type === 'tfhd') {
                    currentTrackId = dv.getUint32(pos + 12); // track_ID field
                    // Reset per-traf defaults so audio traf doesn't corrupt video state
                    defDuration = defSize = defFlags = 0;
                    const flags = (m[pos+9] << 16) | (m[pos+10] << 8) | m[pos+11];
                    let p = pos + 16; // 8B hdr + 4B ver/flags + 4B track_ID
                    if (flags & 0x000001) p += 8;  // base-data-offset-present
                    if (flags & 0x000002) p += 4;  // sample-description-index-present
                    if (flags & 0x000008) { defDuration = dv.getUint32(p); p += 4; }
                    if (flags & 0x000010) { defSize     = dv.getUint32(p); p += 4; }
                    if (flags & 0x000020) { defFlags    = dv.getUint32(p); }
                } else if (type === 'tfdt') {
                    const ver = m[pos + 8];
                    baseDecodeTime = (ver === 1)
                        ? dv.getUint32(pos+12) * 4294967296 + dv.getUint32(pos+16)
                        : dv.getUint32(pos + 12);
                } else if (type === 'trun') {
                    // Skip this traf entirely if it belongs to a non-video track
                    // (e.g. audio track in a multiplexed video+audio FMP4 segment).
                    if (videoTrackId !== null && currentTrackId !== videoTrackId) {
                        pos += size;
                        continue;
                    }
                    const flags = (m[pos+9] << 16) | (m[pos+10] << 8) | m[pos+11];
                    const count = dv.getUint32(pos + 12);
                    let p = pos + 16;
                    let dataOffset = 0, firstFlags = defFlags;
                    if (flags & 0x001) { dataOffset = dv.getInt32(p);  p += 4; }
                    if (flags & 0x004) { firstFlags = dv.getUint32(p); p += 4; }
                    const entries = [];
                    for (let i = 0; i < count; i++) {
                        let dur = defDuration, siz = defSize;
                        let flg = (i === 0) ? firstFlags : defFlags;
                        if (flags & 0x100) { dur = dv.getUint32(p); p += 4; }
                        if (flags & 0x200) { siz = dv.getUint32(p); p += 4; }
                        if (flags & 0x400) { flg = dv.getUint32(p); p += 4; }
                        if (flags & 0x800) p += 4; // composition-time-offset, unused
                        entries.push({ dur, siz, flg });
                    }
                    runs.push({ dataOffset, entries, baseDecodeTime });
                }
                pos += size;
            }
        }
        walk(0, m.byteLength);

        const TIMESCALE = 90000; // standard 90 kHz video timescale
        const result    = [];

        for (const run of runs) {
            // data_offset is relative to the start of the moof box (position 0 in m).
            // mdatBuf starts at position m.byteLength in the combined stream and
            // its first 8 bytes are the mdat box header (size[4] + type[4]).
            // So: offset_into_mdatBuf = data_offset - m.byteLength
            // Example: data_offset = m.byteLength + 8  →  off = 8 (first payload byte) ✓
            let off = run.dataOffset - m.byteLength;
            let dts = run.baseDecodeTime;
            for (const entry of run.entries) {
                if (entry.siz > 0 && off >= 0 && off + entry.siz <= mdat.byteLength) {
                    // sample_is_non_sync_sample = bit 16 of sample_flags
                    const isKey = !((entry.flg >> 16) & 0x1);
                    result.push({
                        data:      mdat.slice(off, off + entry.siz),
                        timestamp: Math.round(dts * 1000000 / TIMESCALE), // µs
                        duration:  Math.round(entry.dur * 1000000 / TIMESCALE),
                        isKey,
                    });
                }
                off += entry.siz;
                dts += entry.dur;
            }
        }
        return result;
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
            this._nudgedAt   = 0;      // timestamp of last readyState nudge (0 = never)
            this._nudgeCount = 0;      // how many nudges fired so far
            this._sbFallbackTried  = false; // true after one permissive-MIME retry
            this._initBuf         = null;   // saved init segment for fallback retry
            this._wc               = null;   // S3ProWebCodecsFallback, set when MSE fails
            this._wcFallbackTried  = false;  // true after WebCodecs fallback attempted
            this._wcDisposePending = null;   // WC canvas kept as frozen cover during WC→MSE transition
            this._csCanvas           = null;   // freeze-frame canvas shown during cross-family codec switch
            this._pendingCodecSwitch = false;  // true: changeType path waiting for async remove to complete

            // --- Box assembly ---
            this._ftypBuf = null;
            this._moofBuf = null;

            // --- Codec state ---
            this._currentMime  = '';
            this._stagingCodec = null;
            this._stagingTimer = null;

            // --- Diagnostics ---
            this._segmentsReceived  = 0;
            this._moovReceived      = false;
            this._httpEnded         = false;
            this._noDataSince       = 0;
            this._waitLogSince      = 0;
            this._stallSince        = 0;
            this._mseInitStallSince = 0; // for proactive HEVC→WebCodecs fallback

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
        /** True while VideoDecoder (WebCodecs) fallback is rendering to the canvas overlay. */
        get webCodecsActive()  { return this._wc !== null; }

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

            if (!hasMvex(boxBuf)) {
                this._log('warn', '[fmp4] moov has no mvex box — stream is not fragmented MP4; ' +
                    'Chrome MSE will likely reject the init segment');
            }

            const codecErr = checkCodecSupport(mime, this._cb.messages);
            if (codecErr) {
                this._log('error', '[fmp4] ' + codecErr);
                this._cb.showError(codecErr);
                this._stop();
                return;
            }

            // Emit 'buffering' only on the first moov (stream start).
            // Subsequent moov boxes are codec/resolution changes — suppress the
            // loading overlay for those to avoid a flash between the playing state.
            if (!this._moovReceived) {
                this._cb.onState('buffering');
            }
            this._moovReceived = true;

            // Build init segment: ftyp (if any) + moov
            const initBuf = this._ftypBuf
                ? concat(new Uint8Array(this._ftypBuf), new Uint8Array(boxBuf)).buffer
                : boxBuf;
            this._ftypBuf = null;

            // Discard any orphaned moof from the previous codec segment
            this._moofBuf = null;

            // ── WebCodecs active: live codec / resolution change ──────────────
            // Always try MSE first for codec changes — the new codec may have a
            // level/profile that the hardware decoder can handle even though the
            // previous one could not.  Keep the WC canvas visible as a frozen
            // cover while MSE initialises so there is no blank frame.
            // WC reinit is the fallback only when MediaSource.isTypeSupported()
            // returns false for the new codec.
            if (this._wc) {
                const mseSupportsMime = (typeof MediaSource !== 'undefined') &&
                    (() => { try { return MediaSource.isTypeSupported(mime); } catch (_) { return false; } })();

                if (mseSupportsMime) {
                    this._log('info', '[wc→mse] codec change -> ' + mime + '; trying MSE first');
                    // Freeze canvas as visual cover — stop feeding old decoder.
                    this._wcDisposePending = this._wc;
                    this._wc              = null;
                    this._sbFallbackTried = false;
                    this._wcFallbackTried = false;
                    this._segmentsReceived = 0;
                    this._pendingQ        = [];
                    this._setupMS(mime, initBuf);
                    return;
                }

                // MSE cannot handle this codec — reinitialise VideoDecoder directly.
                const wcNewCodec = (mime.match(/codecs="([^"]+)"/) || [])[1]
                    ?.split(',').map(s => s.trim())
                    .find(c => !/^mp4a|^opus|^ac-3/.test(c));
                if (wcNewCodec && /^avc1|^avc3|^hvc1|^hev1/i.test(wcNewCodec)) {
                    this._log('info', '[wc] in-stream codec change detected -> ' + wcNewCodec
                        + '; reinitialising VideoDecoder');
                    this._initBuf = new Uint8Array(initBuf);
                    const oldWc  = this._wc;
                    const wcNewBuf = new Uint8Array(initBuf);
                    const wcDesc = /^hvc1|^hev1/i.test(wcNewCodec)
                        ? _extractHevcDescription(wcNewBuf)
                        : _extractAvcDescription(wcNewBuf);
                    const newWc  = new S3ProWebCodecsFallback(
                        this._videoEl, (lv, msg) => this._log(lv, msg));
                    newWc.start(wcNewCodec, wcDesc, initBuf, () => this._cb.onState('playing')).then(ok => {
                        // Dispose old decoder AFTER new canvas is mounted so
                        // there is no frame where both canvases are visible.
                        oldWc.dispose();
                        if (ok) {
                            this._wc = newWc;
                            this._log('info', '[wc] VideoDecoder re-configured for ' + wcNewCodec);
                            this._cb.onState('buffering');
                        } else {
                            newWc.dispose();
                            this._wc = null;
                            this._log('warn', '[wc] VideoDecoder reinit failed for ' + wcNewCodec);
                            this._cb.showError('Codec change: VideoDecoder does not support ' + wcNewCodec);
                            this._cb.onState('error');
                        }
                    });
                } else {
                    this._log('warn', '[wc] in-stream codec change to unsupported codec: ' + mime);
                }
                return;
            }
            // ─────────────────────────────────────────────────────────────────

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

            // WebCodecs path — active when MSE has been replaced by VideoDecoder fallback.
            // Feed the raw moof+mdat separately so _parseMoofSamples can slice samples.
            if (this._wc) {
                const moofBuf = this._moofBuf;
                const mdatBuf = new Uint8Array(boxBuf);
                this._moofBuf = null;
                this._segmentsReceived++;
                const dtsMs = readTfdt(moofBuf);
                if (dtsMs !== null) this._cb.onDts(dtsMs, this._currentMime || '');
                this._log('info', '[fmp4] segment #' + this._segmentsReceived
                    + ' (wc) dts=' + (dtsMs !== null ? dtsMs.toFixed(0) + 'ms' : 'n/a'));
                this._wc.feedSegment(moofBuf, mdatBuf);
                return;
            }

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
                    const _prevMime = this._currentMime;
                    this._currentMime = mime;
                    this._cb.onCodecChange(mime, _prevMime);
                    // Reset per-session state — identical to the cross-family path.
                    // Stale values cause two classes of bug after a same-family switch:
                    //  1. _mseInitStallSince left non-zero → the 3 s WC-fallback timer
                    //     may fire immediately after changeType() empties the buffer.
                    //  2. _nudgeCount left at the previous session's value → first nudge
                    //     uses the 3.0 s offset, forcing Chrome to decode many delta
                    //     frames before showing the first frame and leaving < 0.1 s of
                    //     buffer ahead, causing a visible freeze.
                    this._stallSince        = 0;
                    this._mseInitStallSince = 0;
                    this._nudgeCount        = 0;
                    this._nudgedAt          = 0;
                    this._noDataSince       = 0;
                    this._segmentsReceived  = 0;
                    this._sbFallbackTried   = false;
                    this._wcFallbackTried   = false;

                    const ct = (this._videoEl && this._videoEl.currentTime > 0)
                        ? this._videoEl.currentTime : 0;

                    // Remove old-codec frames ahead of the playhead.
                    // Without this, the buffer still contains old Lo frames from
                    // [ct + duration(preSegments)] to bufferEnd, which play back
                    // between the pre-staged segments and the next live segments —
                    // causing visible alternating hi/lo frames at the switch point.
                    this._pendingQ = [{ buf: initBuf }, ...preSegments];
                    if (this._sb.buffered.length > 0) {
                        const bufEnd = this._sb.buffered.end(this._sb.buffered.length - 1);
                        if (bufEnd > ct + 0.1) {
                            // Async remove path: do NOT set timestampOffset yet.
                            // _handleUpdateEnd will set it to the live currentTime
                            // once the remove (and any trim) has completed.
                            // For HEVC, Chrome can take 2+ s to process a large IDR,
                            // advancing ct by that much between now and when
                            // _drainQueue finally fires; using the stale ct from here
                            // would place the new-codec init segment behind the live
                            // playhead, causing an immediate buffer-underrun freeze.
                            this._pendingCodecSwitch = true;
                            this._sb.remove(ct, Infinity);
                            return;
                        }
                    }
                    // No async remove needed: set timestampOffset now.
                    // sequence mode: offset was reset to 0 by changeType(); restore
                    // it to currentTime so the new segments start at the playhead.
                    try { this._sb.timestampOffset = ct; } catch (_) {}
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
            const _prevMimeRec = this._currentMime;
            this._pendingQ = [...preSegments];
            this._moofBuf = null;
            // Reset per-session state so the new MSE instance starts clean.
            // Without this, stale values from the prior codec session cause two problems:
            //  1. _sbFallbackTried/_wcFallbackTried may suppress the correct fallback
            //     on re-entry (e.g. hvc1→avc1→hvc1 second attempt).
            //  2. _nudgeCount carries the previous session's offset index.  With
            //     nudgeCount=2 the first nudge uses the 3.0 s offset (capped to
            //     bEnd−0.1 ≈ 1.9 s), forcing Chrome to software-decode ~57 HEVC delta
            //     frames from the IDR before showing the first frame and leaving only
            //     ~0.1 s of buffer ahead — causing a visible 3–5 s freeze.  A fresh
            //     nudgeCount=0 uses the gentle 0.5 s offset, leaves 1.5 s of buffer
            //     ahead, and lets Chrome reach readyState=4 immediately after decoding
            //     just 15 frames.
            this._segmentsReceived   = 0;
            this._sbFallbackTried    = false;
            this._wcFallbackTried    = false;
            this._nudgeCount         = 0;
            this._nudgedAt           = 0;
            this._stallSince         = 0;
            this._mseInitStallSince  = 0;
            this._pendingCodecSwitch = false; // cancel any in-flight same-family switch
            // Capture a freeze-frame canvas before tearing down — keeps the last
            // decoded frame visible instead of flashing black while the new
            // MediaSource is being set up.  Removed in _handleUpdateEnd once
            // readyState >= 3 (new codec is rendering).
            const _csParent = this._videoEl && this._videoEl.parentElement;
            if (_csParent && this._videoEl.readyState >= 2 && this._videoEl.videoWidth > 0) {
                try {
                    const _cv = document.createElement('canvas');
                    _cv.width  = this._videoEl.videoWidth;
                    _cv.height = this._videoEl.videoHeight;
                    _cv.getContext('2d').drawImage(this._videoEl, 0, 0);
                    _cv.style.cssText = 'position:absolute;top:0;left:0;width:100%;height:100%;' +
                        'background:#000;z-index:1;pointer-events:none;';
                    _csParent.style.position = 'relative';
                    _csParent.appendChild(_cv);
                    this._csCanvas = _cv;
                } catch (_) {}
            }
            this._teardownMS();
            // Skip 'buffering' spinner — freeze-frame canvas covers the video.
            // onState('playing') fires in _handleUpdateEnd when readyState >= 3.
            this._setupMS(mime, initBuf);
            this._cb.onCodecChange(mime, _prevMimeRec);
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
                    // If the error fired before any segment was decoded (init-segment
                    // rejection), try once with a Level-4.0 MIME string.
                    // This fixes: avc1.4d0033 (H.264 Level 5.1) — Chrome reports
                    // isTypeSupported=true but the GPU driver rejects Level > 4.0,
                    // causing an immediate SourceBuffer error on the init append.
                    if (this._segmentsReceived === 0 && !this._sbFallbackTried) {
                        const fallbackMime = _avcPermissiveMime(mime) || _hevcPermissiveMime(mime);
                        if (fallbackMime) {
                            this._sbFallbackTried = true;
                            this._log('warn', '[fmp4] init-decode error — hardware decoder rejected ' +
                                mime + '; retrying with ' + fallbackMime);
                            const savedInitBuf = this._initBuf;
                            this._teardownMS();
                            this._setupMS(fallbackMime, savedInitBuf);
                            return;
                        }
                    }
                    // WebCodecs fallback — VideoDecoder with prefer-software bypasses
                    // hardware decoder level restrictions that cause MSE to fail
                    // (e.g. H.264 Level 5.1 on GPUs capped at Level 4.0).
                    if (!this._wcFallbackTried && S3ProWebCodecsFallback.isSupported()) {
                        this._wcFallbackTried = true;
                        // Always derive the video codec from the ORIGINAL init segment,
                        // not from `mime` which may be the permissive-fallback value
                        // (e.g. avc1.4d0028) when the second SB attempt also fails.
                        // VideoDecoder with prefer-software handles any H.264 level, so
                        // using the correct original descriptor (e.g. avc1.4d0033) is
                        // more accurate and avoids a level mismatch with the avcC bytes.
                        const wcOrigMime = (this._initBuf
                            ? parseMimeFromInitSegment(this._initBuf) : null) || mime;
                        let wcCodec = (wcOrigMime.match(/codecs="([^"]+)"/) || [])[1]
                            ?.split(',').map(s => s.trim())
                            .find(c => !/^mp4a|^opus|^ac-3/.test(c));
                        // For HEVC the declared level may be wrong (e.g. L60 = Level 2.0
                        // from cameras that send incorrect codec strings).  Bump to a
                        // permissive level so Chrome's isConfigSupported check passes;
                        // VideoDecoder uses the hvcC description box for actual params.
                        if (wcCodec && /^(hvc1|hev1)/i.test(wcCodec)) {
                            const permMime = _hevcPermissiveMime('video/mp4; codecs="' + wcCodec + '"');
                            if (permMime) {
                                wcCodec = (permMime.match(/codecs="([^"]+)"/) || [])[1] || wcCodec;
                            }
                        }
                        if (wcCodec && /^avc1|^avc3|^hvc1|^hev1/i.test(wcCodec)) {
                            const wcDesc = /^hvc1|^hev1/i.test(wcCodec)
                                ? _extractHevcDescription(this._initBuf)
                                : _extractAvcDescription(this._initBuf);
                            this._log('warn', '[fmp4] MSE decode failed — starting VideoDecoder ' +
                                '(prefer-software) fallback for ' + wcCodec);
                            // ── Suppress the MEDIA_ERR_SRC_NOT_SUPPORTED that Chrome
                            // fires SYNCHRONOUSLY as the SourceBuffer error propagates up
                            // to the video element — BEFORE wc.start() resolves.
                            // clearError() sets _suppressMseError=true and registers a
                            // capture-phase blocker on the <video> element right now, so
                            // both the blocker and the player.on('error') guard are in
                            // place before the async error arrives.
                            if (this._cb.clearError) this._cb.clearError();
                            const wc = new S3ProWebCodecsFallback(
                                this._videoEl, (lv, msg) => this._log(lv, msg));
                            wc.start(wcCodec, wcDesc, this._initBuf, () => this._cb.onState('playing')).then(ok => {
                                // Dispose any pending WC canvas kept as cover during a
                                // prior WC→MSE attempt that MSE failed to handle.
                                if (this._wcDisposePending) {
                                    this._wcDisposePending.dispose();
                                    this._wcDisposePending = null;
                                }
                                if (ok) {
                                    this._wc = wc;
                                    // Skip endOfStream() — SourceBuffer is already in error
                                    // state; calling it would trigger MEDIA_ERR_SRC_NOT_SUPPORTED
                                    // on the video element and show a VJS error overlay.
                                    const savedMime = wcOrigMime; // preserve before teardown clears it
                                    this._teardownMS(true);
                                    this._currentMime = savedMime; // restore for onDts callbacks
                                    // Clear the VJS error overlay that was shown when the
                                    // video element fired its error event due to MSE failure.
                                    if (this._cb.clearError) this._cb.clearError();
                                    this._cb.onState('buffering');
                                    this._log('info', '[wc] VideoDecoder fallback active');
                                } else {
                                    wc.dispose();
                                    this._stop();
                                    this._cb.showError('Media decode error (' + mime + ')');
                                    this._cb.onState('error');
                                }
                            });
                            return;
                        }
                    }
                    // Save mime before _stop() clears this._currentMime
                    const errMime = mime;
                    this._stop();
                    this._cb.showError('Media decode error (' + errMime + ')');
                    this._cb.onState('error');
                });

                this._log('info', '[fmp4] SourceBuffer created: ' + mime);
                this._initBuf = initBuf; // saved for fallback retry
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
            const ms      = new MediaSource();
            this._ms      = ms;
            const prevUrl = this._objUrl; // may be non-null in WC→MSE transition path
            this._objUrl  = URL.createObjectURL(ms);
            this._videoEl.src = this._objUrl;
            if (prevUrl) URL.revokeObjectURL(prevUrl); // safe: video already uses new URL
            ms.addEventListener('sourceopen', () => {
                if (this._ms !== ms) return;  // stale: a newer MS replaced this one
                this._msOpen = true;
                this._doAddSourceBuffer(ms, mime, initBuf);
            }, { once: true });
        }

        _teardownMS(skipEndOfStream = false) {
            if (this._sb && this._onUpdateEnd) {
                try { this._sb.removeEventListener('updateend', this._onUpdateEnd); } catch (_) {}
            }
            if (!skipEndOfStream) {
                try {
                    if (this._ms && this._ms.readyState === 'open') this._ms.endOfStream();
                } catch (_) {}
            }
            // When skipEndOfStream is true (WebCodecs fallback path) we deliberately
            // keep the blob URL alive.  Revoking it would make the video element's src
            // invalid, causing the browser to fire another MEDIA_ERR_SRC_NOT_SUPPORTED
            // event which re-shows the VJS error overlay even after clearError().
            // The blob URL (and the abandoned MediaSource) are reclaimed by GC once the
            // video element is reset in a subsequent _stop() → _teardownMS() call.
            if (this._objUrl && !skipEndOfStream) { URL.revokeObjectURL(this._objUrl); this._objUrl = null; }
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
                const bStart  = sb.buffered.start(0);
                const bEnd    = sb.buffered.end(sb.buffered.length - 1);
                const bufDur  = (bEnd - bStart).toFixed(2);
                this._log('info', '[fmp4] updateend ct=' + v.currentTime.toFixed(2)
                    + ' buffered=[' + bStart.toFixed(2) + '..' + bEnd.toFixed(2) + ']'
                    + ' bufDur=' + bufDur + 's'
                    + ' readyState=' + v.readyState
                    + ' paused=' + v.paused
                    + ' nudges=' + this._nudgeCount);
            } else {
                this._log('info', '[fmp4] updateend — buffer empty, ct=' + v.currentTime.toFixed(2)
                    + ' readyState=' + v.readyState);
            }

            // Trim old frames: keep at most 5 s behind currentTime.
            // Begin trimming as soon as there is > 6 s of history (guard = bStart+1).
            // Without this, the SourceBuffer accumulates the entire stream from t=0.
            // For HEVC, Chrome's growing decode ring causes a multi-second delay
            // in updateend after each large IDR, letting _pendingQ grow to 60+ items
            // (~7 s of content) before the chain resumes.
            // Trim fires at most once per ~1 s of playback (negligible overhead).
            if (sb.buffered.length > 0) {
                try {
                    const trimTo = v.currentTime - 5;
                    const bStart = sb.buffered.start(0);
                    if (trimTo > bStart + 1) {
                        sb.remove(bStart, trimTo);
                        return;
                    }
                } catch (_) {}
            }

            // Autoplay: once buffered data is available, resume playback.
            // Call v.play() directly (not vp.play()) — calling the VJS player's
            // play() can trigger another handleSource invocation and orphan the
            // MediaSource.  VJS will pick up the native 'play'/'playing' events.
            //
            // IMPORTANT: seek ct to bStart unconditionally (not just when paused).
            // VJS calls play() very early, before any data arrives, so v.paused
            // is already false on the first updateend.  In sequence mode the
            // browser assigns timestamps starting at ~0.09 s (not 0.00 s), so
            // ct=0.00 < bStart means ct is outside the buffered range, which
            // keeps readyState at 1 forever even though the SourceBuffer is full.
            if (sb.buffered.length > 0) {
                const bStart = sb.buffered.start(0);
                const bEnd   = sb.buffered.end(sb.buffered.length - 1);
                const bufDur = bEnd - bStart;

                // Periodic nudge: when buffer has ≥2 s of data but readyState is
                // still < 3 (HAVE_FUTURE_DATA), seek currentTime forward so Chrome
                // re-evaluates the decode position.  A tiny offset (0.001) often
                // snaps back because the first decodable IDR may be at ≥0.033 s in
                // sequence-mode.  Use a generous starting offset (0.5 s) and retry
                // with increasing offsets every 3 s until playback starts.
                if (bufDur >= 2.0 && v.readyState < 3 && !this._httpEnded) {
                    const nowMs = performance.now();
                    const msSinceNudge = nowMs - this._nudgedAt;
                    // First nudge fires immediately; subsequent nudges every 3 s.
                    if (this._nudgedAt === 0 || msSinceNudge >= 3000) {
                        this._nudgeCount++;
                        this._nudgedAt = nowMs;
                        // Offsets: 0.5 s, 1.5 s, 3.0 s
                        const offsets = [0.5, 1.5, 3.0];
                        const rawOff  = offsets[Math.min(this._nudgeCount - 1, offsets.length - 1)];
                        const ct      = v.currentTime;
                        // Two cases:
                        //  a) Initial-decode stall (HEVC, ct near bStart): seek
                        //     FORWARD from bStart to force Chrome to commit the
                        //     buffered range.
                        //  b) Active-playback buffer underrun (ct >> bStart):
                        //     seek BACKWARD from ct to give the decoder a tiny
                        //     run-up.  Seeking all the way back to bStart would
                        //     jump the user back seconds in a VOD stream.
                        const nudgeTarget = ct > bStart + 1.0
                            ? Math.max(ct - rawOff, bStart + 0.1)
                            : Math.min(bStart + rawOff, bEnd - 0.1);
                        // Also log v.buffered to detect divergence from sb.buffered
                        const vbStr = v.buffered.length > 0
                            ? v.buffered.start(0).toFixed(4) + '..' + v.buffered.end(v.buffered.length - 1).toFixed(4)
                            : 'empty';
                        this._log('info', '[fmp4] NUDGE #' + this._nudgeCount
                            + ': bufDur=' + bufDur.toFixed(2)
                            + 's readyState=' + v.readyState
                            + ' sb.buf=[' + bStart.toFixed(6) + '..' + bEnd.toFixed(4) + ']'
                            + ' v.buf=[' + vbStr + ']'
                            + ' → ct=' + nudgeTarget.toFixed(4));
                        v.currentTime = nudgeTarget;
                    }
                }

                if (v.currentTime < bStart) {
                    v.currentTime = bStart;
                }
                // Guard: do NOT call play() after a VOD/replay stream has finished.
                // Without this, the trim's updateend chain resumes after v.pause(),
                // and Chrome reacts to play()-on-ended by seeking back to position 0.
                if (v.paused && !this._httpEnded) {
                    const playPromise = v.play();
                    if (playPromise && playPromise.catch) playPromise.catch(() => {});
                }
            }

            // Notify playing state once the element has enough data.
            // VJS 'playing' event may be delayed on some configs — emit here so
            // the loading overlay is hidden as soon as the decoder can start.
            if (sb.buffered.length > 0 && v.readyState >= 3) {
                // Complete WC→MSE transition: frozen canvas cover is no longer needed.
                if (this._wcDisposePending) {
                    this._wcDisposePending.dispose();
                    this._wcDisposePending = null;
                    if (this._cb.disableMseSuppress) this._cb.disableMseSuppress();
                    this._log('info', '[wc→mse] MSE playing, WebCodecs canvas removed');
                }
                // Complete codec-switch freeze-frame: new video is rendering.
                if (this._csCanvas) {
                    this._csCanvas.remove();
                    this._csCanvas = null;
                    this._log('info', '[fmp4] codec-switch freeze-frame canvas removed');
                }
                this._cb.onState('playing');
            }

            // After a same-family (changeType) codec switch the timestampOffset
            // was intentionally NOT set during the async remove phase.  Now that
            // all remove/trim operations have completed, set it to the live
            // currentTime so the new-codec init segment lands at the playhead.
            // Using the ct from the switch call would be stale: for HEVC, Chrome
            // takes 2+ s to process a large IDR, advancing ct by that amount
            // before _drainQueue fires, which would place the init segment behind
            // the live position and cause an immediate buffer-underrun freeze.
            if (this._pendingCodecSwitch) {
                this._pendingCodecSwitch = false;
                try { this._sb.timestampOffset = v.currentTime; } catch (_) {}
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
                // Pause the video element when it fires 'ended' to prevent the
                // browser or VJS (autoplay:'any') from calling play() again and
                // seeking back to currentTime=0, which would replay the buffer.
                const v = this._videoEl;
                if (v) v.addEventListener('ended', () => { v.pause(); }, { once: true });
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
            if (this._wc)              { this._wc.dispose();              this._wc = null;              }
            if (this._wcDisposePending){ this._wcDisposePending.dispose(); this._wcDisposePending = null; }
            if (this._csCanvas)        { this._csCanvas.remove();          this._csCanvas = null;         }
            if (this._stagingTimer) { clearTimeout(this._stagingTimer); this._stagingTimer = null; }
            this._pendingQ          = [];
            this._moofBuf           = null;
            this._ftypBuf           = null;
            this._stagingCodec      = null;
            this._moovReceived      = false;
            this._httpEnded         = false;
            this._noDataSince       = 0;
            this._waitLogSince      = 0;
            this._segmentsReceived  = 0;
            this._sbFallbackTried   = false;
            this._wcFallbackTried   = false;
            this._mseInitStallSince = 0;
            this._pendingCodecSwitch = false;
            this._initBuf           = null;
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

            // Proactive HEVC MSE → WebCodecs fallback.
            // Chrome's software HEVC decoder accepts the SourceBuffer but is too
            // slow to produce a committed buffered range, leaving readyState=1 for
            // 15-20 s.  If sb.buffered duration stays near zero for 3 s after data
            // started arriving, switch to WebCodecs which renders the first frame
            // on the very next keyframe (typically < 1 s after start).
            if (this._sb && !this._wc && !this._wcFallbackTried &&
                    this._segmentsReceived > 0 && rs <= 2 && !this._httpEnded &&
                    S3ProWebCodecsFallback.isSupported()) {
                // rs <= 2 covers two stall patterns:
                //  (a) Init stall  — readyState never left 1; sbDur near 0;
                //      HEVC decoder never committed a buffered range.
                //  (b) Post-play stall — readyState briefly reached 4 (one frame
                //      shown), then fell to 2 (HAVE_CURRENT_DATA); sbDur > 0;
                //      HEVC software decoder decoded first IDR but is too slow
                //      to keep up with the stream rate.
                // In both cases a 3-second timeout without rs reaching 3 is a
                // reliable signal that MSE cannot play this stream in real-time.
                if (!this._mseInitStallSince) this._mseInitStallSince = performance.now();
                    if (performance.now() - this._mseInitStallSince > 3000) {
                        this._mseInitStallSince = 0;
                        this._wcFallbackTried   = true;
                        this._log('warn', '[fmp4] HEVC MSE init stall 3 s — switching to WebCodecs');
                        const wcOrigMime = (this._initBuf
                            ? parseMimeFromInitSegment(this._initBuf) : null) || this._currentMime;
                        const wcCodec = (wcOrigMime.match(/codecs="([^"]+)"/) || [])[1]
                            ?.split(',').map(s => s.trim())
                            .find(c => !/^mp4a|^opus|^ac-3/.test(c));
                        if (wcCodec && /^avc1|^avc3|^hvc1|^hev1/i.test(wcCodec)) {
                            const wcDesc = /^hvc1|^hev1/i.test(wcCodec)
                                ? _extractHevcDescription(this._initBuf)
                                : _extractAvcDescription(this._initBuf);
                            if (this._cb.clearError) this._cb.clearError();
                            const wc = new S3ProWebCodecsFallback(
                                this._videoEl, (lv, msg) => this._log(lv, msg));
                            wc.start(wcCodec, wcDesc, this._initBuf,
                                    () => this._cb.onState('playing')).then(ok => {
                                if (ok) {
                                    this._wc = wc;
                                    const savedMime = wcOrigMime;
                                    this._teardownMS(true);
                                    this._currentMime = savedMime;
                                    if (this._cb.clearError) this._cb.clearError();
                                    this._cb.onState('buffering');
                                    this._log('info', '[wc] VideoDecoder active (HEVC MSE stall)');
                                } else {
                                    wc.dispose();
                                    this._log('warn', '[wc] VideoDecoder fallback failed — MSE continues');
                                }
                            });
                        }
                    }
            } else {
                this._mseInitStallSince = 0;
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

            // Stall detection & nudge.
            // v.buffered may be empty while sb.buffered already has data because
            // Chrome (MSE sequence-mode) doesn't mark a GOP as buffered until the
            // next IDR arrives.  Fall back to sb.buffered so that stall detection
            // keeps running even while the first GOP is accumulating.
            const _stBuf = v.buffered.length > 0 ? v.buffered
                         : (this._sb && this._sb.buffered.length > 0 ? this._sb.buffered : v.buffered);
            if (_stBuf.length > 0) {
                const bStart = _stBuf.start(0);
                const bEnd   = _stBuf.end(_stBuf.length - 1);
                // ct past buffer end (sequence-mode quirk: readyState may stay 4
                // even though decoder has no frames left) — reset to buffer start.
                // Skip this correction when httpEnded: the stream legitimately
                // finished and the video should stay paused at the last frame.
                if (v.currentTime > bEnd + 0.1) {
                    if (!this._httpEnded) {
                        if (!this._stallSince) this._stallSince = performance.now();
                        if (performance.now() - this._stallSince > 300) {
                            this._log('info', '[fmp4] stall-skip ct=' + v.currentTime.toFixed(2) +
                                ' past bEnd=' + bEnd.toFixed(2) + ', reset to bStart=' + bStart.toFixed(2));
                            v.currentTime    = bStart;
                            this._stallSince = 0;
                            if (v.paused) v.play().catch(() => {});
                        }
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
                            if (v.paused && !this._httpEnded) v.play().catch(() => {});
                        }
                    } else if (stallMs > 800) {
                        // ct within buffer but decoder stalled — nudge forward
                        const nudge = Math.min(v.currentTime + 0.15, Math.max(bEnd - 0.05, bStart));
                        this._log('info', '[fmp4] stall-nudge ' + v.currentTime.toFixed(2) +
                            ' -> ' + nudge.toFixed(2));
                        v.currentTime    = nudge;
                        this._stallSince = 0;
                        if (v.paused && !this._httpEnded) v.play().catch(() => {});
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
    // § 4b — S3ProWebCodecsFallback
    //
    // Activated by S3ProFmp4Handler when MSE SourceBuffer fails to decode
    // (typically because the GPU driver rejects a codec level that
    // MediaSource.isTypeSupported() falsely reported as supported).
    //
    // Uses VideoDecoder with hardwareAcceleration:'prefer-software' — the same
    // software fallback path that native <video> uses — and renders decoded
    // VideoFrames to a Canvas overlay placed on top of the <video> element.
    //
    // Audio: MSE is torn down after this class starts, so audio goes silent.
    // For most surveillance / monitoring use-cases this is acceptable.
    // =========================================================================
    class S3ProWebCodecsFallback {

        /** Feature-detect: returns true if WebCodecs VideoDecoder is available. */
        static isSupported() {
            return typeof VideoDecoder      !== 'undefined' &&
                   typeof EncodedVideoChunk !== 'undefined' &&
                   typeof VideoDecoder.isConfigSupported === 'function';
        }

        /**
         * @param {HTMLVideoElement} videoEl  — the player's <video> element
         * @param {Function}         log      — (level, msg) logger
         */
        constructor(videoEl, log) {
            this._videoEl      = videoEl;
            this._log          = log;
            this._decoder      = null;
            this._canvas       = null;
            this._ctx          = null;
            this._running      = false;
            this._videoTrackId = 1; // video track ID parsed from moov, default 1
            this._codec        = '';    // codec string passed to start(), used for NAL detection
            this._hasKeyFrame  = false; // true once the first key frame has been sent to the decoder
            // Frame scheduling — frames are queued and rendered via requestAnimationFrame
            // so that each frame is displayed at its correct stream timestamp rather than
            // all at once (which causes a burst-then-freeze artefact).
            this._frameQueue   = []; // pending VideoFrame objects, in timestamp order
            this._rafId        = null; // pending requestAnimationFrame id
            this._epochWall    = null; // performance.now() when the first frame arrived (ms)
            this._epochTs      = null; // VideoDecoder timestamp of the first frame (µs)
            this._description  = null; // saved codec description for decoder recovery

            this._onFirstFrame    = null;
            this._firstFrameFired = false;
        }

        /**
         * Configure the VideoDecoder and mount a Canvas overlay.
         *
         * @param {string}                codec        e.g. 'avc1.4d0033'
         * @param {Uint8Array|null}        description  avcC payload from _extractAvcDescription()
         * @param {ArrayBuffer|Uint8Array|null} initBuf moov init segment (for track ID extraction)
         * @returns {Promise<boolean>} true on success, false if unsupported
         */
        async start(codec, description, initBuf = null, onFirstFrame = null) {
            // Identify video track ID so feedSegment can ignore audio traf entries
            // in multiplexed (video+audio) FMP4 segments.
            this._videoTrackId = _findVideoTrackId(initBuf);
            this._codec        = codec;
            this._description  = description || null; // saved for decoder error recovery
            this._hasKeyFrame  = false;
            const config = {
                codec,
                hardwareAcceleration: 'prefer-software',
            };
            if (description && description.byteLength > 0) {
                // VideoDecoder.configure() expects an ArrayBuffer (BufferSource)
                config.description = description.buffer.slice(
                    description.byteOffset,
                    description.byteOffset + description.byteLength
                );
            }

            try {
                const support = await VideoDecoder.isConfigSupported(config);
                if (!support.supported) {
                    this._log('warn', '[wc] VideoDecoder (prefer-software) does not support: ' + codec);
                    return false;
                }
            } catch (e) {
                this._log('warn', '[wc] isConfigSupported error: ' + e.message);
                return false;
            }

            this._decoder = new VideoDecoder({
                output: frame => this._onFrame(frame),
                error:  err   => {
                    this._log('error', '[wc] decoder error: ' + err.message);
                    // VideoDecoder enters 'closed' on any error — frames stop.
                    // Clear the reference and reset the keyframe gate so that
                    // feedSegment will recreate the decoder on the next IDR.
                    this._decoder     = null;
                    this._hasKeyFrame = false;
                },
            });
            this._decoder.configure(config);

            // Mount canvas overlay on top of the video element
            const parent = this._videoEl && this._videoEl.parentElement;
            if (parent) {
                this._canvas = document.createElement('canvas');
                this._canvas.style.cssText =
                    'position:absolute;top:0;left:0;width:100%;height:100%;' +
                    'background:transparent;z-index:1;pointer-events:none;';
                // Transparent background: before the first WC frame is drawn the
                // underlying <video> element (showing the last MSE frame at
                // readyState=2) remains visible, avoiding a black flash.
                parent.style.position = 'relative';
                parent.appendChild(this._canvas);
                this._ctx = this._canvas.getContext('2d');
            }

            this._onFirstFrame    = onFirstFrame || null;
            this._firstFrameFired = false;
            this._running         = true;
            this._log('info', '[wc] VideoDecoder started (prefer-software), codec=' + codec);
            return true;
        }

        /**
         * Parse samples from a moof+mdat pair and feed them to the VideoDecoder.
         * @param {Uint8Array} moofBuf
         * @param {Uint8Array} mdatBuf
         */
        feedSegment(moofBuf, mdatBuf) {
            if (!this._running) return;
            // Decoder may be null after an error — try to recreate it.
            if (!this._decoder) {
                const cfg = { codec: this._codec, hardwareAcceleration: 'prefer-software' };
                const desc = this._description;
                if (desc && desc.byteLength > 0) {
                    cfg.description = desc.buffer.slice(
                        desc.byteOffset, desc.byteOffset + desc.byteLength);
                }
                try {
                    const dec = new VideoDecoder({
                        output: frame => this._onFrame(frame),
                        error:  err   => {
                            this._log('error', '[wc] decoder error (recovery): ' + err.message);
                            this._decoder     = null;
                            this._hasKeyFrame = false;
                        },
                    });
                    dec.configure(cfg);
                    this._decoder = dec;
                    this._log('info', '[wc] VideoDecoder recreated after error, waiting for next keyframe');
                } catch (e) {
                    this._log('warn', '[wc] decoder recreation failed: ' + e.message);
                    return;
                }
            }
            if (this._decoder.state === 'closed') return;
            const samples = _parseMoofSamples(moofBuf, mdatBuf, this._videoTrackId);
            for (const s of samples) {
                // Cross-check keyframe status from the NAL unit type.
                // Some HEVC encoders do not set trun.first_sample_flags (bit 0x004)
                // and leave default_sample_flags=0x00010000 (non-sync) in tfhd,
                // causing _parseMoofSamples to mark the IDR frame as delta.
                // Feeding a delta chunk to a freshly-configured VideoDecoder closes
                // the decoder with DataError — all subsequent calls silently return.
                const isKey = s.isKey || this._isIrapFrame(s.data);

                // Hold off until the decoder has received at least one key frame.
                if (!this._hasKeyFrame) {
                    if (!isKey) continue; // skip leading delta frames
                    this._hasKeyFrame = true;
                }

                try {
                    this._decoder.decode(new EncodedVideoChunk({
                        type:      isKey ? 'key' : 'delta',
                        timestamp: s.timestamp,
                        duration:  s.duration,
                        data:      s.data,
                    }));
                } catch (e) {
                    this._log('warn', '[wc] decode chunk error: ' + e.message);
                }
            }
        }

        /**
         * Detect whether the first NAL unit in an AVCC/HVCC sample is an IRAP
         * (Intra Random Access Point) by reading the NAL unit type from the header.
         * This is used to override isKey when sample_flags are misleading.
         *
         * @param {Uint8Array} data  raw sample bytes (4-byte length prefix + NAL data)
         * @returns {boolean}
         */
        _isIrapFrame(data) {
            if (!data || data.byteLength < 5) return false;
            if (/^hvc1|^hev1/i.test(this._codec)) {
                // HEVC NAL header: byte0 = forbidden(1) | nal_unit_type(6) | layer_id high bit
                // nal_unit_type = (byte0 >> 1) & 0x3F
                // IRAP types 16-23: BLA_W_LP, BLA_W_RADL, BLA_N_LP, IDR_W_RADL,
                //                   IDR_N_LP, CRA_NUT, RSV_IRAP_VCL22, RSV_IRAP_VCL23
                const nalType = (data[4] >> 1) & 0x3F;
                return nalType >= 16 && nalType <= 23;
            } else {
                // AVC NAL header: byte0 = forbidden(1) | nal_ref_idc(2) | nal_unit_type(5)
                // IDR slice = 5
                const nalType = data[4] & 0x1F;
                return nalType === 5;
            }
        }

        /** Called by VideoDecoder for each decoded frame. */
        _onFrame(frame) {
            if (!this._running) { frame.close(); return; }

            // Establish wall-clock epoch on the first frame so subsequent frames
            // can be scheduled relative to real time.
            if (this._epochTs === null) {
                this._epochWall = performance.now();
                this._epochTs   = frame.timestamp; // µs
            }

            // Safety cap: if the decoder is running far ahead of the display (e.g.
            // a large segment was decoded all at once), discard old frames to keep
            // memory bounded and shift the epoch so rendering catches up quickly.
            const MAX_QUEUE = 12;
            if (this._frameQueue.length >= MAX_QUEUE) {
                // Re-anchor epoch to now so all queued frames are treated as "due".
                this._epochWall = performance.now()
                    - (this._frameQueue[0].timestamp - this._epochTs) / 1000;
                // Drop the oldest half to avoid a prolonged display-burst.
                const keep = Math.floor(MAX_QUEUE / 2);
                while (this._frameQueue.length > keep) {
                    this._frameQueue.shift().close();
                }
            }

            this._frameQueue.push(frame);
            if (!this._rafId) {
                this._rafId = requestAnimationFrame(() => this._renderLoop());
            }

            if (this._onFirstFrame && !this._firstFrameFired) {
                this._firstFrameFired = true;
                this._onFirstFrame();
            }
        }

        /**
         * rAF-driven render loop: draw the latest frame whose scheduled wall-clock
         * time has arrived, discard earlier frames, reschedule if more are pending.
         */
        _renderLoop() {
            this._rafId = null;
            if (!this._running) {
                while (this._frameQueue.length) this._frameQueue.shift().close();
                return;
            }

            const now = performance.now();
            let   frameToRender = null;

            // Walk the queue: consume all frames whose scheduled time ≤ now,
            // keeping only the most recent one to render (drop skipped frames).
            while (this._frameQueue.length) {
                const f = this._frameQueue[0];
                // Scheduled wall-clock time for this frame (epoch + offset from first frame)
                const scheduled = this._epochWall + (f.timestamp - this._epochTs) / 1000;
                if (scheduled <= now + 2) { // 2 ms lookahead tolerance
                    if (frameToRender) frameToRender.close(); // release skipped frame
                    frameToRender = this._frameQueue.shift();
                } else {
                    break; // this frame is still in the future
                }
            }

            if (frameToRender) {
                if (this._ctx && this._canvas) {
                    const w = frameToRender.displayWidth, h = frameToRender.displayHeight;
                    if (this._canvas.width !== w || this._canvas.height !== h) {
                        this._canvas.width  = w;
                        this._canvas.height = h;
                    }
                    this._ctx.drawImage(frameToRender, 0, 0);
                }
                frameToRender.close();
            }

            // Reschedule if there are frames still waiting in the queue.
            if (this._frameQueue.length) {
                this._rafId = requestAnimationFrame(() => this._renderLoop());
            }
        }

        /** Tear down the decoder and remove the canvas overlay. */
        dispose() {
            this._running = false;
            if (this._rafId) { cancelAnimationFrame(this._rafId); this._rafId = null; }
            while (this._frameQueue.length) this._frameQueue.shift().close();
            this._frameQueue = [];
            if (this._decoder && this._decoder.state !== 'closed') {
                try { this._decoder.close(); } catch (_) {}
            }
            this._decoder = null;
            if (this._canvas && this._canvas.parentElement) {
                this._canvas.parentElement.removeChild(this._canvas);
            }
            this._canvas = null;
            this._ctx    = null;
        }
    } // end S3ProWebCodecsFallback

    // =========================================================================
    // § 5 — SourceHandler registration (once per Html5Tech class)
    // =========================================================================

    // Module-level debug flag — controls _makeFallbackCb console output.
    // Enabled when any S3ProPlugin instance has _debugLog=true (see constructor).
    let _globalDebug = false;

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
                if (source.type === 'application/x-fmp4live')    return 'probably';
                if (/^wss?:\/\//i.test(source.src))              return 'probably';
                if (/\.live2\.mp4(\?.*)?$/.test(source.src))    return 'maybe';
                return '';
            },
            handleSource(source, tech, options) {
                const vjsPlayer = tech.player_;
                const plugin = _pendingPlugin
                    || (_pluginRegistry && vjsPlayer ? _pluginRegistry.get(vjsPlayer) : null);

                // Guard: VJS sometimes calls handleSource a second time when
                // player.play() is invoked on a freshly-set src (e.g. because
                // readyState is still 0 and VJS retries the source load).
                // Without this guard a second MediaSource (MS2) is created and
                // attached to the video element while the original handler keeps
                // appending to MS1 — video element sees empty MS2 → readyState=1
                // forever even though sb.buffered grows to 30 s.
                if (!_pendingPlugin && plugin && plugin._handler && plugin._handler.running) {
                    return plugin._handler;
                }

                const cb      = plugin ? plugin._makeHandlerCb() : _makeFallbackCb();
                const handler = new S3ProFmp4Handler(source, tech, options, cb);
                if (plugin) plugin._handler = handler;
                return handler;
            },
        }, 0);
    }

    function _makeFallbackCb() {
        return {
            log:               (lv, msg) => { if (_globalDebug) (console[lv] || console.log)('[s3pro]', msg); },
            showError:         (msg) => console.error('[s3pro] fatal:', msg),
            disableMseSuppress:() => {},
            onState:           () => {},
            onDts:             () => {},
            onCodecChange:     () => {},
            messages:          null,
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
            this._player          = player;
            this._opts            = opts || {};
            this._handler         = null;
            this._state           = 'idle';
            this._listeners       = {};
            this._pollTimer       = null;
            // Set to true when the WebCodecs canvas fallback becomes active so that
            // the async MEDIA_ERR_SRC_NOT_SUPPORTED from the abandoned SourceBuffer
            // is suppressed in player.on('error').  Survives handler disposal.
            this._suppressMseError = false;

            // Monotonic DTS tracking — keeps the emitted 'dts' value always
            // increasing regardless of codec switches or tfdt resets.
            // Reset only on play(new url) or stop().
            this._dts = new DtsTracker();

            // When true, all 'log' events are also printed to the browser console.
            // Controlled by opts.debug at construction time and setDebug() at runtime.
            this._debugLog = !!(opts.debug);
            if (this._debugLog) _globalDebug = true;

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
                // While the WebCodecs fallback canvas is active, the abandoned
                // MediaSource may fire a delayed MEDIA_ERR_SRC_NOT_SUPPORTED on
                // the video element.  Suppress it — the canvas overlay is
                // rendering correctly and the VJS error overlay must stay hidden.
                //
                // Use this._suppressMseError (a plugin-level flag) rather than
                // this._handler.webCodecsActive because this._handler may already
                // be null by the time the async error fires (e.g. VJS called
                // handleSource again and replaced the handler reference).
                if (this._suppressMseError) {
                    try { player.error(null); } catch (_) {}
                    try { player.removeClass('vjs-error'); } catch (_) {}
                    _closeVjsErrorDisplay(player);
                    setTimeout(() => {
                        if (this._suppressMseError) {
                            try { player.error(null); } catch (_) {}
                            try { player.removeClass('vjs-error'); } catch (_) {}
                            _closeVjsErrorDisplay(player);
                        }
                    }, 0);
                    return;
                }
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
            this._suppressMseError = false; // reset for new stream
            // Reset monotonic DTS so the new stream starts from 0.
            this._dts.reset();
            this._player.pause();
            this._stopHandler();
            // Clear any VJS error overlay from the previous stream without
            // triggering a full reset (which can race with the new handler's
            // eager MediaSource creation and delay sourceopen by ~15 s).
            try { this._player.error(null); } catch (_) {}
            this._setState('connecting');

            let type;
            if (/\.m3u8(\?.*)?$/i.test(url)) {
                // type = 'application/x-mpegURL';
                type = "application/vnd.apple.mpegurl";
            } else if (/^wss?:\/\//i.test(url) || /\.live2\.mp4(\?.*)?$/i.test(url)) {
                type = 'application/x-fmp4live';  // plugin
            } else {
                type = 'video/mp4';               // .live.mp4, .mp4, ... → VJS native
            }

            _pendingPlugin = this;
            this._player.src({ src: url, type });
            _pendingPlugin = null;
            // Do NOT call player.play() here: on a freshly-set src with
            // readyState=0, VJS re-invokes the source handler to reload,
            // creating an orphaned duplicate MediaSource (see handleSource guard).
            // Autoplay is triggered from _handleUpdateEnd once the first data
            // is buffered, at which point the video element is ready.
        }

        stop() {
            this._emit('log', 'info', '=== stop ===');
            this._suppressMseError = false;
            this._dts.reset();
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

        /**
         * Enable or disable console log output at runtime.
         * Equivalent to passing { debug: true } in the constructor options.
         *
         *   sp.setDebug(true);   // turn on
         *   sp.setDebug(false);  // turn off
         */
        setDebug(enable) {
            this._debugLog = !!enable;
            if (enable) _globalDebug = true;
            return this; // chainable
        }

        get debug() { return this._debugLog; }

        getState() {
            const h = this._handler;
            const v = this.videoEl;
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

        // -- New clean API: direct player-state getters -------------------------

        /** The underlying &lt;video&gt; element (null before VJS initialises). */
        get videoEl() {
            const el = this._player.el && this._player.el();
            return el ? el.querySelector('video') : null;
        }

        /** Monotonic DTS elapsed ms since play() (interpolates between packets). */
        get elapsedMs() { return this._dts.elapsed; }

        /** Current plugin state string. */
        get state() { return this._state; }

        /** Raw MIME string of the active codec (e.g. 'video/mp4; codecs="avc1...."'). */
        get codec() { return this._handler ? this._handler.currentMime : ''; }

        /** Total buffered seconds in the video element. */
        get bufferLen() {
            const v = this.videoEl;
            if (!v || !v.buffered) return 0;
            let n = 0;
            for (let i = 0; i < v.buffered.length; i++) n += v.buffered.end(i) - v.buffered.start(i);
            return n;
        }

        /** Video frame dimensions, or {width:0, height:0} when no frame yet. */
        get videoSize() {
            const v = this.videoEl;
            return v ? { width: v.videoWidth, height: v.videoHeight } : { width: 0, height: 0 };
        }

        /** Current volume [0..1]. */
        get volume() { const v = this.videoEl; return v ? v.volume : 1; }
        set volume(val) { const v = this.videoEl; if (v) { v.volume = val; v.muted = (val === 0); } }

        /** Muted state. */
        get muted() { const v = this.videoEl; return v ? v.muted : false; }
        set muted(val) { const v = this.videoEl; if (v) v.muted = val; }

        /** True when the video element is paused or not yet initialised. */
        get paused() { const v = this.videoEl; return v ? v.paused : true; }

        /**
         * Capture the current video frame and return a PNG data URL, or null if
         * no frame is available. Works with both MSE and WebCodecs paths.
         */
        snapshot() {
            const v = this.videoEl;
            if (!v || !v.videoWidth) return null;
            const c = document.createElement('canvas');
            c.width  = v.videoWidth;
            c.height = v.videoHeight;
            try {
                c.getContext('2d').drawImage(v, 0, 0);
                return c.toDataURL('image/png');
            } catch (_) { return null; }
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
            // When debug mode is on, mirror every log message to the browser console.
            if (event === 'log' && this._debugLog) {
                const [level, msg] = args;
                (console[level] || console.log)('[s3pro]', msg);
            }
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
                log:        (level, msg) => this._emit('log', level, msg),
                showError:  (msg) => { this._setState('error'); this._emit('error', msg); },
                clearError: () => {
                    // Dismiss the VJS error overlay without changing plugin state.
                    // Called when a fallback (e.g. WebCodecs) takes over after MSE fails.

                    // Raise plugin-level flag FIRST so player.on('error') can suppress
                    // the incoming async MEDIA_ERR_SRC_NOT_SUPPORTED even if
                    // this._handler is already null when that event fires.
                    this._suppressMseError = true;

                    try { this._player.error(null); } catch (_) {}
                    try { this._player.removeClass('vjs-error'); } catch (_) {}

                    // Belt-and-suspenders: also install a one-shot CAPTURE-PHASE
                    // interceptor on the raw <video> element.  Capture fires before
                    // VJS's bubble-phase Tech listener, so stopImmediatePropagation()
                    // prevents VJS from ever calling player.error() for this event.
                    try {
                        const _self = this;
                        const tech  = this._player.tech && this._player.tech(false);
                        const vEl   = (tech && typeof tech.el === 'function')
                                    ? tech.el()
                                    : this._player.el().querySelector('video');
                        if (vEl) {
                            const blocker = (e) => {
                                vEl.removeEventListener('error', blocker, true);
                                if (!_self._suppressMseError) return;
                                const code = e.target && e.target.error && e.target.error.code;
                                if (code === 4 /* MEDIA_ERR_SRC_NOT_SUPPORTED */) {
                                    e.stopImmediatePropagation();
                                    try { _self._player.error(null); } catch (_) {}
                                    try { _self._player.removeClass('vjs-error'); } catch (_) {}
                                    _closeVjsErrorDisplay(_self._player);
                                }
                            };
                            vEl.addEventListener('error', blocker, true);
                        }
                    } catch (_) {}

                    if (this._state === 'error') this._setState('buffering');
                },
                disableMseSuppress: () => { this._suppressMseError = false; },
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
                onDts: (dtsMs, codec) => {
                    const mono = this._dts.feed(dtsMs);
                    this._emit('dts', mono, codec);
                },
                onCodecChange: (newMime, oldMime) => {
                    this._emit('codecchange', newMime, oldMime);
                },
                messages: this._opts.messages || null,
            };
        }

        /** 200 ms poll: emit timeupdate, drive stall detection. */
        _poll() {
            const v = this.videoEl;
            if (!v) return;

            const h      = this._handler;
            const mime   = h ? h.currentMime : '';
            let   bufLen = 0;
            if (v.buffered) {
                for (let i = 0; i < v.buffered.length; i++)
                    bufLen += v.buffered.end(i) - v.buffered.start(i);
            }
            this._emit('timeupdate', this._dts.elapsed, mime, bufLen,
                       { width: v.videoWidth, height: v.videoHeight });

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
