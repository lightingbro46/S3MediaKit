// =============================================================================
// pc-timeline.js — 24-hour recording timeline: DOM build, data fetch, cursor,
//                  seek-to-wall-time.
// Called by initPlayerControls(). Populates state with seekToWallTime,
// moveCursorToWallTime, and triggerTimelineRefresh.
// =============================================================================

function initTimeline(player, state) {
    var timelineEl = document.getElementById('timeline');
    var dateInput  = document.getElementById('timeline-date');
    var fetchBtn   = document.getElementById('timeline-fetch');

    // Floating time label positioned above the active cursor
    var cursorTimeEl = document.createElement('div');
    cursorTimeEl.id = 'tl-cursor-time';
    timelineEl.appendChild(cursorTimeEl);

    // Default date = today (local)
    (function setToday() {
        var d = new Date();
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

        bar.addEventListener('click', function (e) {
            if (!state.dayStart) return;
            const rect     = bar.getBoundingClientRect();
            const ratio    = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));
            const wallTime = state.dayStart + h * 3600 + ratio * 3600;
            const isRecording = e.target.classList.contains('tl-segment');
            if (isRecording) {
                seekToWallTime(wallTime);
            } else {
                moveCursorToWallTime(wallTime);
                if (state.showNoData) state.showNoData();
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
        document.querySelectorAll('#timeline .tl-cursor').forEach(function (c) {
            c.style.display = 'none';
        });
        cursorTimeEl.style.display = 'none';

        if (!state.dayStart || wallTime < state.dayStart || wallTime >= state.dayStart + 86400) return;

        const relSec = wallTime - state.dayStart;
        const h      = Math.floor(relSec / 3600);
        const pct    = ((relSec - h * 3600) / 3600) * 100;

        const bar = document.querySelector('#timeline .tl-bar[data-hour="' + h + '"]');
        if (!bar) return;

        const cursor = bar.querySelector('.tl-cursor');
        if (cursor) {
            cursor.style.left    = pct + '%';
            cursor.style.display = 'block';
        }

        const barRect = bar.getBoundingClientRect();
        const tlRect  = timelineEl.getBoundingClientRect();
        const labelLeft = barRect.left - tlRect.left + barRect.width * pct / 100;
        const labelTop  = barRect.top  - tlRect.top  - 20; // 20px above bar

        const d = new Date(wallTime * 1000);
        cursorTimeEl.textContent   = d.toLocaleTimeString();
        cursorTimeEl.style.left    = labelLeft + 'px';
        cursorTimeEl.style.top     = labelTop  + 'px';
        cursorTimeEl.style.display = 'block';
    }
    state.moveCursorToWallTime = moveCursorToWallTime;

    // ── DTS event: advance timeline cursor at real-clock speed ────────────────
    // Only move the cursor once the player is actually displaying frames.
    // During the initial buffering/stall phase (readyState < 3, no frame yet)
    // DTS events still arrive from incoming segments but nothing is on screen —
    // advancing the cursor early would show the wrong position to the user.
    var _playerPlaying = false;
    player.on('statechange', function (s) {
        _playerPlaying = (s === 'playing');
    });

    player.on('dts', function (/* dtsMs, codec */) {
        if (!_playerPlaying) return;
        if (state.mode !== 'replay' || !state.replayStartTime) return;
        // Use player.elapsedMs (real wall-clock playing time) instead of the raw
        // DTS value.  readTfdt() always divides by 90, so dtsMs only equals real
        // milliseconds when the stream uses a 90 kHz timescale.  Streams that use
        // other timescales (e.g. 1000 Hz) produce a dtsMs that is far smaller than
        // the actual playback position, causing the cursor to oscillate between
        // the (wrong) DTS-based position and the (correct) position emitted by the
        // 200 ms poll in pc-control-bar.js.
        var wallTime = state.replayStartTime + player.elapsedMs / 1000;
        moveCursorToWallTime(wallTime);
        if (state.pcbMoveCursor) state.pcbMoveCursor(wallTime);
    });

    // =========================================================================
    // Seek to exact wall-clock time
    // =========================================================================
    function seekToWallTime(wallTime) {
        // HLS does not support replay/VOD in this player
        if (state.selectedTransport === 'hls') {
            if (state.errMsgEl)   state.errMsgEl.textContent = 'HLS không hỗ trợ chế độ Replay — vui lòng chọn MP4';
            if (state.errOverlay) state.errOverlay.classList.add('show');
            return;
        }
        const ts       = Math.floor(wallTime);
        const domain   = (document.getElementById('media-domain').value || '').replace(/\/+$/, '');
        const cameraId = (document.getElementById('camera-id').value    || '').trim();
        const streamId = state.getActiveStreamId ? state.getActiveStreamId() : '';
        state.replayStartTime = ts;
        if (state.setMode) state.setMode('replay');
        document.getElementById('url-input').value = state.buildVodUrl
            ? state.buildVodUrl(domain, cameraId, streamId, ts)
            : '';
        document.getElementById('btn-play').click();
        moveCursorToWallTime(wallTime);
    }
    state.seekToWallTime = seekToWallTime;

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
        // stream-id is optional (auto mode); only camera-id and media-domain are required
        const ids = ['camera-id', 'media-domain'];
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
            if (state.consoleLogEnabled) console.warn('[Timeline] dates key mismatch: expected "' + dateStr + '", got "' + keys[0] + '"');
            return datesObj[keys[0]];
        }
        return null;
    }

    // Merge two 24-element arrays of period arrays (union per hour)
    function _mergeRecordingHours(hoursA, hoursB) {
        if (!hoursA && !hoursB) return null;
        if (!hoursA) return hoursB;
        if (!hoursB) return hoursA;
        const merged = [];
        for (let h = 0; h < 24; h++) {
            merged.push((hoursA[h] || []).concat(hoursB[h] || []));
        }
        return merged;
    }

    function fillTimeline(raw, dateStr, dayStart) {
        clearTimeline();
        state.dayStart = dayStart;

        const json = (raw && typeof raw === 'object' && !Array.isArray(raw) && raw.data !== undefined)
            ? raw.data : raw;

        if (state.consoleLogEnabled) console.log('[Timeline] parsed json keys:', Object.keys(json || {}));

        let recordingHours = null;
        if (Array.isArray(json.streams) && json.streams.length > 0) {
            if (state.selectedStreamId === 'auto') {
                // Merge all streams' recording data
                json.streams.forEach(function (s) {
                    const hrs = resolveDatesArray(s && s.dates, dateStr);
                    recordingHours = _mergeRecordingHours(recordingHours, hrs);
                });
                if (state.consoleLogEnabled) console.log('[Timeline] auto mode — merged', json.streams.length, 'streams');
            } else {
                const stream = json.streams.find(function (s) { return s.streamId === state.selectedStreamId; })
                            || json.streams[0];
                if (state.consoleLogEnabled) console.log('[Timeline] stream:', stream ? stream.streamId : 'none');
                recordingHours = resolveDatesArray(stream && stream.dates, dateStr);
            }
        }
        if (state.consoleLogEnabled) console.log('[Timeline] recordingHours length:', recordingHours ? recordingHours.length : 'null');

        const motionHours = resolveDatesArray(json.motionPeriods, dateStr);

        for (let h = 0; h < 24; h++) {
            const bar  = document.querySelector('#timeline .tl-bar[data-hour="'  + h + '"]');
            const mbar = document.querySelector('#timeline .tl-mbar[data-hour="' + h + '"]');
            if (!bar) continue;
            const hStart = dayStart + h * 3600;
            if (recordingHours) renderRecordingHour(bar,  recordingHours[h], hStart);
            if (motionHours)    renderMotionHour(mbar, motionHours[h],    hStart);
        }

        // Pass timeline data to PCB timebar
        state.pcbDayStart = dayStart;
        state.pcbRecHours = recordingHours;
        state.pcbMotHours = motionHours;
        if (state.pcbRenderTimebar) state.pcbRenderTimebar();
    }

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

        if (state.consoleLogEnabled) console.log('[Timeline] fetch:', url);
        fetchBtn.disabled    = true;
        fetchBtn.textContent = '...';

        fetch(url)
            .then(function (r) { return r.json(); })
            .then(function (json) {
                if (state.consoleLogEnabled) console.log('[Timeline] raw response:', JSON.stringify(json).slice(0, 400));
                fillTimeline(json, dateStr, dayStart);
            })
            .catch(function (err) { console.warn('[Timeline] fetch error:', err); })
            .finally(function () { fetchBtn.disabled = false; fetchBtn.textContent = 'Load'; });
    }

    function _triggerTimelineRefresh() {
        if (dateInput.value) fetchTimeline();
    }
    state.triggerTimelineRefresh = _triggerTimelineRefresh;

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
}
