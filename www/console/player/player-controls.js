// =============================================================================
// player-controls.js — Coordinator: assembles all player-control sub-modules.
//
// Sub-module load order (script tags must appear before this file):
//   pc-hud.js → pc-stream-select.js → pc-timeline.js → pc-control-bar.js
//
// Requires an S3ProPlugin instance (vp.s3pro()) passed as argument.
// Compatible with s3pro-player.js VideoJS plugin API.
// =============================================================================

function initPlayerControls(player) {

    // =========================================================================
    // Shared state — all sub-modules read/write this object
    // =========================================================================
    const state = {
        // App state
        mode:              'live',  // 'live' | 'replay'
        stopped:           false,
        replayStartTime:   0,       // unix sec
        dayStart:          0,       // unix sec: midnight of loaded date
        selectedStreamId:  'auto',
        selectedTransport: 'mp4',   // 'mp4' | 'hls' | 'ws-fmp4'
        consoleLogEnabled: false,
        // PCB timebar data (set by timeline, read by control-bar)
        pcbDayStart:       0,
        pcbRecHours:       null,
        pcbMotHours:       null,
        // Cross-module function refs — populated during sub-module init
        setMode:               null,
        seekToWallTime:        null,
        moveCursorToWallTime:  null,
        triggerTimelineRefresh:null,
        pcbMoveCursor:         null,
        pcbRenderTimebar:      null,
        populateQualityMenu:   null,
        updateTransportMenu:   null,
        getActiveStreamId:     null,
        buildLiveUrl:          null,
        buildVodUrl:           null,
        clearFrame:            null,
        showStoppedOverlay:    null,
        hideStoppedOverlay:    null,
        showNoData:            null,
        hideNoData:            null,
        friendlyCodec:         null,
        errOverlay:            null,
        errMsgEl:              null,
    };

    // =========================================================================
    // Mode switch: Live / Replay
    // =========================================================================
    const liveBtn   = document.getElementById('mode-live');
    const replayBtn = document.getElementById('mode-replay');

    // Badge elements
    const badgeLive   = document.getElementById('badge-live');
    const badgeReplay = document.getElementById('badge-replay');
    const badgeMotion = document.getElementById('badge-motion');

    function setMode(m) {
        state.mode = m;
        liveBtn.classList.toggle('active',   m === 'live');
        replayBtn.classList.toggle('active', m === 'replay');
        // Show the relevant badge; hide the other
        if (badgeLive)   badgeLive.classList.toggle('show',   m === 'live');
        if (badgeReplay) badgeReplay.classList.toggle('show', m === 'replay');
        // In replay mode: HLS is not supported — silently switch transport to MP4
        if (m === 'replay' && state.selectedTransport === 'hls') {
            state.selectedTransport = 'mp4';
            if (state.updateTransportMenu) state.updateTransportMenu();
        }
    }
    state.setMode = setMode;
    liveBtn.addEventListener('click',   function () { setMode('live'); });
    replayBtn.addEventListener('click', function () { setMode('replay'); });

    // =========================================================================
    // Motion badge: show MJPEG motion overlay when camera supports motion
    // =========================================================================
    var _motionSupported = false;
    var _motionOnline    = false;
    var _motionPlaying   = false;
    var _motionOverlay   = document.getElementById('motion-overlay');
    var _BLANK_GIF       = 'data:image/gif;base64,R0lGODlhAQABAAD/ACwAAAAAAQABAAACADs=';

    function _updateMotionBadge() {
        if (!badgeMotion) return;
        var canShow = _motionSupported;
        badgeMotion.classList.toggle('show',           canShow);
        badgeMotion.classList.toggle('motion-on',      _motionPlaying);
        badgeMotion.classList.toggle('motion-offline', canShow && !_motionOnline);
        if (!canShow || !_motionOnline) {
            if (_motionPlaying) _stopMotionStream();
            badgeMotion.title = canShow
                ? 'Motion offline — không có kết nối'
                : 'Motion không được hỗ trợ';
        } else {
            badgeMotion.title = _motionPlaying
                ? 'Đang xem motion — nhấn để tắt'
                : 'Xem luồng phân tích motion';
        }
    }

    function _startMotionStream() {
        var domain   = (document.getElementById('media-domain').value || '').replace(/\/+$/, '');
        var cameraId = (document.getElementById('camera-id').value    || '').trim();
        if (!domain || !cameraId) return;
        var url = domain + '/media/live/' + encodeURIComponent(cameraId)
                + '.motion.mjpeg?overlay_roi=1&overlay_motion=1';
        if (_motionOverlay) {
            _motionOverlay.src = url;
            _motionOverlay.classList.add('show');
        }
        _motionPlaying = true;
        _updateMotionBadge();
    }

    function _stopMotionStream() {
        if (_motionOverlay) {
            _motionOverlay.src = _BLANK_GIF;
            _motionOverlay.classList.remove('show');
        }
        _motionPlaying = false;
        _updateMotionBadge();
    }

    if (badgeMotion) {
        badgeMotion.addEventListener('click', function () {
            if (!_motionOnline) return; // ignore click when offline
            if (_motionPlaying) { _stopMotionStream(); }
            else                { _startMotionStream(); }
        });
    }
    _updateMotionBadge();

    // Called by _applyDeviceStreams when device statistic is loaded
    window.setMotionActive = function (supported, online) {
        _motionSupported = !!supported;
        _motionOnline    = !!online;
        _updateMotionBadge();
    };
    window.isMotionPlaying = function () { return _motionPlaying; };

    // =========================================================================
    // Play / Stop button wiring
    // =========================================================================
    document.getElementById('btn-stop').addEventListener('click', function (e) {
        state.stopped = true;
        player.stop();
        if (e.isTrusted) {
            if (state.hideNoData)         state.hideNoData();
            if (state.clearFrame)         state.clearFrame();
            if (state.showStoppedOverlay) state.showStoppedOverlay();
        }
    }, true /* capture */);

    document.getElementById('btn-play').addEventListener('click', function () {
        state.stopped = false;
        if (state.hideNoData)         state.hideNoData();
        if (state.hideStoppedOverlay) state.hideStoppedOverlay();
        if (state.clearFrame)         state.clearFrame();
        if (state.errOverlay) state.errOverlay.classList.remove('show');
        var url = document.getElementById('url-input')
            ? document.getElementById('url-input').value.trim() : '';
        if (url) player.play(url);
    });

    var _urlInput = document.getElementById('url-input');
    if (_urlInput) {
        _urlInput.addEventListener('keydown', function (e) {
            if (e.key === 'Enter') document.getElementById('btn-play').click();
        });
    }

    // =========================================================================
    // Init sub-modules
    // Order: HUD → PCB → Streams → Timeline
    //   HUD first  → sets state.errOverlay / errMsgEl used by later modules
    //   PCB second → state.pcbMoveCursor / pcbRenderTimebar set before timeline
    //   Streams    → uses pcbQualMenu DOM (no PCB function refs needed at init)
    //   Timeline   → sets seekToWallTime / moveCursorToWallTime / triggerTimelineRefresh
    // =========================================================================
    initPlayerHud(player, state);
    initControlBar(player, state);
    initStreamSelect(player, state);
    initTimeline(player, state);

    // Auto-play from ?url= query parameter (runs after all modules are wired)
    var _qUrl = new URLSearchParams(location.search).get('url');
    if (_qUrl) {
        if (_urlInput) _urlInput.value = _qUrl;
        document.getElementById('btn-play').click();
    }

    return {
        setConsoleLogEnabled: function (v) { state.consoleLogEnabled = !!v; },
    };
}
