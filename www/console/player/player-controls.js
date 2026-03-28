// =============================================================================
// player-controls.js - Mode switch (Live/Replay) + Recording Timeline + Cursor
// Requires a S3ProPlayer instance passed as argument.
// =============================================================================

function initPlayerControls(player) {

    // =========================================================================
    // Mode switch: Live / Replay
    // =========================================================================
    const liveBtn   = document.getElementById('mode-live');
    const replayBtn = document.getElementById('mode-replay');
    let _mode    = 'live';
    let _stopped = false;

    // Badge elements
    const badgeLive   = document.getElementById('badge-live');
    const badgeReplay = document.getElementById('badge-replay');
    const badgeMotion = document.getElementById('badge-motion');

    function setMode(m) {
        _mode = m;
        liveBtn.classList.toggle('active',   m === 'live');
        replayBtn.classList.toggle('active', m === 'replay');
        // Show the relevant badge; hide the other
        if (badgeLive)   badgeLive.classList.toggle('show',   m === 'live');
        if (badgeReplay) badgeReplay.classList.toggle('show', m === 'replay');
    }
    liveBtn.addEventListener('click',   function () { setMode('live'); });
    replayBtn.addEventListener('click', function () { setMode('replay'); })

    // =========================================================================
    // Motion badge toggle + public API
    // =========================================================================
    let _motionEnabled = true;   // UI toggle state
    let _motionActive  = false;  // server-driven active state

    function _updateMotionBadge() {
        if (!badgeMotion) return;
        // Enabled+active → red;  enabled+idle → gray;  disabled → hidden/dim
        badgeMotion.classList.toggle('motion-on', _motionEnabled && _motionActive);
        badgeMotion.style.opacity = _motionEnabled ? '1' : '0.35';
        badgeMotion.title = _motionEnabled
            ? (_motionActive ? 'Motion detected — click to disable' : 'Motion detection ON — click to disable')
            : 'Motion detection OFF — click to enable';
    }

    if (badgeMotion) {
        badgeMotion.addEventListener('click', function () {
            _motionEnabled = !_motionEnabled;
            _updateMotionBadge();
            // Hook for future feature: notify external code
            if (typeof window.onMotionToggle === 'function') {
                window.onMotionToggle(_motionEnabled);
            }
        });
    }
    _updateMotionBadge();

    // Public API: call from future motion-event handler to light up the badge
    //   window.setMotionActive(true)   → badge turns red
    //   window.setMotionActive(false)  → badge back to gray
    window.setMotionActive = function (active) {
        _motionActive = !!active;
        _updateMotionBadge();
    };
    // Public API: check whether user has motion UI enabled
    window.isMotionEnabled = function () { return _motionEnabled; };

    // =========================================================================
    // Wire player events: log, fatal error, timeupdate
    // =========================================================================
    var logEl      = document.getElementById('log');
    var infoEl     = document.getElementById('info-text');
    var timeEl     = document.getElementById('time-display');
    var errOverlay = document.getElementById('player-error');
    var errMsgEl   = document.getElementById('player-error-msg');

    player.on('log', function (level, msg) {
        if (!logEl) return;
        var d = document.createElement('div');
        d.className   = 'log-' + level;
        d.textContent = msg;
        logEl.appendChild(d);
        logEl.scrollTop = logEl.scrollHeight;
    });

    player.on('error', function (msg) {
        if (msg === null) {
            if (errOverlay) errOverlay.classList.remove('show');
            return;
        }
        if (errMsgEl)   errMsgEl.textContent = msg || 'Connection error';
        if (errOverlay) errOverlay.classList.add('show');
    });

    player.on('timeupdate', function (ct, fmt, codec, bufLen) {
        if (infoEl)
            infoEl.textContent = 'codec: ' + (codec || '--') +
                                 ' | bufferLen: ' + (bufLen ? bufLen.toFixed(2) + 's' : '--');
        if (timeEl) timeEl.textContent = fmt || '00:00:00';
    });

    // =========================================================================
    // URL builder for Live mode - capture phase runs before player.play()
    // =========================================================================
    document.getElementById('btn-play').addEventListener('click', function () {
        if (_mode !== 'live') return;
        const domain   = (document.getElementById('media-domain').value || '').replace(/\/+$/, '');
        const cameraId = (document.getElementById('camera-id').value    || '').trim();
        const streamId = (document.getElementById('stream-id').value    || '').trim();
        document.getElementById('url-input').value =
            domain + '/media/live/' + cameraId + '/' + streamId + '.live.mp4';
    }, true /* capture phase */);

    // =========================================================================
    // No-data overlay
    // =========================================================================
    // ── Stopped overlay ──
    function showStoppedOverlay() {
        const el = document.getElementById('player-stopped');
        if (el) el.classList.add('show');
    }
    function hideStoppedOverlay() {
        const el = document.getElementById('player-stopped');
        if (el) el.classList.remove('show');
    }

    function showNoData() {
        document.getElementById('btn-stop').click(); // programmatic (isTrusted=false)
        const nd = document.getElementById('player-nodata');
        if (nd) nd.classList.add('show');
    }
    function hideNoData() {
        const nd = document.getElementById('player-nodata');
        if (nd) nd.classList.remove('show');
    }

    // Stop button: stop player + set _stopped, show stopped overlay (user click only)
    document.getElementById('btn-stop').addEventListener('click', function (e) {
        _stopped = true;
        player.stop();
        if (e.isTrusted) {
            hideNoData();
            showStoppedOverlay();
        }
    }, true /* capture */);

    // Hide overlays + call player.play() whenever Play is clicked
    document.getElementById('btn-play').addEventListener('click', function () {
        _stopped = false;
        hideNoData();
        hideStoppedOverlay();
        if (errOverlay) errOverlay.classList.remove('show');
        var url = document.getElementById('url-input')
            ? document.getElementById('url-input').value.trim() : '';
        if (url) player.play(url);
    });

    // url-input Enter key triggers play
    var _urlInput = document.getElementById('url-input');
    if (_urlInput) {
        _urlInput.addEventListener('keydown', function (e) {
            if (e.key === 'Enter') document.getElementById('btn-play').click();
        });
    }

    // Auto-play from ?url= query parameter
    var _qUrl = new URLSearchParams(location.search).get('url');
    if (_qUrl) {
        if (_urlInput) _urlInput.value = _qUrl;
        document.getElementById('btn-play').click();
    }

    // =========================================================================
    // Timeline state
    // =========================================================================
    const timelineEl   = document.getElementById('timeline');
    const dateInput    = document.getElementById('timeline-date');
    const fetchBtn     = document.getElementById('timeline-fetch');

    let _dayStart        = 0;   // unix sec: midnight of the loaded date
    let _replayStartTime = 0;   // unix sec: wall-clock start of current replay

    // Create the floating time label once
    const cursorTimeEl = document.createElement('div');
    cursorTimeEl.id = 'tl-cursor-time';
    timelineEl.appendChild(cursorTimeEl);

    // Default date = today (local)
    (function setToday() {
        const d = new Date();
        dateInput.value = d.getFullYear() + '-'
            + String(d.getMonth() + 1).padStart(2, '0') + '-'
            + String(d.getDate()).padStart(2, '0');
    })();

    // =========================================================================
    // Build 24 rows once
    // =========================================================================
    for (let h = 0; h < 24; h++) {
        const row = document.createElement('div');
        row.className = 'tl-row';

        const lbl = document.createElement('span');
        lbl.className = 'tl-hour';
        lbl.textContent = String(h).padStart(2, '0');

        // Two-lane wrapper: recording bar (top) + motion strip (bottom)
        const tracks = document.createElement('div');
        tracks.className = 'tl-tracks';

        const bar = document.createElement('div');
        bar.className = 'tl-bar';
        bar.dataset.hour = h;
        bar.style.cursor = 'crosshair';

        // Thin motion indicator strip — visual only, not interactive
        const mbar = document.createElement('div');
        mbar.className = 'tl-mbar';
        mbar.dataset.hour = h;

        // White cursor line inside recording bar
        const cursor = document.createElement('div');
        cursor.className = 'tl-cursor';
        bar.appendChild(cursor);

        // Click handler on recording bar
        bar.addEventListener('click', function (e) {
            if (!_dayStart) return;
            const rect     = bar.getBoundingClientRect();
            const ratio    = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));
            const wallTime = _dayStart + Number(bar.dataset.hour) * 3600 + ratio * 3600;

            const isRecording = e.target.classList.contains('tl-segment');

            if (isRecording) {
                seekToWallTime(wallTime);
            } else {
                moveCursorToWallTime(wallTime);
                showNoData();
            }
        });

        tracks.appendChild(bar);
        tracks.appendChild(mbar);
        row.appendChild(lbl);
        row.appendChild(tracks);
        timelineEl.appendChild(row);
    }

    // =========================================================================
    // Cursor helpers
    // =========================================================================
    function moveCursorToWallTime(wallTime) {
        // Hide all cursor lines
        document.querySelectorAll('#timeline .tl-cursor').forEach(function (c) {
            c.style.display = 'none';
        });
        cursorTimeEl.style.display = 'none';

        if (!_dayStart || wallTime < _dayStart || wallTime >= _dayStart + 86400) return;

        const relSec = wallTime - _dayStart;
        const h      = Math.floor(relSec / 3600);
        const pct    = ((relSec - h * 3600) / 3600) * 100;

        const bar = document.querySelector('#timeline .tl-bar[data-hour="' + h + '"]');
        if (!bar) return;

        // Show cursor line in bar
        const cursor = bar.querySelector('.tl-cursor');
        if (cursor) {
            cursor.style.left    = pct + '%';
            cursor.style.display = 'block';
        }

        // Position floating time label above this bar
        const barRect = bar.getBoundingClientRect();
        const tlRect  = timelineEl.getBoundingClientRect();
        const labelLeft = barRect.left - tlRect.left + barRect.width * pct / 100;
        const labelTop  = barRect.top  - tlRect.top  - 20; // 20px above bar

        const d = new Date(wallTime * 1000);
        cursorTimeEl.textContent  = d.toLocaleTimeString();
        cursorTimeEl.style.left   = labelLeft + 'px';
        cursorTimeEl.style.top    = labelTop  + 'px';
        cursorTimeEl.style.display = 'block';
    }

    // Poll videoEl.currentTime every 200ms to drive cursor during replay
    setInterval(function () {
        if (_stopped || _mode !== 'replay' || !_replayStartTime || !_dayStart) return;
        const videoEl = document.querySelector('.vjs-tech');
        if (!videoEl || videoEl.paused) return;
        if (!videoEl.currentTime) return;
        moveCursorToWallTime(_replayStartTime + videoEl.currentTime);
    }, 200);

    // =========================================================================
    // Seek to exact wall-clock time
    // =========================================================================
    function seekToWallTime(wallTime) {
        const ts       = Math.floor(wallTime);
        const domain   = (document.getElementById('media-domain').value || '').replace(/\/+$/, '');
        const cameraId = (document.getElementById('camera-id').value    || '').trim();
        const streamId = (document.getElementById('stream-id').value    || '').trim();
        _replayStartTime = ts;
        setMode('replay');
        document.getElementById('url-input').value =
            domain + '/media/record/' + cameraId + '/' + streamId + '/vod/' + ts + '.live.mp4';
        document.getElementById('btn-play').click();
        moveCursorToWallTime(wallTime);
    }

    // =========================================================================
    // Timeline data helpers
    // =========================================================================
    function clearTimeline() {
        document.querySelectorAll('#timeline .tl-bar').forEach(function (b) {
            Array.from(b.childNodes).forEach(function (c) {
                if (!c.classList || !c.classList.contains('tl-cursor')) b.removeChild(c);
            });
        });
        document.querySelectorAll('#timeline .tl-mbar').forEach(function (b) {
            b.innerHTML = '';
        });
        cursorTimeEl.style.display = 'none';
    }

    function validateInputs() {
        const ids = ['camera-id', 'stream-id', 'media-domain'];
        let ok = true;
        ids.forEach(function (id) {
            const el = document.getElementById(id);
            if (!el || !el.value.trim()) {
                ok = false;
                if (el) {
                    el.classList.add('ctrl-input-error');
                    el.addEventListener('input', function () {
                        el.classList.remove('ctrl-input-error');
                    }, { once: true });
                }
            }
        });
        return ok;
    }

    function renderRecordingHour(bar, periods, hStart) {
        if (!Array.isArray(periods)) return;
        periods.forEach(function (p) {
            const st  = Number(p.startTime);
            const dur = Number(p.duration);
            if (!dur) return;
            const os = Math.max(st, hStart);
            const oe = Math.min(st + dur, hStart + 3600);
            if (os >= oe) return;

            const seg = document.createElement('div');
            seg.className = 'tl-segment';
            seg.style.left  = ((os - hStart) / 3600 * 100) + '%';
            seg.style.width = ((oe - os)     / 3600 * 100) + '%';
            seg.title = new Date(os * 1000).toLocaleTimeString()
                + ' - ' + new Date(oe * 1000).toLocaleTimeString();
            // clicks bubble up to bar handler
            bar.appendChild(seg);
        });
    }

    function renderMotionHour(mbar, periods, hStart) {
        if (!Array.isArray(periods) || !mbar) return;
        periods.forEach(function (p) {
            const st  = Number(p.startTime);
            const dur = Number(p.duration);
            if (!dur) return;
            const os = Math.max(st, hStart);
            const oe = Math.min(st + dur, hStart + 3600);
            if (os >= oe) return;

            const seg = document.createElement('div');
            seg.className = 'tl-mseg';
            seg.style.left  = ((os - hStart) / 3600 * 100) + '%';
            seg.style.width = ((oe - os)     / 3600 * 100) + '%';
            mbar.appendChild(seg);
        });
    }

    function resolveDatesArray(datesObj, dateStr) {
        if (!datesObj) return null;
        if (Array.isArray(datesObj[dateStr])) return datesObj[dateStr];
        const keys = Object.keys(datesObj);
        if (keys.length > 0 && Array.isArray(datesObj[keys[0]])) {
            console.warn('[Timeline] dates key mismatch: expected "' + dateStr + '", got "' + keys[0] + '"');
            return datesObj[keys[0]];
        }
        return null;
    }

    function fillTimeline(raw, dateStr, dayStart) {
        clearTimeline();
        _dayStart = dayStart;

        const json = (raw && typeof raw === 'object' && !Array.isArray(raw) && raw.data !== undefined)
            ? raw.data : raw;

        console.log('[Timeline] parsed json keys:', Object.keys(json || {}));
        const streamId = (document.getElementById('stream-id').value || '').trim();

        let recordingHours = null;
        if (Array.isArray(json.streams) && json.streams.length > 0) {
            const stream = json.streams.find(function (s) { return s.streamId === streamId; })
                        || json.streams[0];
            console.log('[Timeline] stream:', stream ? stream.streamId : 'none',
                        '| dates keys:', stream && stream.dates ? Object.keys(stream.dates) : []);
            recordingHours = resolveDatesArray(stream && stream.dates, dateStr);
        }
        console.log('[Timeline] recordingHours length:', recordingHours ? recordingHours.length : 'null');

        const motionHours = resolveDatesArray(json.motionPeriods, dateStr);

        for (let h = 0; h < 24; h++) {
            const bar    = document.querySelector('#timeline .tl-bar[data-hour="'  + h + '"]');
            const mbar   = document.querySelector('#timeline .tl-mbar[data-hour="' + h + '"]');
            if (!bar) continue;
            const hStart = dayStart + h * 3600;
            if (recordingHours) renderRecordingHour(bar,  recordingHours[h], hStart);
            if (motionHours)    renderMotionHour(mbar, motionHours[h],    hStart);
        }
        // Expose data to custom timebar (vars declared in custom-controls section)
        _pcbDayStart  = dayStart;
        _pcbRecHours  = recordingHours;
        _pcbMotHours  = motionHours;
        _pcbRenderTimebar();
    }

    // =========================================================================
    // Fetch
    // =========================================================================
    function fetchTimeline() {
        const dateStr = dateInput.value;
        if (!dateStr) return;
        if (!validateInputs()) return;

        const domain   = (document.getElementById('media-domain').value || 'http://localhost:8080').replace(/\/+$/, '');
        const cameraId = (document.getElementById('camera-id').value    || '').trim();

        const parts    = dateStr.split('-').map(Number);
        const dayStart = Math.floor(new Date(parts[0], parts[1] - 1, parts[2]).getTime() / 1000);
        const dayEnd   = dayStart + 86400;

        const url = domain + '/media/esc/recordedTimePeriod'
            + '?cameraId='  + encodeURIComponent(cameraId)
            + '&startTime=' + dayStart
            + '&endTime='   + dayEnd
            + '&periodType=2&detail=1&motion=1';

        console.log('[Timeline] fetch:', url);
        fetchBtn.disabled    = true;
        fetchBtn.textContent = '...';

        fetch(url)
            .then(function (r) { return r.json(); })
            .then(function (json) {
                console.log('[Timeline] raw response:', JSON.stringify(json).slice(0, 400));
                fillTimeline(json, dateStr, dayStart);
            })
            .catch(function (err) {
                console.warn('[Timeline] fetch error:', err);
            })
            .finally(function () {
                fetchBtn.disabled    = false;
                fetchBtn.textContent = 'Load';
            });
    }

    fetchBtn.addEventListener('click', fetchTimeline);
    dateInput.addEventListener('change', fetchTimeline);

    // Auto-refresh timeline every 60 seconds (silent, no button spinner)
    setInterval(function () {
        const dateStr = dateInput.value;
        if (!dateStr) return;
        const camId = (document.getElementById('camera-id').value || '').trim();
        if (!camId) return;

        const domain   = (document.getElementById('media-domain').value || 'http://localhost:8080').replace(/\/+$/, '');
        const parts    = dateStr.split('-').map(Number);
        const dayStart = Math.floor(new Date(parts[0], parts[1] - 1, parts[2]).getTime() / 1000);
        const dayEnd   = dayStart + 86400;

        const url = domain + '/media/esc/recordedTimePeriod'
            + '?cameraId='  + encodeURIComponent(camId)
            + '&startTime=' + dayStart
            + '&endTime='   + dayEnd
            + '&periodType=2&detail=1&motion=1';

        fetch(url)
            .then(function (r) { return r.json(); })
            .then(function (json) { fillTimeline(json, dateStr, dayStart); })
            .catch(function (err) { console.warn('[Timeline] auto-refresh error:', err); });
    }, 60000);

    // =========================================================================
    // Custom control bar
    // =========================================================================
    // Public hook: called by fmp4-player after timeline data available
    // so timebar can render recording/motion segments for the current hour.
    // Also called on every poll tick to advance the cursor.

    // Data captured by fillTimeline (hoisted function writes these before use)
    var _pcbRecHours  = null;   // 24-element array of period arrays
    var _pcbMotHours  = null;
    var _pcbDayStart  = 0;

    var playerWrap    = document.getElementById('player-wrap');
    var pcbTrack      = document.getElementById('pcb-timebar-track');
    var pcbCursor     = document.getElementById('pcb-cursor');
    var pcbHoverTime  = document.getElementById('pcb-hover-time');
    var pcbTimeEl     = document.getElementById('pcb-time');
    var pcbPlayPause  = document.getElementById('pcb-playpause');
    var pcbIconPlay   = document.getElementById('pcb-icon-play');
    var pcbIconPause  = document.getElementById('pcb-icon-pause');
    var pcbVolBtn     = document.getElementById('pcb-vol-btn');
    var pcbVolWrap    = document.getElementById('pcb-vol-wrap');
    var pcbVolSlider  = document.getElementById('pcb-vol');
    var pcbIconVol    = document.getElementById('pcb-icon-vol');
    var pcbIconMute   = document.getElementById('pcb-icon-mute');
    var pcbCapture    = document.getElementById('pcb-capture');
    var pcbFsBtn      = document.getElementById('pcb-fullscreen');
    var pcbIconFs     = document.getElementById('pcb-icon-fs');
    var pcbIconExitFs = document.getElementById('pcb-icon-exit-fs');
    var pcbQualBtn    = document.getElementById('pcb-quality-btn');
    var pcbQualMenu   = document.getElementById('pcb-quality-menu');

    var _pcbCurrentHour = -1;

    // -- Render timebar segments for the current wall-clock hour --
    function _pcbRenderTimebar() {
        if (!pcbTrack) return;
        // Clear old segments
        Array.from(pcbTrack.children).forEach(function (c) { pcbTrack.removeChild(c); });
        if (!_pcbDayStart || !_replayStartTime) return;

        var wallNow = _replayStartTime; // current replay wall start
        var h = Math.floor((wallNow - _pcbDayStart) / 3600);
        if (h < 0 || h >= 24) return;
        _pcbCurrentHour = h;
        var hStart = _pcbDayStart + h * 3600;

        // Recording segments
        if (_pcbRecHours && _pcbRecHours[h]) {
            _pcbRecHours[h].forEach(function (p) {
                var st  = Number(p.startTime);
                var dur = Number(p.duration);
                if (!dur) return;
                var os = Math.max(st, hStart);
                var oe = Math.min(st + dur, hStart + 3600);
                if (os >= oe) return;
                var seg = document.createElement('div');
                seg.className = 'pcb-ts-seg';
                seg.style.left  = ((os - hStart) / 3600 * 100) + '%';
                seg.style.width = ((oe - os)     / 3600 * 100) + '%';
                pcbTrack.appendChild(seg);
            });
        }
        // Motion segments
        if (_pcbMotHours && _pcbMotHours[h]) {
            _pcbMotHours[h].forEach(function (p) {
                var st  = Number(p.startTime);
                var dur = Number(p.duration);
                if (!dur) return;
                var os = Math.max(st, hStart);
                var oe = Math.min(st + dur, hStart + 3600);
                if (os >= oe) return;
                var seg = document.createElement('div');
                seg.className = 'pcb-tm-seg';
                seg.style.left  = ((os - hStart) / 3600 * 100) + '%';
                seg.style.width = ((oe - os)     / 3600 * 100) + '%';
                pcbTrack.appendChild(seg);
            });
        }
    }

    // -- Move timebar cursor to a wall-clock time --
    function _pcbMoveCursor(wallTime) {
        if (!pcbCursor || !pcbTrack) return;
        if (!_pcbDayStart) { pcbCursor.style.display = 'none'; return; }
        var h = Math.floor((wallTime - _pcbDayStart) / 3600);
        if (h !== _pcbCurrentHour) {
            _pcbBuildForHour(h, wallTime);
        }
        var hStart = _pcbDayStart + h * 3600;
        var pct    = (wallTime - hStart) / 3600;
        pct = Math.max(0, Math.min(1, pct));
        pcbCursor.style.left    = (pct * 100) + '%';
        pcbCursor.style.display = 'block';
    }

    // Rebuild timebar segments when the replay position crosses into a new hour
    function _pcbBuildForHour(h, wallTime) {
        if (!pcbTrack) return;
        _pcbCurrentHour = h;
        var hStart = _pcbDayStart + h * 3600;
        Array.from(pcbTrack.children).forEach(function (c) { pcbTrack.removeChild(c); });
        if (_pcbRecHours && _pcbRecHours[h]) {
            _pcbRecHours[h].forEach(function (p) {
                var st = Number(p.startTime), dur = Number(p.duration);
                if (!dur) return;
                var os = Math.max(st, hStart), oe = Math.min(st + dur, hStart + 3600);
                if (os >= oe) return;
                var seg = document.createElement('div');
                seg.className = 'pcb-ts-seg';
                seg.style.left  = ((os - hStart) / 3600 * 100) + '%';
                seg.style.width = ((oe - os)     / 3600 * 100) + '%';
                pcbTrack.appendChild(seg);
            });
        }
        if (_pcbMotHours && _pcbMotHours[h]) {
            _pcbMotHours[h].forEach(function (p) {
                var st = Number(p.startTime), dur = Number(p.duration);
                if (!dur) return;
                var os = Math.max(st, hStart), oe = Math.min(st + dur, hStart + 3600);
                if (os >= oe) return;
                var seg = document.createElement('div');
                seg.className = 'pcb-tm-seg';
                seg.style.left  = ((os - hStart) / 3600 * 100) + '%';
                seg.style.width = ((oe - os)     / 3600 * 100) + '%';
                pcbTrack.appendChild(seg);
            });
        }
    }

    // -- Timebar click: seek to wall time within current hour --
    var pcbTimebarWrap = document.getElementById('pcb-timebar-wrap');
    if (pcbTimebarWrap) {
        pcbTimebarWrap.addEventListener('click', function (e) {
            if (!_pcbDayStart || _pcbCurrentHour < 0) return;
            var rect  = pcbTrack.getBoundingClientRect();
            var ratio = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));
            var wall  = _pcbDayStart + _pcbCurrentHour * 3600 + ratio * 3600;
            // Check if this position has recording data
            var hPeriods = _pcbRecHours && _pcbRecHours[_pcbCurrentHour];
            var hasData  = false;
            if (hPeriods) {
                hPeriods.forEach(function (p) {
                    if (wall >= Number(p.startTime) && wall < Number(p.startTime) + Number(p.duration)) {
                        hasData = true;
                    }
                });
            }
            if (hasData) { seekToWallTime(wall); }
        });

        // Hover tooltip
        pcbTimebarWrap.addEventListener('mousemove', function (e) {
            if (!_pcbDayStart || _pcbCurrentHour < 0) return;
            var rect  = pcbTrack.getBoundingClientRect();
            var ratio = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));
            var wall  = _pcbDayStart + _pcbCurrentHour * 3600 + ratio * 3600;
            var wrapRect = pcbTimebarWrap.getBoundingClientRect();
            pcbHoverTime.textContent  = new Date(wall * 1000).toLocaleTimeString();
            pcbHoverTime.style.left   = (e.clientX - wrapRect.left) + 'px';
            pcbHoverTime.style.display = 'block';
        });
        pcbTimebarWrap.addEventListener('mouseleave', function () {
            if (pcbHoverTime) pcbHoverTime.style.display = 'none';
        });
    }

    // -- Play/Pause button --
    if (pcbPlayPause) {
        pcbPlayPause.addEventListener('click', function () {
            var videoEl = document.querySelector('.vjs-tech');
            if (!videoEl) return;
            if (videoEl.paused) { videoEl.play().catch(function(){}); }
            else                { videoEl.pause(); }
        });
    }

    // -- Volume button + slider --
    var _prevVol = 0.5;
    if (pcbVolBtn) {
        pcbVolBtn.addEventListener('click', function () {
            if (pcbVolWrap) pcbVolWrap.classList.toggle('show');
        });
    }
    if (pcbVolSlider) {
        pcbVolSlider.addEventListener('input', function () {
            var v = parseFloat(pcbVolSlider.value);
            var videoEl = document.querySelector('.vjs-tech');
            if (videoEl) {
                videoEl.muted  = (v === 0);
                videoEl.volume = v || 0.001;
            }
            if (v > 0) _prevVol = v;
            _pcbUpdateVolIcon(v);
        });
    }
    function _pcbUpdateVolIcon(v) {
        if (!pcbIconVol || !pcbIconMute) return;
        if (v === 0) { pcbIconVol.style.display = 'none'; pcbIconMute.style.display = ''; }
        else         { pcbIconVol.style.display = '';     pcbIconMute.style.display = 'none'; }
    }
    // Double-click on slider area = mute/unmute
    if (pcbVolBtn) {
        pcbVolBtn.addEventListener('dblclick', function () {
            var videoEl = document.querySelector('.vjs-tech');
            if (!videoEl) return;
            if (videoEl.muted || videoEl.volume === 0) {
                videoEl.muted  = false;
                videoEl.volume = _prevVol || 0.5;
                if (pcbVolSlider) pcbVolSlider.value = videoEl.volume;
                _pcbUpdateVolIcon(videoEl.volume);
            } else {
                _prevVol = videoEl.volume;
                videoEl.muted = true;
                if (pcbVolSlider) pcbVolSlider.value = 0;
                _pcbUpdateVolIcon(0);
            }
        });
    }

    // -- Capture (canvas snapshot) --
    if (pcbCapture) {
        pcbCapture.addEventListener('click', function () {
            var videoEl = document.querySelector('.vjs-tech');
            if (!videoEl) return;
            var canvas = document.createElement('canvas');
            canvas.width  = videoEl.videoWidth  || 1280;
            canvas.height = videoEl.videoHeight || 720;
            canvas.getContext('2d').drawImage(videoEl, 0, 0, canvas.width, canvas.height);
            var ts  = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
            var cam = (document.getElementById('camera-id').value || 'cam').trim();
            var a   = document.createElement('a');
            a.download = 'capture-' + cam + '-' + ts + '.png';
            a.href     = canvas.toDataURL('image/png');
            a.click();
        });
    }

    // -- Fullscreen --
    if (pcbFsBtn) {
        pcbFsBtn.addEventListener('click', function () {
            if (!document.fullscreenElement) {
                (playerWrap || document.body).requestFullscreen().catch(function(){});
            } else {
                document.exitFullscreen().catch(function(){});
            }
        });
        document.addEventListener('fullscreenchange', function () {
            var isFs = !!document.fullscreenElement;
            if (pcbIconFs)     pcbIconFs.style.display     = isFs ? 'none' : '';
            if (pcbIconExitFs) pcbIconExitFs.style.display = isFs ? ''     : 'none';
            // Keep controls visible in fullscreen
            if (playerWrap) playerWrap.classList.toggle('controls-pinned', isFs);
        });
    }

    // -- Quality / profile picker --
    // Placeholder: list is populated externally via window.setQualityProfiles([...])
    if (pcbQualBtn) {
        pcbQualBtn.addEventListener('click', function (e) {
            e.stopPropagation();
            if (pcbQualMenu) pcbQualMenu.classList.toggle('show');
        });
        document.addEventListener('click', function () {
            if (pcbQualMenu) pcbQualMenu.classList.remove('show');
        });
    }
    if (pcbQualMenu) {
        pcbQualMenu.addEventListener('click', function (e) {
            var item = e.target.closest('.qm-item');
            if (!item) return;
            pcbQualMenu.querySelectorAll('.qm-item').forEach(function (i) { i.classList.remove('active'); });
            item.classList.add('active');
            var profile = item.dataset.profile;
            pcbQualMenu.classList.remove('show');
            if (typeof window.onProfileSelect === 'function') window.onProfileSelect(profile);
        });
    }
    // Public API: populate quality menu from stream info
    window.setQualityProfiles = function (profiles) {
        // profiles: [{id, label}]  e.g. [{id:'auto',label:'Auto'},{id:'1080p',label:'1080p HD'}]
        if (!pcbQualMenu) return;
        var active = (pcbQualMenu.querySelector('.qm-item.active') || {}).dataset || {};
        var activeId = active.profile || 'auto';
        // Remove existing items (keep title)
        Array.from(pcbQualMenu.querySelectorAll('.qm-item')).forEach(function (el) { el.remove(); });
        profiles.forEach(function (p) {
            var div = document.createElement('div');
            div.className = 'qm-item' + (p.id === activeId ? ' active' : '');
            div.dataset.profile = p.id;
            div.innerHTML = '<svg class="qm-check" viewBox="0 0 24 24" fill="currentColor"><path d="M9 16.17L4.83 12l-1.42 1.41L9 19 21 7l-1.41-1.41z"/></svg>' + p.label;
            pcbQualMenu.appendChild(div);
        });
    };

    // -- Poll every 200ms to advance cursor + sync play/pause icon --
    setInterval(function () {
        var videoEl = document.querySelector('.vjs-tech');
        if (!videoEl) return;

        // Play/Pause icon
        if (pcbIconPlay && pcbIconPause) {
            if (videoEl.paused) { pcbIconPlay.style.display = ''; pcbIconPause.style.display = 'none'; }
            else                { pcbIconPlay.style.display = 'none'; pcbIconPause.style.display = ''; }
        }

        // Volume icon sync (in case browser muted externally)
        if (pcbVolSlider && !pcbVolSlider._dragging) {
            var v = videoEl.muted ? 0 : videoEl.volume;
            pcbVolSlider.value = v;
            _pcbUpdateVolIcon(v);
        }

        // Time display
        if (pcbTimeEl) {
            var ct = videoEl.currentTime;
            if (_replayStartTime && _mode === 'replay') {
                var wall = _replayStartTime + ct;
                var d2   = new Date(wall * 1000);
                pcbTimeEl.textContent = d2.toLocaleTimeString();
            } else {
                var s  = Math.floor(ct);
                var hh = Math.floor(s / 3600), mm = Math.floor((s % 3600) / 60), ss = s % 60;
                pcbTimeEl.textContent = String(hh).padStart(2,'0') + ':' + String(mm).padStart(2,'0') + ':' + String(ss).padStart(2,'0');
            }
        }

        // Timebar cursor
        if (_mode === 'replay' && _replayStartTime && _pcbDayStart) {
            _pcbMoveCursor(_replayStartTime + videoEl.currentTime);
        }
    }, 200);
}
