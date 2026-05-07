// =============================================================================
// player-controls.js - Mode switch (Live/Replay) + Stream fetch + Timeline
// Requires an S3ProPlugin instance (vp.s3pro()) passed as argument.
// Compatible with s3pro-player.js VideoJS plugin API.
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
        // In replay mode: HLS is not supported — silently switch transport to MP4
        if (m === 'replay' && _selectedTransport === 'hls') {
            _selectedTransport = 'mp4';
            _updateTransportMenu();
        }
    }
    liveBtn.addEventListener('click',   function () { setMode('live'); });
    replayBtn.addEventListener('click', function () { setMode('replay'); })

    // =========================================================================
    // Motion badge: show MJPEG motion overlay when camera supports motion
    // =========================================================================
    var _motionSupported = false; // true when camera reports enableMotion=true
    var _motionOnline    = false; // true when the device/motion stream is online
    var _motionPlaying   = false; // true while MJPEG motion overlay is shown
    var _motionOverlay   = document.getElementById('motion-overlay');
    var _BLANK_GIF       = 'data:image/gif;base64,R0lGODlhAQABAAD/ACwAAAAAAQABAAACADs=';

    function _updateMotionBadge() {
        if (!badgeMotion) return;
        var canShow = _motionSupported; // only show if feature is configured
        badgeMotion.classList.toggle('show',      canShow);
        badgeMotion.classList.toggle('motion-on', _motionPlaying);
        badgeMotion.classList.toggle('motion-offline', canShow && !_motionOnline);
        if (!canShow || !_motionOnline) {
            // If device went offline while playing, stop the overlay
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
    // Wire player events: log, fatal error, timeupdate
    // =========================================================================
    var logEl      = document.getElementById('log');
    var infoEl     = document.getElementById('info-text');
    var timeEl     = document.getElementById('time-display');
    var errOverlay = document.getElementById('player-error');
    var errMsgEl   = document.getElementById('player-error-msg');

    // Devtools console log — default OFF; enable via setConsoleLogEnabled(true)
    var _consoleLogEnabled = false;

    // Expose so external callers can do: initPlayerControls(player).setConsoleLogEnabled(true)
    var _publicApi = {
        setConsoleLogEnabled: function (enabled) { _consoleLogEnabled = !!enabled; },
    };

    player.on('log', function (level, msg) {
        // Always write to the UI log panel
        if (logEl) {
            var d = document.createElement('div');
            d.className   = 'log-' + level;
            d.textContent = msg;
            logEl.appendChild(d);
            logEl.scrollTop = logEl.scrollHeight;
        }
        // Optionally mirror to browser devtools Console
        if (_consoleLogEnabled) {
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

    // Loading overlay element (spinner shown while connecting/buffering)
    var loadingOverlay = document.getElementById('player-loading');

    // =========================================================================
    // Freeze-frame canvas: capture last video frame when stream ends
    // =========================================================================
    var freezeCanvas = document.getElementById('player-freeze');

    /** Draw current video frame onto the canvas and show it. */
    function _captureFrame() {
        var v = document.querySelector('.vjs-tech');
        if (!freezeCanvas || !v || v.videoWidth === 0 || v.readyState < 2) return false;
        freezeCanvas.width  = v.videoWidth;
        freezeCanvas.height = v.videoHeight;
        try {
            freezeCanvas.getContext('2d').drawImage(v, 0, 0);
            freezeCanvas.classList.add('show');
            return true;
        } catch (_) { return false; }
    }

    /** Hide and clear the freeze canvas. */
    function _clearFrame() {
        if (!freezeCanvas) return;
        freezeCanvas.classList.remove('show');
        // Clear pixels so stale content doesn’t flash on the next stream
        var ctx = freezeCanvas.getContext('2d');
        if (ctx) ctx.clearRect(0, 0, freezeCanvas.width, freezeCanvas.height);
    }

    player.on('statechange', function (state) {
        // Show spinner for both connecting and buffering; hide on playing/stopped/error/idle
        var isLoading = (state === 'connecting' || state === 'buffering');
        if (loadingOverlay) {
            loadingOverlay.classList.toggle('show', isLoading);
            // Buffering uses reduced opacity so the frame stays visible underneath
            loadingOverlay.classList.toggle('buffering', state === 'buffering');
        }
        // Clear error overlay when playback resumes
        if (state === 'playing' || state === 'buffering') {
            if (errOverlay) errOverlay.classList.remove('show');
        }
        // When stream ends naturally (server closed): capture last frame and show
        // stopped overlay with the frozen image underneath.
        // When a new stream starts: clear frozen frame immediately.
        if (state === 'stopped') {
            _captureFrame();
            showStoppedOverlay();
        } else if (state === 'connecting') {
            _clearFrame();
            hideStoppedOverlay();
        }
        // Update _stopped flag so the play/pause PCB button stays consistent
        if (state === 'stopped' || state === 'idle' || state === 'error') {
            _stopped = true;
        } else if (state === 'connecting' || state === 'playing' || state === 'buffering') {
            _stopped = false;
        }
    });

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

    player.on('timeupdate', function (ct, fmt, codec, bufLen) {
        var friendly = _friendlyCodec(codec);
        if (infoEl)
            infoEl.textContent = 'codec: ' + (friendly || '--') +
                                 ' | bufferLen: ' + (bufLen ? bufLen.toFixed(2) + 's' : '--');
        if (timeEl) timeEl.textContent = fmt || '00:00:00';
        if (!friendly) return; // no codec yet — skip quality menu update
        // Update Auto item realtime info (resolution + codec)
        var videoEl = document.querySelector('.vjs-tech');
        if (!videoEl) return;
        var w = videoEl.videoWidth, h = videoEl.videoHeight;
        var info = (w && h ? w + 'x' + h + ' ' : '') + friendly;
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

    // =========================================================================
    // Stream selection state
    // =========================================================================
    // 'auto' = no specific stream; otherwise contains the streamId string
    let _selectedStreamId  = 'auto';
    let _selectedTransport = 'mp4'; // 'mp4' | 'hls' | 'ws-fmp4'

    // Cached stream profiles fetched from device statistic API
    let _deviceStreamProfiles = [];

    function _getActiveStreamId() {
        if (_selectedStreamId === 'auto') return '';
        return _selectedStreamId;
    }

    // =========================================================================
    // URL builder helpers
    // =========================================================================

    /** Convert an http(s):// domain to ws(s):// for WebSocket transport. */
    function _toWsUrl(domain) {
        return domain.replace(/^https:/i, 'wss:').replace(/^http:/i, 'ws:');
    }

    function _buildLiveUrl(domain, cameraId, streamId) {
        if (_selectedTransport === 'hls') {
            if (streamId) {
                return domain + '/media/live/' + encodeURIComponent(cameraId)
                    + '/' + encodeURIComponent(streamId) + '/hls.m3u8';
            }
            // HLS master playlist — quality selection handled by the HLS manifest itself
            return domain + '/media/live/' + encodeURIComponent(cameraId) + '/hls.master.m3u8';
        }
        if (_selectedTransport === 'ws-fmp4') {
            var wsDomain = _toWsUrl(domain);
            if (streamId) {
                return wsDomain + '/media/live/' + encodeURIComponent(cameraId)
                     + '/' + encodeURIComponent(streamId) + '.live.mp4';
            }
            return wsDomain + '/media/live/' + encodeURIComponent(cameraId) + '.live2.mp4?quality=auto&prefered=hi';
        }
        if (streamId) {
            return domain + '/media/live/' + encodeURIComponent(cameraId)
                 + '/' + encodeURIComponent(streamId) + '.live.mp4';
        }
        return domain + '/media/live/' + encodeURIComponent(cameraId) + '.live2.mp4?quality=auto&prefered=hi';
    }

    function _buildVodUrl(domain, cameraId, streamId, stampSec) {
        var base = (_selectedTransport === 'ws-fmp4') ? _toWsUrl(domain) : domain;
        if (streamId) {
            return base + '/media/record/' + encodeURIComponent(cameraId)
                 + '/' + encodeURIComponent(streamId) + '/vod/' + stampSec + '.live.mp4';
        }
        return base + '/media/record/' + encodeURIComponent(cameraId)
             + '/vod/' + stampSec + '.live2.mp4?quality=auto&prefered=hi';
    }

    // =========================================================================
    // URL builder for Live mode - capture phase runs before player.play()
    // =========================================================================
    document.getElementById('btn-play').addEventListener('click', function () {
        if (_mode !== 'live') return;
        const domain   = (document.getElementById('media-domain').value || '').replace(/\/+$/, '');
        const cameraId = (document.getElementById('camera-id').value    || '').trim();
        const streamId = _getActiveStreamId();
        document.getElementById('url-input').value = _buildLiveUrl(domain, cameraId, streamId);
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
            _clearFrame();         // user explicitly stopped → clear freeze frame
            showStoppedOverlay();
        }
    }, true /* capture */);

    // Hide overlays + call player.play() whenever Play is clicked
    document.getElementById('btn-play').addEventListener('click', function () {
        _stopped = false;
        hideNoData();
        hideStoppedOverlay();
        _clearFrame();             // new stream → clear any frozen image
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
    // Device statistic fetch — populates quality menu
    // =========================================================================
    var qualStatusEl   = document.getElementById('qual-status');
    var fetchStreamsBtn = document.getElementById('btn-fetch-streams');

    function _setQualStatus(msg, isError) {
        if (!qualStatusEl) return;
        qualStatusEl.textContent = msg;
        qualStatusEl.style.color = isError ? '#ef9a9a' : '#888';
    }

    function _buildAuthHeaders() {
        var token = (document.getElementById('auth-token') || {}).value || '';
        token = token.trim();
        var headers = {};
        if (token) {
            headers['Authorization'] = token.startsWith('Bearer ') ? token : 'Bearer ' + token;
        }
        return headers;
    }

    var _statJsonEl = document.getElementById('device-stat-json');

    function _showStatJson(json) {
        if (!_statJsonEl) return;
        try {
            _statJsonEl.textContent = JSON.stringify(json, null, 2);
        } catch(e) {
            _statJsonEl.textContent = String(json);
        }
    }

    function fetchDeviceStatistic() {
        var domain   = (document.getElementById('media-domain').value || '').replace(/\/+$/, '').trim();
        var cameraId = (document.getElementById('camera-id').value    || '').trim();

        if (!domain || !cameraId) {
            _setQualStatus('Nhập Camera ID và Media Server trước', true);
            return;
        }

        _setQualStatus('Đang tải...', false);
        if (fetchStreamsBtn) { fetchStreamsBtn.disabled = true; fetchStreamsBtn.textContent = '...'; }

        var url = domain + '/media/mserver/device/statistics?id=' + encodeURIComponent(cameraId);
        fetch(url, { headers: _buildAuthHeaders() })
            .then(function (r) {
                if (!r.ok) throw new Error('HTTP ' + r.status);
                return r.json();
            })
            .then(function (json) {
                _showStatJson(json);
                var device = json && json.data ? json.data : null;
                if (!device || !device.deviceId) {
                    _setQualStatus('Không tìm thấy camera: ' + cameraId, true);
                    return;
                }
                _applyDeviceStreams(device);
            })
            .catch(function (err) {
                console.warn('[Streams] fetch error:', err);
                _setQualStatus('Lỗi: ' + (err.message || err), true);
            })
            .finally(function () {
                if (fetchStreamsBtn) { fetchStreamsBtn.disabled = false; fetchStreamsBtn.textContent = 'Load Streams'; }
            });
    }

    function _applyDeviceStreams(device) {
        var profiles = [{ id: 'auto', label: 'Auto', streamId: '', online: null }];
        _deviceStreamProfiles = [];

        function _addStream(streamData, streamIdField, label) {
            if (!streamData || !device[streamIdField]) return;
            var sid    = device[streamIdField];
            var online = streamData.status === true;
            var w      = streamData.width  || 0;
            var h      = streamData.height || 0;
            var codec  = streamData.vcodec || '?';
            var info   = (w && h ? w + 'x' + h + ' ' : '') + codec;
            profiles.push({ id: sid, label: label, streamId: sid, online: online, info: info });
            _deviceStreamProfiles.push({ id: sid, streamId: sid, online: online, info: info });
        }

        _addStream(device.primaryStream,   'primaryStreamId',   'Stream chính');
        _addStream(device.secondaryStream, 'secondaryStreamId', 'Stream phụ');

        var onlineCount = _deviceStreamProfiles.filter(function (p) { return p.online; }).length;
        var totalCount  = _deviceStreamProfiles.length;
        _setQualStatus(totalCount + ' stream' + (totalCount !== 1 ? 's' : '') + ' · ' + onlineCount + ' online', false);

        _populateQualityMenu(profiles);

        var stillValid = profiles.some(function (p) { return p.id === _selectedStreamId; });
        if (!stillValid) {
            _selectedStreamId = 'auto';
            document.getElementById('stream-id').value = '';
        }

        _triggerTimelineRefresh();
        // Motion badge: show only when enableMotion is configured AND device is online
        var enableMotion = (device.options && device.options.enableMotion === true);
        var deviceOnline = device.status === true;
        window.setMotionActive(enableMotion, deviceOnline);
    }

    function _populateQualityMenu(profiles) {
        if (!pcbQualMenu) return;
        Array.from(pcbQualMenu.querySelectorAll('.qm-item, .qm-section')).forEach(function (el) { el.remove(); });
        var checkSvg = '<svg class="qm-check" viewBox="0 0 24 24" fill="currentColor">'
            + '<path d="M9 16.17L4.83 12l-1.42 1.41L9 19 21 7l-1.41-1.41z"/></svg>';
        // -- Profile items --
        profiles.forEach(function (p) {
            var div = document.createElement('div');
            div.className = 'qm-item' + (p.id === _selectedStreamId ? ' active' : '');
            div.dataset.profile = p.id;
            var body = '';
            if (p.id === 'auto') {
                body = checkSvg + 'Auto<span class="qm-info"></span>';
            } else {
                var dotClass = p.online ? 'qm-dot qm-dot-on' : 'qm-dot qm-dot-off';
                var dotHtml  = '<span class="' + dotClass + ' qm-dot-right" title="' + (p.online ? 'Online' : 'Offline') + '"></span>';
                body = checkSvg
                    + p.label
                    + dotHtml
                    + (p.info ? '<span class="qm-info">' + p.info + '</span>' : '');
            }
            div.innerHTML = body;
            pcbQualMenu.appendChild(div);
        });
        // -- Transport section --
        var secDiv = document.createElement('div');
        secDiv.className = 'qm-section';
        secDiv.textContent = 'Transport';
        pcbQualMenu.appendChild(secDiv);
        [{ id: 'mp4', label: 'MP4 (FMP4)' }, { id: 'ws-fmp4', label: 'WS (FMP4)' }, { id: 'hls', label: 'HLS' }].forEach(function (t) {
            var div = document.createElement('div');
            div.className = 'qm-item' + (t.id === _selectedTransport ? ' active' : '');
            div.dataset.transport = t.id;
            div.innerHTML = checkSvg + t.label;
            pcbQualMenu.appendChild(div);
        });
    }

    function _updateTransportMenu() {
        if (!pcbQualMenu) return;
        pcbQualMenu.querySelectorAll('.qm-item[data-transport]').forEach(function (i) {
            i.classList.toggle('active', i.dataset.transport === _selectedTransport);
        });
    }

    window.onTransportSelect = function (transport) {
        if (transport === _selectedTransport) return;
        _selectedTransport = transport;
        _updateTransportMenu();
        if (!_stopped) {
            if (_mode === 'replay' && transport === 'hls') {
                // Show error, revert to MP4
                _selectedTransport = 'mp4';
                _updateTransportMenu();
                if (errMsgEl)   errMsgEl.textContent = 'HLS không hỗ trợ chế độ Replay — vui lòng chọn MP4 hoặc WS';
                if (errOverlay) errOverlay.classList.add('show');
                return;
            }
            if (_mode === 'live') {
                document.getElementById('btn-play').click();
            }
        }
    };

    if (fetchStreamsBtn) fetchStreamsBtn.addEventListener('click', fetchDeviceStatistic);

    // Debounced auto-fetch when camera-id or domain changes
    var _fetchDebounce = null;
    function _scheduleFetch() {
        clearTimeout(_fetchDebounce);
        _fetchDebounce = setTimeout(function () {
            var camId  = (document.getElementById('camera-id').value    || '').trim();
            var domain = (document.getElementById('media-domain').value || '').trim();
            if (camId && domain) fetchDeviceStatistic();
        }, 600);
    }
    var _camInput    = document.getElementById('camera-id');
    var _domainInput = document.getElementById('media-domain');
    if (_camInput)    _camInput.addEventListener('input',  _scheduleFetch);
    if (_domainInput) _domainInput.addEventListener('input', _scheduleFetch);

    // Profile selection handler (called by quality menu click)
    window.onProfileSelect = function (profileId) {
        if (profileId === _selectedStreamId) return; // no change
        _selectedStreamId = profileId;
        document.getElementById('stream-id').value = (profileId === 'auto') ? '' : profileId;
        _triggerTimelineRefresh();
        if (!_stopped) {
            // Auto-switch: restart stream with the new profile at the current position
            if (_mode === 'replay' && _replayStartTime > 0) {
                var videoEl = document.querySelector('.vjs-tech');
                var offset  = (videoEl && videoEl.currentTime > 0) ? videoEl.currentTime : 0;
                seekToWallTime(_replayStartTime + offset);
            } else if (_mode === 'live') {
                document.getElementById('btn-play').click();
            }
        }
    };
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

    // DTS event: update timeline cursor + PCB timebar during replay
    // dtsMs comes from moof tfdt (baseMediaDecodeTime / 90) — relative to VOD segment start
    player.on('dts', function (dtsMs /*, codec */) {
        if (_mode !== 'replay' || !_replayStartTime) return;
        var wallTime = _replayStartTime + dtsMs / 1000;
        moveCursorToWallTime(wallTime);
        _pcbMoveCursor(wallTime);
    });

    // =========================================================================
    // Seek to exact wall-clock time
    // =========================================================================
    function seekToWallTime(wallTime) {
        // HLS does not support replay/VOD in this player
        if (_selectedTransport === 'hls') {
            if (errMsgEl)   errMsgEl.textContent = 'HLS không hỗ trợ chế độ Replay — vui lòng chọn MP4';
            if (errOverlay) errOverlay.classList.add('show');
            return;
        }
        const ts       = Math.floor(wallTime);
        const domain   = (document.getElementById('media-domain').value || '').replace(/\/+$/, '');
        const cameraId = (document.getElementById('camera-id').value    || '').trim();
        const streamId = _getActiveStreamId();
        _replayStartTime = ts;
        setMode('replay');
        document.getElementById('url-input').value = _buildVodUrl(domain, cameraId, streamId, ts);
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
            console.warn('[Timeline] dates key mismatch: expected "' + dateStr + '", got "' + keys[0] + '"');
            return datesObj[keys[0]];
        }
        return null;
    }

    // Merge two 24-element arrays of period arrays (union per hour)
    function _mergeRecordingHours(hoursA, hoursB) {
        if (!hoursA && !hoursB) return null;
        if (!hoursA) return hoursB;
        if (!hoursB) return hoursA;
        var merged = [];
        for (var h = 0; h < 24; h++) {
            merged.push((hoursA[h] || []).concat(hoursB[h] || []));
        }
        return merged;
    }

    function fillTimeline(raw, dateStr, dayStart) {
        clearTimeline();
        _dayStart = dayStart;

        const json = (raw && typeof raw === 'object' && !Array.isArray(raw) && raw.data !== undefined)
            ? raw.data : raw;

        console.log('[Timeline] parsed json keys:', Object.keys(json || {}));

        let recordingHours = null;
        if (Array.isArray(json.streams) && json.streams.length > 0) {
            if (_selectedStreamId === 'auto') {
                // Merge all streams' recording data
                json.streams.forEach(function (s) {
                    var hrs = resolveDatesArray(s && s.dates, dateStr);
                    recordingHours = _mergeRecordingHours(recordingHours, hrs);
                });
                console.log('[Timeline] auto mode — merged', json.streams.length, 'streams');
            } else {
                const stream = json.streams.find(function (s) { return s.streamId === _selectedStreamId; })
                            || json.streams[0];
                console.log('[Timeline] stream:', stream ? stream.streamId : 'none');
                recordingHours = resolveDatesArray(stream && stream.dates, dateStr);
            }
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
        _pcbDayStart  = dayStart;
        _pcbRecHours  = recordingHours;
        _pcbMotHours  = motionHours;
        _pcbRenderTimebar();
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

        console.log('[Timeline] fetch:', url);
        fetchBtn.disabled    = true;
        fetchBtn.textContent = '...';

        fetch(url)
            .then(function (r) { return r.json(); })
            .then(function (json) {
                console.log('[Timeline] raw response:', JSON.stringify(json).slice(0, 400));
                fillTimeline(json, dateStr, dayStart);
            })
            .catch(function (err) { console.warn('[Timeline] fetch error:', err); })
            .finally(function () { fetchBtn.disabled = false; fetchBtn.textContent = 'Load'; });
    }

    function _triggerTimelineRefresh() {
        if (dateInput.value) fetchTimeline();
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
    var _pcbRecHours  = null;
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

    // Ensure the Auto item always exists so the codec/resolution info can be shown
    // even before device statistics are loaded.
    _populateQualityMenu([{ id: 'auto', label: 'Auto', streamId: '', online: null }]);

    var _pcbCurrentHour = -1;

    // Fill pcbTrack with recording+motion segments for hour h
    function _pcbFillTrack(h) {
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

    // Render timebar for the current replay wall time
    function _pcbRenderTimebar() {
        if (!pcbTrack || !_pcbDayStart || !_replayStartTime) return;
        var h = Math.floor((_replayStartTime - _pcbDayStart) / 3600);
        if (h < 0 || h >= 24) return;
        _pcbFillTrack(h);
    }

    // Move timebar cursor to a wall-clock time
    function _pcbMoveCursor(wallTime) {
        if (!pcbCursor || !pcbTrack) return;
        if (!_pcbDayStart) { pcbCursor.style.display = 'none'; return; }
        var h = Math.floor((wallTime - _pcbDayStart) / 3600);
        if (h !== _pcbCurrentHour) _pcbFillTrack(h);
        var hStart = _pcbDayStart + h * 3600;
        var pct = Math.max(0, Math.min(1, (wallTime - hStart) / 3600));
        pcbCursor.style.left    = (pct * 100) + '%';
        pcbCursor.style.display = 'block';
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

    // -- Play/Pause/Stop button (replaces the manual btn-play / btn-stop below) --
    if (pcbPlayPause) {
        pcbPlayPause.addEventListener('click', function () {
            var videoEl = document.querySelector('.vjs-tech');
            if (_stopped || !videoEl || videoEl.readyState === 0) {
                // Completely stopped — build URL and start stream
                document.getElementById('btn-play').click();
            } else if (videoEl.paused || videoEl.ended) {
                // Paused mid-stream (e.g. after VOD ended) — restart
                document.getElementById('btn-play').click();
            } else {
                // Currently playing — stop
                document.getElementById('btn-stop').click();
            }
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

    // -- Quality menu open/close --
    if (pcbQualBtn) {
        pcbQualBtn.addEventListener('click', function (e) {
            e.stopPropagation();
            if (pcbQualMenu) pcbQualMenu.classList.toggle('show');
        });
        document.addEventListener('click', function () {
            if (pcbQualMenu) pcbQualMenu.classList.remove('show');
        });
    }

    // -- Quality menu item click: delegate to onProfileSelect --
    if (pcbQualMenu) {
        pcbQualMenu.addEventListener('click', function (e) {
            var item = e.target.closest('.qm-item');
            if (!item) return;
            pcbQualMenu.classList.remove('show');
            if (item.dataset.transport !== undefined) {
                // Transport item
                pcbQualMenu.querySelectorAll('.qm-item[data-transport]').forEach(function (i) { i.classList.remove('active'); });
                item.classList.add('active');
                window.onTransportSelect(item.dataset.transport);
            } else if (item.dataset.profile !== undefined) {
                // Profile item
                pcbQualMenu.querySelectorAll('.qm-item[data-profile]').forEach(function (i) { i.classList.remove('active'); });
                item.classList.add('active');
                window.onProfileSelect(item.dataset.profile);
            }
        });
    }

    // Public API: populate quality menu from external code
    window.setQualityProfiles = function (profiles) {
        _populateQualityMenu(profiles);
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

        // Codec + buffer info bar — driven directly from player state (robust fallback)
        var pState = player.getState();
        if (pState.codec) {
            var friendly2 = _friendlyCodec(pState.codec);
            if (friendly2) {
                if (infoEl) {
                    var bl = pState.bufferLen || 0;
                    infoEl.textContent = 'codec: ' + friendly2
                        + ' | bufferLen: ' + (bl > 0 ? bl.toFixed(2) + 's' : '--');
                }
                var w2 = videoEl.videoWidth, h2 = videoEl.videoHeight;
                var info2 = (w2 && h2 ? w2 + 'x' + h2 + ' ' : '') + friendly2;
                var autoItem = pcbQualMenu && pcbQualMenu.querySelector('.qm-item[data-profile="auto"]');
                if (autoItem) {
                    var sp = autoItem.querySelector('.qm-info');
                    if (!sp) { sp = document.createElement('span'); sp.className = 'qm-info'; autoItem.appendChild(sp); }
                    if (sp.textContent !== info2) sp.textContent = info2;
                }
            }
        }
    }, 200);

    return _publicApi;
}
