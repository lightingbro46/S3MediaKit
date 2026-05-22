// =============================================================================
// pc-control-bar.js — PCB: custom player control bar.
//   Handles: play/pause button, volume, capture, fullscreen, quality menu
//   click delegation, timebar (mini timeline for current hour), 200ms UI poll.
// Called by initPlayerControls(). Populates state with pcbMoveCursor and
// pcbRenderTimebar so the timeline module can drive the PCB cursor.
// =============================================================================

function initControlBar(player, state) {
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
    var pcbTimebarWrap = document.getElementById('pcb-timebar-wrap');

    var _pcbCurrentHour = -1;

    // ── Timebar: fill recording+motion segments for one hour ──────────────────
    function _pcbFillTrack(h) {
        if (!pcbTrack) return;
        _pcbCurrentHour = h;
        var hStart = state.pcbDayStart + h * 3600;
        Array.from(pcbTrack.children).forEach(function (c) { pcbTrack.removeChild(c); });
        if (state.pcbRecHours && state.pcbRecHours[h]) {
            state.pcbRecHours[h].forEach(function (p) {
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
        if (state.pcbMotHours && state.pcbMotHours[h]) {
            state.pcbMotHours[h].forEach(function (p) {
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

    // Render timebar for the current replay wall time (called after timeline data loads)
    function _pcbRenderTimebar() {
        if (!pcbTrack || !state.pcbDayStart || !state.replayStartTime) return;
        var h = Math.floor((state.replayStartTime - state.pcbDayStart) / 3600);
        if (h < 0 || h >= 24) return;
        _pcbFillTrack(h);
    }

    // Move timebar cursor to a wall-clock time
    function _pcbMoveCursor(wallTime) {
        if (!pcbCursor || !pcbTrack) return;
        if (!state.pcbDayStart) { pcbCursor.style.display = 'none'; return; }
        var h = Math.floor((wallTime - state.pcbDayStart) / 3600);
        if (h !== _pcbCurrentHour) _pcbFillTrack(h);
        var hStart = state.pcbDayStart + h * 3600;
        var pct = Math.max(0, Math.min(1, (wallTime - hStart) / 3600));
        pcbCursor.style.left    = (pct * 100) + '%';
        pcbCursor.style.display = 'block';
    }

    state.pcbMoveCursor    = _pcbMoveCursor;
    state.pcbRenderTimebar = _pcbRenderTimebar;

    // ── Timebar click: seek within current hour ───────────────────────────────
    if (pcbTimebarWrap) {
        pcbTimebarWrap.addEventListener('click', function (e) {
            if (!state.pcbDayStart || _pcbCurrentHour < 0) return;
            var rect  = pcbTrack.getBoundingClientRect();
            var ratio = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));
            var wall  = state.pcbDayStart + _pcbCurrentHour * 3600 + ratio * 3600;
            // Only seek if position has recording data
            var hPeriods = state.pcbRecHours && state.pcbRecHours[_pcbCurrentHour];
            var hasData  = false;
            if (hPeriods) {
                hPeriods.forEach(function (p) {
                    if (wall >= Number(p.startTime) && wall < Number(p.startTime) + Number(p.duration)) {
                        hasData = true;
                    }
                });
            }
            if (hasData && state.seekToWallTime) state.seekToWallTime(wall);
        });

        // Hover tooltip
        pcbTimebarWrap.addEventListener('mousemove', function (e) {
            if (!state.pcbDayStart || _pcbCurrentHour < 0) return;
            var rect  = pcbTrack.getBoundingClientRect();
            var ratio = Math.max(0, Math.min(1, (e.clientX - rect.left) / rect.width));
            var wall  = state.pcbDayStart + _pcbCurrentHour * 3600 + ratio * 3600;
            var wrapRect = pcbTimebarWrap.getBoundingClientRect();
            pcbHoverTime.textContent   = new Date(wall * 1000).toLocaleTimeString();
            pcbHoverTime.style.left    = (e.clientX - wrapRect.left) + 'px';
            pcbHoverTime.style.display = 'block';
        });
        pcbTimebarWrap.addEventListener('mouseleave', function () {
            if (pcbHoverTime) pcbHoverTime.style.display = 'none';
        });
    }

    // ── Play / Pause ──────────────────────────────────────────────────────────
    if (pcbPlayPause) {
        pcbPlayPause.addEventListener('click', function () {
            if (state.stopped || player.paused) {
                document.getElementById('btn-play').click();
            } else {
                document.getElementById('btn-stop').click();
            }
        });
    }

    // ── Volume ────────────────────────────────────────────────────────────────
    var _prevVol = 0.5;

    function _pcbUpdateVolIcon(v) {
        if (!pcbIconVol || !pcbIconMute) return;
        if (v === 0) { pcbIconVol.style.display = 'none'; pcbIconMute.style.display = ''; }
        else         { pcbIconVol.style.display = '';     pcbIconMute.style.display = 'none'; }
    }

    if (pcbVolBtn) {
        pcbVolBtn.addEventListener('click', function () {
            if (pcbVolWrap) pcbVolWrap.classList.toggle('show');
        });
        // Double-click = toggle mute
        pcbVolBtn.addEventListener('dblclick', function () {
            if (player.muted || player.volume === 0) {
                player.muted  = false;
                player.volume = _prevVol || 0.5;
                if (pcbVolSlider) pcbVolSlider.value = player.volume;
                _pcbUpdateVolIcon(player.volume);
            } else {
                _prevVol = player.volume;
                player.muted = true;
                if (pcbVolSlider) pcbVolSlider.value = 0;
                _pcbUpdateVolIcon(0);
            }
        });
    }

    if (pcbVolSlider) {
        pcbVolSlider.addEventListener('input', function () {
            var v = parseFloat(pcbVolSlider.value);
            player.volume = v || 0.001;
            player.muted  = (v === 0);
            if (v > 0) _prevVol = v;
            _pcbUpdateVolIcon(v);
        });
    }

    // ── Capture (snapshot) ────────────────────────────────────────────────────
    if (pcbCapture) {
        pcbCapture.addEventListener('click', function () {
            var dataUrl = player.snapshot();
            if (!dataUrl) return;
            var ts  = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
            var cam = (document.getElementById('camera-id').value || 'cam').trim();
            var a   = document.createElement('a');
            a.download = 'capture-' + cam + '-' + ts + '.png';
            a.href     = dataUrl;
            a.click();
        });
    }

    // ── Fullscreen ────────────────────────────────────────────────────────────
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

    // ── Quality menu open / close ─────────────────────────────────────────────
    if (pcbQualBtn) {
        pcbQualBtn.addEventListener('click', function (e) {
            e.stopPropagation();
            if (pcbQualMenu) pcbQualMenu.classList.toggle('show');
        });
        document.addEventListener('click', function () {
            if (pcbQualMenu) pcbQualMenu.classList.remove('show');
        });
    }

    // ── Quality menu item click: delegate to onProfileSelect / onTransportSelect
    if (pcbQualMenu) {
        pcbQualMenu.addEventListener('click', function (e) {
            var item = e.target.closest('.qm-item');
            if (!item) return;
            pcbQualMenu.classList.remove('show');
            if (item.dataset.transport !== undefined) {
                pcbQualMenu.querySelectorAll('.qm-item[data-transport]').forEach(function (i) { i.classList.remove('active'); });
                item.classList.add('active');
                window.onTransportSelect(item.dataset.transport);
            } else if (item.dataset.profile !== undefined) {
                pcbQualMenu.querySelectorAll('.qm-item[data-profile]').forEach(function (i) { i.classList.remove('active'); });
                item.classList.add('active');
                window.onProfileSelect(item.dataset.profile);
            }
        });
    }

    // ── Fallback elapsed timer: used when DTS packets are unavailable ─────────
    // Records wall-clock time when the FIRST FRAME is displayed so we can show
    // approximate elapsed time even if readTfdt / onDts never fires.
    // _playerPlaying gates ALL time display and cursor movement so that the
    // timebar and timeline cursor do not advance before the first frame is shown.
    var _localPlayTs    = 0;
    var _playerPlaying  = false;
    player.on('statechange', function (s) {
        if (s === 'playing') {
            _playerPlaying = true;
            if (_localPlayTs === 0) _localPlayTs = Date.now(); // set once on first 'playing'
        } else if (s === 'stopped' || s === 'error' || s === 'idle' || s === 'connecting') {
            _playerPlaying = false;
            _localPlayTs   = 0;
        }
    });

    // ── 200ms poll: sync play/pause icon, volume, time display, cursors ───────
    setInterval(function () {
        // Play/Pause icon
        if (pcbIconPlay && pcbIconPause) {
            if (player.paused) { pcbIconPlay.style.display = ''; pcbIconPause.style.display = 'none'; }
            else               { pcbIconPlay.style.display = 'none'; pcbIconPause.style.display = ''; }
        }

        // Volume icon sync (in case browser muted externally)
        if (pcbVolSlider && !pcbVolSlider._dragging) {
            var vol = player.muted ? 0 : player.volume;
            pcbVolSlider.value = vol;
            _pcbUpdateVolIcon(vol);
        }

        // Elapsed ms: prefer DTS-based interpolation (accurate); fall back to
        // wall-clock delta when no DTS has been received yet.
        // Gate on _playerPlaying so the display and cursors stay at 00:00:00
        // until the first frame is actually visible on screen.
        var elapsedMs = 0;
        if (_playerPlaying) {
            elapsedMs = player.elapsedMs;
            if (elapsedMs === 0 && _localPlayTs > 0) elapsedMs = Date.now() - _localPlayTs;
        }

        // Timebar always shows player elapsed time (hh:mm:ss) regardless of mode
        if (pcbTimeEl) {
            var sec = Math.floor(elapsedMs / 1000);
            var hh = Math.floor(sec / 3600), mm = Math.floor((sec % 3600) / 60), ss = sec % 60;
            pcbTimeEl.textContent = String(hh).padStart(2,'0') + ':' + String(mm).padStart(2,'0') + ':' + String(ss).padStart(2,'0');
        }

        // Move timeline/PCB cursors with wall time in replay mode
        if (_playerPlaying && state.mode === 'replay' && state.replayStartTime) {
            var wallTime = state.replayStartTime + elapsedMs / 1000;
            if (state.dayStart && state.moveCursorToWallTime) state.moveCursorToWallTime(wallTime);
            if (state.pcbDayStart) _pcbMoveCursor(wallTime);
        }
    }, 200);
}
