// =============================================================================
// pc-stream-select.js — Stream selection, device statistics API, URL builders,
//                       quality/transport menu.
// Called by initPlayerControls(). Populates state with URL builders and menu helpers.
// =============================================================================

function initStreamSelect(player, state) {
    var qualStatusEl   = document.getElementById('qual-status');
    var fetchStreamsBtn = document.getElementById('btn-fetch-streams');
    var _statJsonEl    = document.getElementById('device-stat-json');
    var _deviceStreamProfiles = [];

    // ── Status helper ─────────────────────────────────────────────────────────
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

    function _showStatJson(json) {
        if (!_statJsonEl) return;
        try {
            _statJsonEl.textContent = JSON.stringify(json, null, 2);
        } catch (e) {
            _statJsonEl.textContent = String(json);
        }
    }

    // ── URL helpers ───────────────────────────────────────────────────────────
    function _toWsUrl(domain) {
        return domain.replace(/^https:/i, 'wss:').replace(/^http:/i, 'ws:');
    }

    function _getActiveStreamId() {
        return state.selectedStreamId === 'auto' ? '' : state.selectedStreamId;
    }

    function _buildLiveUrl(domain, cameraId, streamId) {
        if (state.selectedTransport === 'hls') {
            if (streamId) {
                return domain + '/media/live/' + encodeURIComponent(cameraId)
                    + '/' + encodeURIComponent(streamId) + '/hls.m3u8';
            }
            return domain + '/media/live/' + encodeURIComponent(cameraId) + '/hls.master.m3u8';
        }
        if (state.selectedTransport === 'ws-fmp4') {
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
        var base = (state.selectedTransport === 'ws-fmp4') ? _toWsUrl(domain) : domain;
        if (streamId) {
            return base + '/media/record/' + encodeURIComponent(cameraId)
                 + '/' + encodeURIComponent(streamId) + '/vod/' + stampSec + '.live.mp4';
        }
        return base + '/media/record/' + encodeURIComponent(cameraId)
             + '/vod/' + stampSec + '.live2.mp4?quality=auto';
    }

    state.getActiveStreamId = _getActiveStreamId;
    state.buildLiveUrl      = _buildLiveUrl;
    state.buildVodUrl       = _buildVodUrl;

    // ── Quality / transport menu ──────────────────────────────────────────────
    function _populateQualityMenu(profiles) {
        var pcbQualMenu = document.getElementById('pcb-quality-menu');
        if (!pcbQualMenu) return;
        Array.from(pcbQualMenu.querySelectorAll('.qm-item, .qm-section')).forEach(function (el) { el.remove(); });
        var checkSvg = '<svg class="qm-check" viewBox="0 0 24 24" fill="currentColor">'
            + '<path d="M9 16.17L4.83 12l-1.42 1.41L9 19 21 7l-1.41-1.41z"/></svg>';
        profiles.forEach(function (p) {
            var div = document.createElement('div');
            div.className = 'qm-item' + (p.id === state.selectedStreamId ? ' active' : '');
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
        var secDiv = document.createElement('div');
        secDiv.className = 'qm-section';
        secDiv.textContent = 'Transport';
        pcbQualMenu.appendChild(secDiv);
        [{ id: 'mp4', label: 'HTTP (FMP4)' }, { id: 'ws-fmp4', label: 'WS (FMP4)' }, { id: 'hls', label: 'HLS' }].forEach(function (t) {
            var div = document.createElement('div');
            div.className = 'qm-item' + (t.id === state.selectedTransport ? ' active' : '');
            div.dataset.transport = t.id;
            div.innerHTML = checkSvg + t.label;
            pcbQualMenu.appendChild(div);
        });
    }

    function _updateTransportMenu() {
        var pcbQualMenu = document.getElementById('pcb-quality-menu');
        if (!pcbQualMenu) return;
        pcbQualMenu.querySelectorAll('.qm-item[data-transport]').forEach(function (i) {
            i.classList.toggle('active', i.dataset.transport === state.selectedTransport);
        });
    }

    state.populateQualityMenu = _populateQualityMenu;
    state.updateTransportMenu = _updateTransportMenu;

    // ── Device statistics ─────────────────────────────────────────────────────
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

        var stillValid = profiles.some(function (p) { return p.id === state.selectedStreamId; });
        if (!stillValid) {
            state.selectedStreamId = 'auto';
            document.getElementById('stream-id').value = '';
        }

        if (state.triggerTimelineRefresh) state.triggerTimelineRefresh();

        // Motion badge: show only when enableMotion is configured AND device is online
        var enableMotion = (device.options && device.options.enableMotion === true);
        var deviceOnline = device.status === true;
        window.setMotionActive(enableMotion, deviceOnline);
    }

    // ── Transport / profile selection (called by quality menu clicks) ──────────
    window.onTransportSelect = function (transport) {
        if (transport === state.selectedTransport) return;
        state.selectedTransport = transport;
        _updateTransportMenu();
        if (!state.stopped) {
            if (state.mode === 'replay' && transport === 'hls') {
                // Revert: HLS not supported in replay mode
                state.selectedTransport = 'mp4';
                _updateTransportMenu();
                if (state.errMsgEl)   state.errMsgEl.textContent = 'HLS không hỗ trợ chế độ Replay — vui lòng chọn MP4 hoặc WS';
                if (state.errOverlay) state.errOverlay.classList.add('show');
                return;
            }
            if (state.mode === 'live') {
                document.getElementById('btn-play').click();
            } else if (state.mode === 'replay' && state.replayStartTime > 0) {
                if (state.seekToWallTime) state.seekToWallTime(state.replayStartTime + player.elapsedMs / 1000);
            }
        }
    };

    window.onProfileSelect = function (profileId) {
        if (profileId === state.selectedStreamId) return;
        state.selectedStreamId = profileId;
        document.getElementById('stream-id').value = (profileId === 'auto') ? '' : profileId;
        if (state.triggerTimelineRefresh) state.triggerTimelineRefresh();
        if (!state.stopped) {
            if (state.mode === 'replay' && state.replayStartTime > 0) {
                if (state.seekToWallTime) state.seekToWallTime(state.replayStartTime + player.elapsedMs / 1000);
            } else if (state.mode === 'live') {
                document.getElementById('btn-play').click();
            }
        }
    };

    // ── btn-play capture handler: build live URL before player.play() fires ───
    document.getElementById('btn-play').addEventListener('click', function () {
        if (state.mode !== 'live') return;
        var domain   = (document.getElementById('media-domain').value || '').replace(/\/+$/, '');
        var cameraId = (document.getElementById('camera-id').value    || '').trim();
        var streamId = _getActiveStreamId();
        document.getElementById('url-input').value = _buildLiveUrl(domain, cameraId, streamId);
    }, true /* capture phase — runs before the coordinator's bubble-phase handler */);

    // ── Fetch button + debounced auto-fetch on input change ───────────────────
    if (fetchStreamsBtn) fetchStreamsBtn.addEventListener('click', fetchDeviceStatistic);

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

    // Public API: external code may call window.setQualityProfiles(profiles)
    window.setQualityProfiles = function (profiles) { _populateQualityMenu(profiles); };

    // Seed quality menu with Auto item so codec info can appear before device stats load
    _populateQualityMenu([{ id: 'auto', label: 'Auto', streamId: '', online: null }]);
}
