// =============================================================================
// pc-hud.js — Player HUD: log panel, codec/time bar, overlays, freeze frame.
// Called by initPlayerControls(). Populates state with overlay helpers and DOM refs.
// =============================================================================

function initPlayerHud(player, state) {
    var logEl          = document.getElementById('log');
    var infoEl         = document.getElementById('info-text');
    var timeEl         = document.getElementById('time-display');
    var errOverlay     = document.getElementById('player-error');
    var errMsgEl       = document.getElementById('player-error-msg');
    var loadingOverlay = document.getElementById('player-loading');
    var freezeCanvas   = document.getElementById('player-freeze');

    // Expose DOM refs so other modules can show error messages
    state.errOverlay = errOverlay;
    state.errMsgEl   = errMsgEl;

    // ── Codec name helper ─────────────────────────────────────────────────────
    function _friendlyCodec(mime) {
        if (!mime) return '';
        var m = mime.match(/codecs="([^"]+)"/);
        var src = m ? m[1] : mime;
        return src.split(',').map(function (c) {
            c = c.trim();
            if (/^avc1|^avc3/i.test(c)) return 'H.264';
            if (/^hvc1|^hev1/i.test(c)) return 'H.265';
            if (/^av01/i.test(c))       return 'AV1';
            if (/^vp09/i.test(c))       return 'VP9';
            if (/^vp08/i.test(c))       return 'VP8';
            if (/^mp4a\.40/i.test(c))   return 'AAC';
            if (/^opus/i.test(c))       return 'Opus';
            return c;
        }).join(' + ');
    }
    state.friendlyCodec = _friendlyCodec;

    // ── Overlay helpers ───────────────────────────────────────────────────────
    function showStoppedOverlay() {
        var el = document.getElementById('player-stopped');
        if (el) el.classList.add('show');
    }
    function hideStoppedOverlay() {
        var el = document.getElementById('player-stopped');
        if (el) el.classList.remove('show');
    }
    function showNoData() {
        document.getElementById('btn-stop').click(); // programmatic (isTrusted=false)
        var nd = document.getElementById('player-nodata');
        if (nd) nd.classList.add('show');
    }
    function hideNoData() {
        var nd = document.getElementById('player-nodata');
        if (nd) nd.classList.remove('show');
    }

    state.showStoppedOverlay = showStoppedOverlay;
    state.hideStoppedOverlay = hideStoppedOverlay;
    state.showNoData         = showNoData;
    state.hideNoData         = hideNoData;

    // ── Freeze frame ──────────────────────────────────────────────────────────
    function _captureFrame() {
        var v = player.videoEl;
        if (!freezeCanvas || !v || v.videoWidth === 0 || v.readyState < 2) return false;
        freezeCanvas.width  = v.videoWidth;
        freezeCanvas.height = v.videoHeight;
        try {
            freezeCanvas.getContext('2d').drawImage(v, 0, 0);
            freezeCanvas.classList.add('show');
            return true;
        } catch (_) { return false; }
    }

    function _clearFrame() {
        if (!freezeCanvas) return;
        freezeCanvas.classList.remove('show');
        var ctx = freezeCanvas.getContext('2d');
        if (ctx) ctx.clearRect(0, 0, freezeCanvas.width, freezeCanvas.height);
    }
    state.clearFrame = _clearFrame;

    // ── Player event handlers ─────────────────────────────────────────────────
    player.on('log', function (level, msg) {
        if (logEl) {
            var d = document.createElement('div');
            d.className   = 'log-' + level;
            d.textContent = msg;
            logEl.appendChild(d);
            logEl.scrollTop = logEl.scrollHeight;
        }
        if (player.debug) {
            (console[level] || console.log)('[s3pro]', msg);
        }
    });

    player.on('error', function (msg) {
        if (msg === null) {
            if (errOverlay) errOverlay.classList.remove('show');
            return;
        }
        if (errMsgEl)   errMsgEl.textContent = msg || 'Connection error';
        if (errOverlay) errOverlay.classList.add('show');
    });

    player.on('statechange', function (s) {
        var isLoading = (s === 'connecting' || s === 'buffering');
        if (loadingOverlay) {
            loadingOverlay.classList.toggle('show', isLoading);
            loadingOverlay.classList.toggle('buffering', s === 'buffering');
        }
        if (s === 'playing' || s === 'buffering') {
            if (errOverlay) errOverlay.classList.remove('show');
        }
        if (s === 'stopped') {
            _captureFrame();
            showStoppedOverlay();
        } else if (s === 'connecting') {
            _clearFrame();
            hideStoppedOverlay();
        }
        if (s === 'stopped' || s === 'idle' || s === 'error') {
            state.stopped = true;
        } else if (s === 'connecting' || s === 'playing' || s === 'buffering') {
            state.stopped = false;
        }
    });

    player.on('timeupdate', function (elapsedMs, codec, bufLen, videoSize) {
        var friendly = _friendlyCodec(codec);
        if (infoEl)
            infoEl.textContent = 'codec: ' + (friendly || '--') +
                                 ' | bufferLen: ' + (bufLen ? bufLen.toFixed(2) + 's' : '--');
        if (timeEl) {
            if (state.mode === 'replay' && state.replayStartTime && elapsedMs > 0) {
                timeEl.textContent = new Date((state.replayStartTime + elapsedMs / 1000) * 1000).toLocaleTimeString();
            } else {
                var sec = Math.floor(elapsedMs / 1000);
                var hh = Math.floor(sec / 3600), mm = Math.floor((sec % 3600) / 60), ss = sec % 60;
                timeEl.textContent = String(hh).padStart(2,'0') + ':' + String(mm).padStart(2,'0') + ':' + String(ss).padStart(2,'0');
            }
        }
        if (!friendly) return;
        // Update Auto item realtime info (resolution + codec) in quality menu
        var pcbQualMenu = document.getElementById('pcb-quality-menu');
        var vs = videoSize || player.videoSize;
        var info = (vs.width && vs.height ? vs.width + 'x' + vs.height + ' ' : '') + friendly;
        var autoItem = pcbQualMenu && pcbQualMenu.querySelector('.qm-item[data-profile="auto"]');
        if (autoItem) {
            var infoSpan = autoItem.querySelector('.qm-info');
            if (!infoSpan) {
                infoSpan = document.createElement('span');
                infoSpan.className = 'qm-info';
                autoItem.appendChild(infoSpan);
            }
            infoSpan.textContent = info;
        }
    });
}
