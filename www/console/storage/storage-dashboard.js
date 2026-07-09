/* Storage Tier Dashboard */
(function () {
    'use strict';

    var _state = {
        data: null,
        summary: null,
        alerts: [],
        restoreJobs: [],
        tieringJobs: [],
        expiredSegments: [],
        protectedVideos: [],
        cameras: [],
        poolOptions: {},
        mountPoints: [],
        nasMountPoints: [],
        effectivePolicies: [],
        cameraSummary: null,
        cameraTimeline: null,
        selectedCameraId: '',
        chart: null,
        loading: false,
        saving: false
    };

    function _esc(s) {
        return String(s == null ? '' : s)
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;')
            .replace(/"/g, '&quot;');
    }

    function _num(v) {
        var n = parseFloat(v);
        return isFinite(n) ? n : 0;
    }

    function _fmtBytes(v) {
        var n = _num(v);
        if (n >= 1099511627776) return (n / 1099511627776).toFixed(2) + ' TB';
        if (n >= 1073741824) return (n / 1073741824).toFixed(1) + ' GB';
        if (n >= 1048576) return (n / 1048576).toFixed(0) + ' MB';
        if (n >= 1024) return (n / 1024).toFixed(0) + ' KB';
        return n.toFixed(0) + ' B';
    }

    function _fmtTs(sec) {
        var n = parseInt(sec, 10) || 0;
        if (!n) return '-';
        return new Date(n * 1000).toLocaleString();
    }

    function _dateInputValue(sec) {
        var d = new Date((parseInt(sec, 10) || 0) * 1000);
        if (!d.getTime()) d = new Date();
        d.setMinutes(d.getMinutes() - d.getTimezoneOffset());
        return d.toISOString().slice(0, 16);
    }

    function _dateInputSeconds(value) {
        if (!value) return 0;
        var ms = new Date(value).getTime();
        return isFinite(ms) ? Math.floor(ms / 1000) : 0;
    }

    function _statusClass(status) {
        var s = String(status || '').toUpperCase();
        if (s === 'OK' || s === 'DONE') return 'std-ok';
        if (s === 'WARNING' || s === 'HIGH' || s === 'PENDING' || s === 'RUNNING') return 'std-warn';
        if (s === 'CRITICAL' || s === 'OFFLINE' || s === 'FAILED') return 'std-danger';
        return 'std-muted';
    }

    function _tierBadge(tier) {
        return '<span class="std-tier std-tier-' + _esc(String(tier || '').toLowerCase()) + '">' + _esc(tier || '-') + '</span>';
    }

    function _renderShell() {
        var el = document.getElementById('storage-tier-content');
        if (!el) return;
        el.innerHTML =
            '<div class="std-head">' +
                '<div class="std-title-block">' +
                    '<div class="std-title">Storage Tier Dashboard</div>' +
                    '<div class="std-subtitle">Pool, policy, camera assignment và job movement</div>' +
                '</div>' +
                '<button class="std-refresh" id="std-refresh-btn">' +
                    '<svg viewBox="0 0 24 24" fill="currentColor"><path d="M17.65 6.35C16.2 4.9 14.21 4 12 4c-4.42 0-8 3.58-8 8h2c0-3.31 2.69-6 6-6 1.66 0 3.14.69 4.22 1.78L13 11h8V3l-3.35 3.35zM6.35 17.65C7.8 19.1 9.79 20 12 20c4.42 0 8-3.58 8-8h-2c0 3.31-2.69 6-6 6-1.66 0-3.14-.69-4.22-1.78L11 13H3v8l3.35-3.35z"/></svg>' +
                    'Refresh' +
                '</button>' +
            '</div>' +
            '<div id="std-status" class="std-status">Đang tải...</div>' +
            '<div id="std-modal-root"></div>' +
            '<div id="std-body" style="display:none">' +
                '<div class="std-kpis" id="std-kpis"></div>' +
                '<div class="std-main-grid">' +
                    '<div class="std-panel std-chart-panel">' +
                        '<div class="std-panel-title">Dung lượng theo tier</div>' +
                        '<div id="std-tier-chart" class="std-chart"></div>' +
                    '</div>' +
                    '<div class="std-panel">' +
                        '<div class="std-panel-title">Job đang chạy</div>' +
                        '<div id="std-job-counts" class="std-job-counts"></div>' +
                    '</div>' +
                '</div>' +
                '<div class="std-split">' +
                    '<div class="std-panel">' +
                        '<div class="std-panel-title">Tier health</div>' +
                        '<div id="std-tier-summary"></div>' +
                    '</div>' +
                    '<div class="std-panel">' +
                        '<div class="std-panel-title">Storage alerts</div>' +
                        '<div id="std-alerts"></div>' +
                    '</div>' +
                '</div>' +
                '<div class="std-panel">' +
                    '<div class="std-panel-head"><div class="std-panel-title">Pools</div><button class="std-btn std-btn-primary" data-std-action="pool-create">Create pool</button></div>' +
                    '<div id="std-pools"></div>' +
                '</div>' +
                '<div class="std-split">' +
                    '<div class="std-panel">' +
                        '<div class="std-panel-head"><div class="std-panel-title">Policies</div><button class="std-btn std-btn-primary" data-std-action="policy-create">Create policy</button></div>' +
                        '<div id="std-policies"></div>' +
                    '</div>' +
                    '<div class="std-panel">' +
                        '<div class="std-panel-head"><div class="std-panel-title">Camera được cấu hình</div><button class="std-btn std-btn-primary" data-std-action="assign-open">Assign camera</button></div>' +
                        '<div id="std-cameras"></div>' +
                    '</div>' +
                '</div>' +
                '<div class="std-panel">' +
                    '<div class="std-panel-title">Camera storage detail</div>' +
                    '<div id="std-camera-storage"></div>' +
                '</div>' +
                '<div class="std-panel">' +
                    '<div class="std-panel-title">Recent tiering jobs</div>' +
                    '<div id="std-jobs"></div>' +
                '</div>' +
                '<div class="std-split">' +
                    '<div class="std-panel">' +
                        '<div class="std-panel-title">Restore jobs</div>' +
                        '<div id="std-restore-jobs"></div>' +
                    '</div>' +
                    '<div class="std-panel">' +
                        '<div class="std-panel-title">Expired segments</div>' +
                        '<div id="std-expired-segments"></div>' +
                    '</div>' +
                '</div>' +
                '<div class="std-panel">' +
                    '<div class="std-panel-title">Protected video</div>' +
                    '<div id="std-protected-videos"></div>' +
                '</div>' +
            '</div>';

        var btn = document.getElementById('std-refresh-btn');
        if (btn) btn.addEventListener('click', _load);
        el.addEventListener('click', _handleClick);
    }

    function _setStatus(kind, msg) {
        var s = document.getElementById('std-status');
        if (!s) return;
        s.className = 'std-status ' + (kind ? 'std-status-' + kind : '');
        s.textContent = msg || '';
        s.style.display = msg ? '' : 'none';
    }

    function _load() {
        if (_state.loading) return;
        _state.loading = true;
        _setStatus('', 'Đang tải...');
        var btn = document.getElementById('std-refresh-btn');
        if (btn) btn.disabled = true;

        Promise.all([
            S3Auth.apiPost('/media/api/storage/dashboard/detail', {}).then(function (res) { return res.data || {}; }),
            S3Auth.apiPost('/media/api/storage/dashboard/summary', {}).then(function (res) { return res.data || {}; }),
            _api('/media/api/storage/alert/list', { page: 0, size: 20 }),
            _api('/media/api/storage/restoreJob/list', { page: 0, size: 10 }),
            _api('/media/api/storage/tieringJob/list', { page: 0, size: 10 }),
            _api('/media/api/storage/expiredSegment/list', { page: 0, size: 10 }),
            _api('/media/api/storage/protected/list', { page: 0, size: 10 }),
            _api('/media/api/device/statisticsList', {}),
            _api('/media/api/storage/pool/options', {}),
            _api('/media/api/storage/mountpoint/available', {}),
            _api('/media/api/storage/mountpoint/available', { include: 'NAS' }),
            _api('/media/api/storage/camera/effectivePolicy/list', {}),
            _api('/media/api/storage/pool/list', {})
        ]).then(function (res) {
                _state.data = res[0] || {};
                _state.summary = res[1] || {};
                _state.alerts = (res[2] && res[2].items) || [];
                _state.restoreJobs = (res[3] && res[3].items) || [];
                _state.tieringJobs = (res[4] && res[4].items) || [];
                _state.expiredSegments = (res[5] && res[5].items) || [];
                _state.protectedVideos = (res[6] && res[6].items) || [];
                _state.cameras = Array.isArray(res[7]) ? res[7] : [];
                _state.poolOptions = (res[8] && res[8].poolsTypeSupport) || {};
                _state.mountPoints = (res[9] && res[9].mount_point) || [];
                _state.nasMountPoints = (res[10] && res[10].mount_point) || [];
                _state.effectivePolicies = (res[11] && res[11].items) || [];
                if (Array.isArray(res[12])) _state.data.pools = res[12];
                _renderData(_state.data, _state.summary);
                _setStatus('ok', 'Cập nhật lúc ' + new Date().toLocaleTimeString());
            })
            .catch(function (err) {
                _setStatus('err', (err && err.message) || 'Không tải được storage dashboard');
            })
            .finally(function () {
                _state.loading = false;
                if (btn) btn.disabled = false;
            });
    }

    function _api(path, body) {
        return S3Auth.apiPost(path, body).then(function (res) {
            return res.data || {};
        }).catch(function () {
            return {};
        });
    }

    function _apiStrict(path, body) {
        return S3Auth.apiPost(path, body || {}).then(function (res) {
            return res.data || {};
        });
    }

    function _renderData(data, docSummary) {
        var body = document.getElementById('std-body');
        if (body) body.style.display = '';
        var summary = docSummary || {};
        _renderKpis(summary, data);
        _renderTierChart(summary);
        _renderJobCounts(data.job_counts || {}, summary);
        _renderTierSummary(summary.tier_summary || []);
        _renderAlerts(_state.alerts, summary.alerts || []);
        _renderPools(data.pools || []);
        _renderPolicies(data.policies || []);
        _renderCameras(data.configured_cameras || []);
        _renderCameraStoragePanel();
        _renderJobs(_state.tieringJobs);
        _renderRestoreJobs(_state.restoreJobs);
        _renderExpiredSegments(_state.expiredSegments);
        _renderProtectedVideos(_state.protectedVideos);
    }

    function _handleClick(ev) {
        var btn = ev.target.closest('[data-std-action]');
        if (!btn) return;
        var action = btn.getAttribute('data-std-action');
        var id = btn.getAttribute('data-id') || '';
        var cameraId = btn.getAttribute('data-camera-id') || '';
        if (action === 'modal-close') return _closeModal();
        if (action === 'pool-create') return _openPoolModal();
        if (action === 'pool-edit') return _openPoolModal(_findById((_state.data && _state.data.pools) || [], id));
        if (action === 'pool-delete') return _deletePool(id);
        if (action === 'policy-create') return _openPolicyModal();
        if (action === 'policy-edit') return _openPolicyModal(_findById((_state.data && _state.data.policies) || [], id));
        if (action === 'policy-delete') return _deletePolicy(id);
        if (action === 'assign-open') return _openAssignModal();
        if (action === 'camera-remove') return _removeCamera(cameraId);
        if (action === 'camera-storage-load') return _loadCameraStorage();
        if (action === 'playback-resolve') return _resolvePlayback(btn);
        if (action === 'expired-approve') return _approveExpiredRange(id);
        if (action === 'expired-extend') return _extendExpiredRange(id);
    }

    function _findById(list, id) {
        return (list || []).filter(function (x) { return x && x.id === id; })[0] || null;
    }

    function _renderKpis(summary, data) {
        var pools = data.pools || [];
        var policies = data.policies || [];
        var cameras = data.configured_cameras || [];
        var total = _num(summary.total_bytes);
        var used = _num(summary.used_bytes);
        var free = _num(summary.free_bytes || Math.max(0, total - used));
        var usedPct = summary.used_percent != null ? _num(summary.used_percent) : (total > 0 ? used * 100 / total : 0);
        var html = [
            _kpi('Total used', _fmtBytes(used), _fmtBytes(total), 'std-blue'),
            _kpi('Free', _fmtBytes(free), 'available', 'std-green'),
            _kpi('Usage', usedPct.toFixed(1) + '%', summary.status || 'OK', _statusClass(summary.status)),
            _kpi('Pools', String(pools.length), (pools.filter(function (p) { return p.enabled; }).length) + ' enabled', 'std-green'),
            _kpi('Policies', String(policies.length), (policies.filter(function (p) { return p.enabled; }).length) + ' enabled', 'std-amber'),
            _kpi('Configured cameras', String(cameras.length), 'policy assignments', 'std-purple'),
            _kpi('Tiering jobs', String(summary.active_tiering_jobs || 0), (summary.failed_tiering_jobs || 0) + ' failed', 'std-pink'),
            _kpi('Restore jobs', String(summary.active_restore_jobs || 0), 'active restore', 'std-purple')
        ].join('');
        document.getElementById('std-kpis').innerHTML = html;
    }

    function _kpi(label, value, sub, cls) {
        return '<div class="std-kpi">' +
            '<div class="std-kpi-label">' + _esc(label) + '</div>' +
            '<div class="std-kpi-value ' + cls + '">' + _esc(value) + '</div>' +
            '<div class="std-kpi-sub">' + _esc(sub || '') + '</div>' +
        '</div>';
    }

    function _renderTierChart(summary) {
        var el = document.getElementById('std-tier-chart');
        if (!el || !window.echarts) return;
        if (!_state.chart) _state.chart = echarts.init(el, null, { renderer: 'canvas' });
        var tiers = _tierMap(summary.tier_summary || []);
        var names = ['HOT', 'WARM', 'COLD'];
        _state.chart.setOption({
            backgroundColor: 'transparent',
            tooltip: {
                trigger: 'axis',
                backgroundColor: '#161b22',
                borderColor: '#30363d',
                textStyle: { color: '#e6edf3', fontSize: 12 },
                valueFormatter: function (v) { return _fmtBytes(v); }
            },
            grid: { left: 48, right: 18, top: 24, bottom: 28 },
            xAxis: { type: 'category', data: names, axisTick: { show: false }, axisLine: { lineStyle: { color: '#30363d' } }, axisLabel: { color: '#7d8590' } },
            yAxis: { type: 'value', axisLabel: { color: '#7d8590', formatter: function (v) { return _fmtBytes(v); } }, splitLine: { lineStyle: { color: 'rgba(48,54,61,0.65)' } } },
            series: [
                { name: 'Used', type: 'bar', stack: 'cap', barWidth: 28, itemStyle: { color: '#388bfd' }, data: names.map(function (n) { return _num(tiers[n] && tiers[n].used_bytes); }) },
                { name: 'Free', type: 'bar', stack: 'cap', barWidth: 28, itemStyle: { color: '#30363d' }, data: names.map(function (n) {
                    var t = tiers[n] || {};
                    return Math.max(0, _num(t.total_bytes) - _num(t.used_bytes));
                }) }
            ]
        }, true);
        _state.chart.resize();
    }

    function _renderJobCounts(counts, summary) {
        var running = counts.RUNNING || 0;
        var pending = counts.PENDING || 0;
        if (!running && !pending && summary.active_tiering_jobs != null) {
            running = summary.active_tiering_jobs;
        }
        var items = [
            ['RUNNING', running],
            ['PENDING', pending],
            ['DONE', counts.DONE || 0],
            ['FAILED', counts.FAILED || 0],
            ['CANCELLED', counts.CANCELLED || 0],
            ['RESTORE', summary.active_restore_jobs || 0]
        ];
        document.getElementById('std-job-counts').innerHTML = items.map(function (it) {
            return '<div class="std-job-pill ' + _statusClass(it[0]) + '"><span>' + it[0] + '</span><b>' + it[1] + '</b></div>';
        }).join('');
    }

    function _tierMap(list) {
        var ret = {};
        (list || []).forEach(function (t) { ret[t.tier] = t; });
        return ret;
    }

    function _renderTierSummary(tiers) {
        if (!tiers.length) {
            document.getElementById('std-tier-summary').innerHTML = '<div class="std-empty">Chưa có thống kê tier</div>';
            return;
        }
        var rows = tiers.map(function (t) {
            return '<tr>' +
                '<td>' + _tierBadge(t.tier) + '</td>' +
                '<td><span class="std-status-chip ' + _statusClass(t.status) + '">' + _esc(t.status || 'OK') + '</span></td>' +
                '<td><div class="std-meter"><span style="width:' + Math.min(100, _num(t.used_percent)).toFixed(1) + '%"></span></div><div class="std-meter-label">' + _num(t.used_percent).toFixed(1) + '% · ' + _fmtBytes(t.used_bytes) + ' / ' + _fmtBytes(t.total_bytes) + '</div></td>' +
                '<td class="std-num">' + _num(t.estimated_remaining_days).toFixed(1) + '</td>' +
                '<td class="std-num">' + _num(t.write_mbps).toFixed(0) + '</td>' +
            '</tr>';
        }).join('');
        document.getElementById('std-tier-summary').innerHTML = _table(
            ['Tier', 'Status', 'Usage', 'Remain days', 'Write Mbps'], rows);
    }

    function _renderAlerts(alerts, summaryAlerts) {
        var list = alerts.length ? alerts : (summaryAlerts || []);
        if (!list.length) {
            document.getElementById('std-alerts').innerHTML = '<div class="std-empty">Không có cảnh báo storage</div>';
            return;
        }
        var rows = list.map(function (a) {
            return '<tr>' +
                '<td><span class="std-status-chip ' + _statusClass(a.level) + '">' + _esc(a.level || '-') + '</span></td>' +
                '<td><div class="std-name">' + _esc(a.message || '-') + '</div><div class="std-id">' + _esc(a.type || '') + '</div></td>' +
                '<td>' + _esc(a.pool_name || a.pool_id || '-') + '</td>' +
                '<td>' + _esc(a.acknowledged ? 'Acked' : 'Open') + '</td>' +
                '<td>' + _fmtTs(a.created_at) + '</td>' +
            '</tr>';
        }).join('');
        document.getElementById('std-alerts').innerHTML = _table(
            ['Level', 'Message', 'Pool', 'State', 'Created'], rows);
    }

    function _renderPools(pools) {
        if (!pools.length) {
            document.getElementById('std-pools').innerHTML = '<div class="std-empty">Chưa có pool nào</div>';
            return;
        }
        var rows = pools.map(function (p) {
            return '<tr>' +
                '<td><div class="std-name">' + _esc(p.name || p.id) + '</div><div class="std-id">' + _esc(p.id) + '</div></td>' +
                '<td>' + _tierBadge(p.tier) + '</td>' +
                '<td>' + _esc(p.type) + '</td>' +
                '<td><span class="std-status-chip ' + _statusClass(p.health_status) + '">' + _esc(p.health_status || 'OK') + '</span></td>' +
                '<td><div class="std-meter"><span style="width:' + Math.min(100, _num(p.usage_pct)).toFixed(1) + '%"></span></div><div class="std-meter-label">' + _num(p.usage_pct).toFixed(1) + '% · ' + _fmtBytes(p.used_bytes) + ' / ' + _fmtBytes(p.total_bytes) + '</div></td>' +
                '<td>' + _esc(p.enabled ? 'Enabled' : 'Disabled') + '</td>' +
                '<td>' + _fmtTs(p.last_health_check) + '</td>' +
                '<td><div class="std-actions"><button class="std-icon-btn" data-std-action="pool-edit" data-id="' + _esc(p.id) + '">Edit</button><button class="std-icon-btn std-danger-btn" data-std-action="pool-delete" data-id="' + _esc(p.id) + '">Delete</button></div></td>' +
            '</tr>';
        }).join('');
        document.getElementById('std-pools').innerHTML = _table(
            ['Pool', 'Tier', 'Type', 'Health', 'Usage', 'State', 'Last check', 'Actions'], rows);
    }

    function _renderPolicies(policies) {
        if (!policies.length) {
            document.getElementById('std-policies').innerHTML = '<div class="std-empty">Chưa có policy nào</div>';
            return;
        }
        var rows = policies.map(function (p) {
            var tiers = (p.tiers || []).filter(function (t) { return t.enabled !== false; }).map(function (t) { return _tierBadge(t.tier); }).join(' ');
            return '<tr>' +
                '<td><div class="std-name">' + _esc(p.name || p.id) + '</div><div class="std-id">' + _esc(p.id) + '</div></td>' +
                '<td>' + _esc(p.enabled ? 'Enabled' : 'Disabled') + '</td>' +
                '<td>' + _esc(p.total_retention_days || 0) + ' ngày</td>' +
                '<td>' + tiers + '</td>' +
                '<td class="std-num">' + _esc(p.camera_count || 0) + '</td>' +
                '<td><div class="std-actions"><button class="std-icon-btn" data-std-action="policy-edit" data-id="' + _esc(p.id) + '">Edit</button><button class="std-icon-btn std-danger-btn" data-std-action="policy-delete" data-id="' + _esc(p.id) + '">Delete</button></div></td>' +
            '</tr>';
        }).join('');
        document.getElementById('std-policies').innerHTML = _table(
            ['Policy', 'State', 'Retention', 'Tiers', 'Cameras', 'Actions'], rows);
    }

    function _renderCameras(cameras) {
        if (!cameras.length) {
            document.getElementById('std-cameras').innerHTML = '<div class="std-empty">Chưa có camera được gán policy</div>';
            return;
        }
        var rows = cameras.map(function (c) {
            return '<tr>' +
                '<td><div class="std-name">' + _esc(c.camera_name || c.camera_id) + '</div><div class="std-id">' + _esc(c.camera_id) + '</div></td>' +
                '<td>' + _esc(c.camera_ip || '-') + '</td>' +
                '<td><div class="std-name">' + _esc(c.policy_name || c.policy_id) + '</div><div class="std-id">' + _esc(c.policy_id) + '</div></td>' +
                '<td>' + _esc(c.policy_enabled ? 'Enabled' : 'Disabled') + '</td>' +
                '<td>' + _fmtTs(c.assigned_at) + '</td>' +
                '<td><button class="std-icon-btn std-danger-btn" data-std-action="camera-remove" data-camera-id="' + _esc(c.camera_id) + '">Remove</button></td>' +
            '</tr>';
        }).join('');
        document.getElementById('std-cameras').innerHTML = _table(
            ['Camera', 'IP', 'Policy áp dụng', 'Policy state', 'Assigned', 'Actions'], rows);
    }

    function _renderCameraStoragePanel() {
        var el = document.getElementById('std-camera-storage');
        if (!el) return;
        var now = Math.floor(Date.now() / 1000);
        var cameraInput = document.getElementById('std-camera-storage-id');
        var fromInput = document.getElementById('std-camera-storage-from');
        var toInput = document.getElementById('std-camera-storage-to');
        var deletedInput = document.getElementById('std-camera-include-deleted');
        var selected = (cameraInput && cameraInput.value) || _state.selectedCameraId || ((_state.effectivePolicies[0] || {}).camera_id || '');
        var fromValue = (fromInput && fromInput.value) || _dateInputValue(now - 86400);
        var toValue = (toInput && toInput.value) || _dateInputValue(now);
        var includeDeleted = deletedInput ? deletedInput.checked : false;
        var resultHtml = _state.cameraSummary || _state.cameraTimeline ?
            _renderCameraStorageResult(_state.cameraSummary || {}, _state.cameraTimeline || {}) :
            '<div class="std-empty">Chọn camera và khoảng thời gian để xem summary và timeline lưu trữ.</div>';

        el.innerHTML =
            '<div class="std-camera-toolbar">' +
                '<label>Camera<select id="std-camera-storage-id">' + _cameraOptionsHtml(selected) + '</select></label>' +
                '<label>Từ<input id="std-camera-storage-from" type="datetime-local" value="' + _esc(fromValue) + '"></label>' +
                '<label>Đến<input id="std-camera-storage-to" type="datetime-local" value="' + _esc(toValue) + '"></label>' +
                '<label class="std-check std-camera-check"><input id="std-camera-include-deleted" type="checkbox"' + (includeDeleted ? ' checked' : '') + '><span>Hiển thị dữ liệu đã xóa</span></label>' +
                '<button class="std-btn std-btn-primary" data-std-action="camera-storage-load">Load camera data</button>' +
            '</div>' +
            '<div id="std-camera-storage-result">' + resultHtml + '</div>';
    }

    function _renderCameraStorageResult(summary, timeline) {
        var tiers = summary.tier_summary || [];
        var ranges = timeline.ranges || [];
        var kpis = '<div class="std-mini-kpis">' +
            _miniKpi('Camera', summary.camera_name || timeline.camera_name || summary.camera_id || timeline.camera_id || '-') +
            _miniKpi('Policy', summary.policy_name || timeline.policy_name || summary.policy_id || timeline.policy_id || '-') +
            _miniKpi('Dung lượng', _fmtBytes(summary.total_size_bytes)) +
            _miniKpi('Segments', String(summary.total_segment_count || 0)) +
            _miniKpi('Last tiering', _fmtTs(summary.last_tiering_job_time)) +
        '</div>';
        var tierRows = tiers.length ? tiers.map(function (t) {
            return '<tr>' +
                '<td>' + _tierBadge(t.tier) + '</td>' +
                '<td class="std-num">' + _fmtBytes(t.size_bytes) + '</td>' +
                '<td class="std-num">' + _esc(t.segment_count || 0) + '</td>' +
                '<td>' + _fmtTs(t.from_time) + '</td>' +
                '<td>' + _fmtTs(t.to_time) + '</td>' +
            '</tr>';
        }).join('') : '<tr><td colspan="5"><div class="std-empty">Chưa có dữ liệu theo tier cho camera này</div></td></tr>';
        var rangeRows = ranges.length ? ranges.map(function (r) {
            var flags = [];
            if (r.restore_required) flags.push('Restore');
            if (r.has_event) flags.push('Event');
            if (r.has_motion) flags.push('Motion');
            var actions = r.restore_required ?
                '<button class="std-icon-btn" data-std-action="playback-resolve" data-camera-id="' + _esc(timeline.camera_id || summary.camera_id || '') + '" data-start="' + _esc(r.start) + '" data-end="' + _esc(r.end) + '">Resolve</button>' :
                '';
            return '<tr>' +
                '<td>' + _fmtTs(r.start) + '<div class="std-id">' + _fmtTs(r.end) + '</div></td>' +
                '<td>' + _tierBadge(r.tier) + '</td>' +
                '<td><span class="std-status-chip ' + _statusClass(r.status) + '">' + _esc(r.status || '-') + '</span></td>' +
                '<td class="std-num">' + _fmtBytes(r.size_bytes) + '</td>' +
                '<td class="std-num">' + _esc(r.segment_count || 0) + '</td>' +
                '<td>' + _esc(r.pool_id || '-') + '</td>' +
                '<td>' + _esc(flags.join(', ') || '-') + '</td>' +
                '<td><div class="std-actions">' + actions + '</div></td>' +
            '</tr>';
        }).join('') : '<tr><td colspan="8"><div class="std-empty">Không có timeline trong khoảng thời gian đã chọn</div></td></tr>';
        return kpis +
            '<div class="std-camera-result-grid">' +
                '<div><div class="std-subpanel-title">Tier summary</div>' + _table(['Tier', 'Size', 'Segments', 'Từ', 'Đến'], tierRows) + '</div>' +
                '<div><div class="std-subpanel-title">Camera timeline</div>' + _table(['Time range', 'Tier', 'Status', 'Size', 'Segments', 'Pool', 'Flags', 'Actions'], rangeRows) + '</div>' +
            '</div>';
    }

    function _miniKpi(label, value) {
        return '<div class="std-mini-kpi"><div>' + _esc(label) + '</div><b>' + _esc(value) + '</b></div>';
    }

    function _loadCameraStorage() {
        var cameraEl = document.getElementById('std-camera-storage-id');
        var fromEl = document.getElementById('std-camera-storage-from');
        var toEl = document.getElementById('std-camera-storage-to');
        var deletedEl = document.getElementById('std-camera-include-deleted');
        var cameraId = cameraEl ? cameraEl.value : '';
        var startTime = _dateInputSeconds(fromEl && fromEl.value);
        var endTime = _dateInputSeconds(toEl && toEl.value);
        if (!cameraId) {
            _setStatus('err', 'Vui lòng chọn camera');
            return;
        }
        if (!startTime || !endTime || startTime >= endTime) {
            _setStatus('err', 'Khoảng thời gian camera không hợp lệ');
            return;
        }
        _state.selectedCameraId = cameraId;
        _setStatus('', 'Đang tải dữ liệu camera...');
        Promise.all([
            _apiStrict('/media/api/storage/camera/summary', { camera_id: cameraId }),
            _apiStrict('/media/api/storage/camera/timeline', {
                camera_id: cameraId,
                start_time: startTime,
                end_time: endTime,
                include_deleted: !!(deletedEl && deletedEl.checked)
            })
        ]).then(function (res) {
            _state.cameraSummary = res[0] || {};
            _state.cameraTimeline = res[1] || {};
            _renderCameraStoragePanel();
            _setStatus('ok', 'Đã tải dữ liệu camera ' + cameraId);
        }).catch(function (err) {
            _setStatus('err', (err && err.message) || 'Không tải được dữ liệu camera');
        });
    }

    function _renderJobs(jobs) {
        if (!jobs.length) {
            document.getElementById('std-jobs').innerHTML = '<div class="std-empty">Không có job gần đây</div>';
            return;
        }
        var rows = jobs.map(function (j) {
            var total = _num(j.total_bytes);
            var processed = _num(j.processed_bytes);
            var pct = j.progress_percent != null ? _num(j.progress_percent) : (total > 0 ? processed * 100 / total : 0);
            return '<tr>' +
                '<td><div class="std-name">' + _esc(j.job_id) + '</div><div class="std-id">' + _esc(j.camera_name || j.camera_id) + '</div></td>' +
                '<td>' + _tierBadge(j.source_tier) + ' <span class="std-arrow">→</span> ' + _tierBadge(j.target_tier) + '</td>' +
                '<td><span class="std-status-chip ' + _statusClass(j.status) + '">' + _esc(j.status) + '</span></td>' +
                '<td><div class="std-meter"><span style="width:' + Math.min(100, pct).toFixed(1) + '%"></span></div><div class="std-meter-label">' + _fmtBytes(processed) + ' / ' + _fmtBytes(total) + '</div></td>' +
                '<td>' + _fmtTs(j.created_at) + '</td>' +
                '<td>' + _esc(j.error_message || '') + '</td>' +
            '</tr>';
        }).join('');
        document.getElementById('std-jobs').innerHTML = _table(
            ['Job / Camera', 'Move', 'Status', 'Progress', 'Created', 'Error'], rows);
    }

    function _renderRestoreJobs(jobs) {
        if (!jobs.length) {
            document.getElementById('std-restore-jobs').innerHTML = '<div class="std-empty">Không có restore job</div>';
            return;
        }
        var rows = jobs.map(function (j) {
            var pct = j.progress_percent != null ? _num(j.progress_percent) : 0;
            return '<tr>' +
                '<td><div class="std-name">' + _esc(j.job_id) + '</div><div class="std-id">' + _esc(j.camera_name || j.camera_id) + '</div></td>' +
                '<td>' + _tierBadge(j.source_tier) + ' <span class="std-arrow">→</span> ' + _tierBadge(j.target_tier) + '</td>' +
                '<td><span class="std-status-chip ' + _statusClass(j.status) + '">' + _esc(j.status) + '</span></td>' +
                '<td><div class="std-meter"><span style="width:' + Math.min(100, pct).toFixed(1) + '%"></span></div><div class="std-meter-label">' + pct.toFixed(0) + '% · ' + _fmtBytes(j.total_bytes) + '</div></td>' +
                '<td>' + _esc(j.playback_ready ? 'Ready' : 'Not ready') + '</td>' +
                '<td>' + _fmtTs(j.created_at) + '</td>' +
            '</tr>';
        }).join('');
        document.getElementById('std-restore-jobs').innerHTML = _table(
            ['Job / Camera', 'Restore', 'Status', 'Progress', 'Playback', 'Created'], rows);
    }

    function _renderExpiredSegments(items) {
        if (!items.length) {
            document.getElementById('std-expired-segments').innerHTML = '<div class="std-empty">Không có segment chờ duyệt xóa</div>';
            return;
        }
        var rows = items.map(function (s) {
            var rangeId = s.range_id;
            return '<tr>' +
                '<td><div class="std-name">' + _esc(s.camera_name || s.camera_id) + '</div><div class="std-id">' + _esc(rangeId) + '</div></td>' +
                '<td>' + _tierBadge(s.tier) + '</td>' +
                '<td>' + _fmtTs(s.start_time) + '<div class="std-id">' + _fmtTs(s.end_time) + '</div></td>' +
                '<td class="std-num">' + _fmtBytes(s.size_bytes) + '</td>' +
                '<td class="std-num">' + _esc(s.segment_count || 0) + '</td>' +
                '<td>' + _esc(s.pool_id || '-') + '</td>' +
                '<td>' + _esc(s.protected ? 'Protected' : 'Normal') + '</td>' +
                '<td>' + _fmtTs(s.expired_at) + '</td>' +
                '<td><div class="std-actions">' +
                    '<button class="std-icon-btn" data-std-action="expired-extend" data-id="' + _esc(rangeId) + '">Extend</button>' +
                    '<button class="std-icon-btn std-danger-btn" data-std-action="expired-approve" data-id="' + _esc(rangeId) + '">Approve</button>' +
                '</div></td>' +
            '</tr>';
        }).join('');
        document.getElementById('std-expired-segments').innerHTML = _table(
            ['Range', 'Tier', 'Time range', 'Size', 'Segments', 'Pool', 'Flag', 'Expired', 'Actions'], rows);
    }

    function _renderProtectedVideos(items) {
        if (!items.length) {
            document.getElementById('std-protected-videos').innerHTML = '<div class="std-empty">Không có đoạn video đang bảo vệ</div>';
            return;
        }
        var rows = items.map(function (p) {
            return '<tr>' +
                '<td><div class="std-name">' + _esc(p.camera_name || p.camera_id) + '</div><div class="std-id">' + _esc(p.protected_id) + '</div></td>' +
                '<td><span class="std-status-chip std-ok">' + _esc(p.type || 'PROTECTED') + '</span></td>' +
                '<td>' + _fmtTs(p.start_time) + '<div class="std-id">' + _fmtTs(p.end_time) + '</div></td>' +
                '<td>' + _esc(p.reason || '') + '</td>' +
                '<td>' + _fmtTs(p.created_at) + '</td>' +
            '</tr>';
        }).join('');
        document.getElementById('std-protected-videos').innerHTML = _table(
            ['Camera / Protected ID', 'Type', 'Time range', 'Reason', 'Created'], rows);
    }

    function _showModal(title, html, onSubmit) {
        var root = document.getElementById('std-modal-root');
        if (!root) return;
        root.innerHTML = '<div class="std-modal-backdrop">' +
            '<div class="std-modal">' +
                '<div class="std-modal-head"><div class="std-modal-title">' + _esc(title) + '</div><button class="std-close" type="button" data-std-action="modal-close">×</button></div>' +
                html +
            '</div>' +
        '</div>';
        var form = root.querySelector('form');
        if (form) {
            form.addEventListener('submit', function (ev) {
                ev.preventDefault();
                if (_state.saving) return;
                onSubmit(form);
            });
        }
    }

    function _closeModal() {
        var root = document.getElementById('std-modal-root');
        if (root) root.innerHTML = '';
    }

    function _bool(v) {
        return v === true || v === 1 || v === '1' || v === 'true';
    }

    function _fieldValue(form, name) {
        return (form.elements[name] && form.elements[name].value || '').trim();
    }

    function _poolId(pool) {
        return String((pool && pool.id) || '');
    }

    function _poolTier(pool) {
        return String((pool && pool.tier) || '').toUpperCase();
    }

    function _policyPoolList(tier) {
        var target = String(tier || '').toUpperCase();
        return ((_state.data && _state.data.pools) || []).filter(function (p) {
            var id = _poolId(p);
            return id && (!target || _poolTier(p) === target);
        });
    }

    function _poolOptionsHtml(tier, selected) {
        selected = String(selected || '');
        var pools = _policyPoolList(tier);
        var html = '<option value="">Select pool</option>';
        var hasSelected = !selected;
        pools.forEach(function (p) {
            var id = _poolId(p);
            var selectedAttr = id === selected ? ' selected' : '';
            var state = p.enabled === false || p.enabled === 0 ? 'disabled' : 'enabled';
            if (selectedAttr) hasSelected = true;
            html += '<option value="' + _esc(id) + '"' + selectedAttr + '>' +
                _esc(p.name || id) + ' · ' + _esc(_poolTier(p) || '-') + ' · ' + _esc(p.type || '-') + ' · ' + state + '</option>';
        });
        if (!hasSelected) {
            html += '<option value="' + _esc(selected) + '" selected>' + _esc(selected) + ' · pool hiện tại không còn trong tier ' + _esc(tier || '-') + '</option>';
        }
        return html;
    }

    function _allowedPoolTypes(tier) {
        var support = (_state.poolOptions && _state.poolOptions[tier]) || null;
        if (!support) return ['LOCAL_DISK', 'NAS', 'MINIO', 'S3'];
        return Object.keys(support).filter(function (type) { return support[type]; });
    }

    function _poolTypeOptionsHtml(tier, selected) {
        var types = _allowedPoolTypes(tier);
        if (types.indexOf(selected) < 0) selected = types[0] || '';
        return types.map(function (type) {
            return '<option value="' + _esc(type) + '"' + (type === selected ? ' selected' : '') + '>' + _esc(type) + '</option>';
        }).join('');
    }

    function _mountOptionsHtml(type, selected) {
        var list = type === 'NAS' ? _state.nasMountPoints : _state.mountPoints;
        var html = '<option value="">--Chọn mount path--</option>';
        (list || []).forEach(function (mp) {
            var mount = mp.mount || '';
            var label = mount + (mp.name ? ' · ' + mp.name : '') + (mp.total ? ' · ' + _fmtBytes(mp.total) : '');
            html += '<option value="' + _esc(mount) + '"' + (mount === selected ? ' selected' : '') + '>' + _esc(label) + '</option>';
        });
        if (selected && html.indexOf('value="' + _esc(selected) + '"') < 0) {
            html += '<option value="' + _esc(selected) + '" selected>' + _esc(selected) + '</option>';
        }
        return html;
    }

    function _policyOptionsHtml(selected) {
        var html = '<option value="">Select policy</option>';
        (((_state.data && _state.data.policies) || [])).forEach(function (p) {
            html += '<option value="' + _esc(p.id) + '"' + (p.id === selected ? ' selected' : '') + '>' + _esc(p.name || p.id) + '</option>';
        });
        return html;
    }

    function _cameraDatalistHtml() {
        var html = '<datalist id="std-camera-list">';
        (_state.effectivePolicies || []).forEach(function (c) {
            var name = c.camera_name || c.camera_id || '';
            html += '<option value="' + _esc(c.camera_id || '') + '">' + _esc(name) + '</option>';
        });
        html += '</datalist>';
        return html;
    }

    function _cameraOptionsHtml(selected) {
        var html = '<option value="">Select camera</option>';
        (_state.effectivePolicies || []).forEach(function (c) {
            var label = (c.camera_name || c.camera_id || '-') + (c.policy_name ? ' · ' + c.policy_name : '');
            html += '<option value="' + _esc(c.camera_id || '') + '"' + (c.camera_id === selected ? ' selected' : '') + '>' + _esc(label) + '</option>';
        });
        return html;
    }

    function _openPoolModal(pool) {
        pool = pool || {};
        var editing = !!pool.id;
        var selectedTier = pool.tier || 'HOT';
        var selectedType = pool.type || (_allowedPoolTypes(selectedTier)[0] || 'LOCAL_DISK');
        var html = '<form class="std-form" id="std-pool-form" data-tested="false">' +
            '<div class="std-form-grid">' +
                _input('name', 'Name', pool.name || '', 'text', true) +
                _select('tier', 'Tier', ['HOT', 'WARM', 'COLD'], pool.tier || 'HOT') +
                '<label>Type<select name="type">' + _poolTypeOptionsHtml(selectedTier, selectedType) + '</select></label>' +
                '<label data-pool-field="mount">Mount path<select name="mount_path">' + _mountOptionsHtml(selectedType, pool.mount_path || '') + '</select></label>' +
                '<label data-pool-field="network">Network path<input name="network_path" type="text" value="' + _esc(pool.network_path || '') + '" placeholder="nfs://server/path hoặc //server/share"></label>' +
                '<label data-pool-field="endpoint">Endpoint<input name="endpoint" type="text" value="' + _esc(pool.endpoint || '') + '" placeholder="http://127.0.0.1:9000"></label>' +
                '<label data-pool-field="bucket">Bucket<input name="bucket" type="text" value="' + _esc(pool.bucket || '') + '"></label>' +
                '<label data-pool-field="base_path">Base path<input name="base_path" type="text" value="' + _esc(pool.base_path || '') + '"></label>' +
                '<label data-pool-field="access_key">Access key<input name="access_key" type="text" value="' + _esc(pool.access_key || '') + '"></label>' +
                '<label data-pool-field="secret_key">Secret key<input name="secret_key" type="password" value=""></label>' +
                _input('high_watermark_percent', 'High watermark %', pool.high_watermark_percent || 85, 'number', true) +
                _input('critical_watermark_percent', 'Critical watermark %', pool.critical_watermark_percent || 90, 'number', true) +
            '</div>' +
            '<div class="std-check-row">' +
                _checkbox('enabled', 'Enabled', pool.enabled !== false) +
                _checkbox('health_check_enabled', 'Health check', pool.health_check_enabled !== false) +
            '</div>' +
            '<div class="std-test-row"><button class="std-btn" type="button" id="std-test-pool-btn">Test connection</button><span id="std-pool-test-result" class="std-test-result">Chưa kiểm tra</span></div>' +
            '<div class="std-form-actions"><button class="std-btn" type="button" data-std-action="modal-close">Cancel</button><button class="std-btn std-btn-primary" type="submit">' + (editing ? 'Update pool' : 'Create pool') + '</button></div>' +
        '</form>';
        _showModal(editing ? 'Update pool' : 'Create pool', html, function (form) {
            var payload = _poolPayloadFromForm(form, pool);
            if (_poolNeedsTest(payload.type) && form.getAttribute('data-tested') !== 'true') {
                _setPoolTestResult('err', 'Cần test kết nối thành công trước khi lưu');
                return;
            }
            _save(editing ? '/media/api/storage/pool/update' : '/media/api/storage/pool/create', payload, 'Pool đã được lưu');
        });
        _bindPoolForm(pool);
    }

    function _poolNeedsTest(type) {
        return type === 'NAS' || type === 'MINIO' || type === 'S3';
    }

    function _poolPayloadFromForm(form, existing) {
        var payload = {
            name: _fieldValue(form, 'name'),
            type: _fieldValue(form, 'type'),
            tier: _fieldValue(form, 'tier'),
            mount_path: _fieldValue(form, 'mount_path'),
            network_path: _fieldValue(form, 'network_path'),
            endpoint: _fieldValue(form, 'endpoint'),
            bucket: _fieldValue(form, 'bucket'),
            base_path: _fieldValue(form, 'base_path'),
            access_key: _fieldValue(form, 'access_key'),
            enabled: !!form.elements.enabled.checked,
            health_check_enabled: !!form.elements.health_check_enabled.checked,
            high_watermark_percent: parseInt(_fieldValue(form, 'high_watermark_percent'), 10) || 85,
            critical_watermark_percent: parseInt(_fieldValue(form, 'critical_watermark_percent'), 10) || 90
        };
        var secret = _fieldValue(form, 'secret_key');
        if (secret) payload.secret_key = secret;
        if (existing && existing.id) payload.id = existing.id;
        return payload;
    }

    function _bindPoolForm(pool) {
        var form = document.getElementById('std-pool-form');
        if (!form) return;
        var tierEl = form.elements.tier;
        var typeEl = form.elements.type;
        var mountEl = form.elements.mount_path;
        var testBtn = document.getElementById('std-test-pool-btn');

        function resetTest() {
            form.setAttribute('data-tested', 'false');
            _setPoolTestResult('', _poolNeedsTest(typeEl.value) ? 'Cần test trước khi lưu' : 'Không cần test cho LOCAL_DISK');
        }

        function syncTypeOptions() {
            var current = typeEl.value;
            typeEl.innerHTML = _poolTypeOptionsHtml(tierEl.value, current);
            syncFields();
        }

        function syncFields() {
            var type = typeEl.value;
            mountEl.innerHTML = _mountOptionsHtml(type, mountEl.value || (pool && pool.mount_path) || '');
            var visible = {
                mount: type === 'LOCAL_DISK' || type === 'NAS',
                network: type === 'NAS',
                endpoint: type === 'MINIO' || type === 'S3',
                bucket: type === 'MINIO' || type === 'S3',
                base_path: type === 'MINIO' || type === 'S3',
                access_key: type === 'MINIO' || type === 'S3',
                secret_key: type === 'MINIO' || type === 'S3'
            };
            Array.prototype.forEach.call(form.querySelectorAll('[data-pool-field]'), function (node) {
                var field = node.getAttribute('data-pool-field');
                node.style.display = visible[field] ? '' : 'none';
                var input = node.querySelector('input,select');
                if (input) {
                    input.required = !!visible[field] && (
                        field === 'mount' ||
                        field === 'network' ||
                        field === 'endpoint' ||
                        field === 'bucket' ||
                        field === 'base_path' ||
                        field === 'access_key' ||
                        (field === 'secret_key' && !(pool && pool.id))
                    );
                }
            });
            testBtn.style.display = _poolNeedsTest(type) ? '' : 'none';
            resetTest();
        }

        tierEl.addEventListener('change', syncTypeOptions);
        typeEl.addEventListener('change', syncFields);
        Array.prototype.forEach.call(form.querySelectorAll('input,select'), function (el) {
            el.addEventListener('input', function () {
                if (el.name !== 'tier' && el.name !== 'type') resetTest();
            });
        });
        testBtn.addEventListener('click', function () {
            var payload = _poolPayloadFromForm(form, pool || {});
            _setPoolTestResult('', 'Đang test kết nối...');
            _apiStrict('/media/api/storage/pool/testConnection', payload).then(function (data) {
                form.setAttribute('data-tested', 'true');
                _setPoolTestResult('ok', 'OK · latency ' + (data.latency_ms || 0) + ' ms' + (data.message ? ' · ' + data.message : ''));
            }).catch(function (err) {
                form.setAttribute('data-tested', 'false');
                _setPoolTestResult('err', (err && err.message) || 'Test kết nối thất bại');
            });
        });
        syncTypeOptions();
    }

    function _setPoolTestResult(kind, msg) {
        var el = document.getElementById('std-pool-test-result');
        if (!el) return;
        el.className = 'std-test-result ' + (kind ? 'std-test-' + kind : '');
        el.textContent = msg || '';
    }

    function _tierConfig(policy, tier) {
        var list = policy && policy.tiers || [];
        for (var i = 0; i < list.length; i++) if (list[i].tier === tier) return list[i];
        return { tier: tier, enabled: tier === 'HOT', pool_id: '', retain_until_days: tier === 'HOT' ? 7 : (tier === 'WARM' ? 14 : 30), high_watermark_percent: 80, critical_watermark_percent: 90, data_mode: 'FULL_VIDEO', overflow_action: 'MOVE_TO_NEXT_TIER', priority: 'NORMAL' };
    }

    function _tierFormHtml(policy, tier) {
        var t = _tierConfig(policy, tier);
        return '<div class="std-tier-form" data-policy-tier="' + _esc(tier) + '">' +
            '<div class="std-tier-form-head">' + _tierBadge(tier) + _checkbox('tier_' + tier + '_enabled', 'Enabled', t.enabled !== false) + '</div>' +
            '<label>Pool<select name="tier_' + tier + '_pool">' + _poolOptionsHtml(tier, t.pool_id || '') + '</select></label>' +
            '<label>Lưu đến ngày thứ<input name="tier_' + tier + '_days" type="number" min="0" value="' + _esc(t.retain_until_days || 0) + '"></label>' +
            '<label>Chế độ dữ liệu<select name="tier_' + tier + '_data_mode">' + _optionList([
                ['FULL_VIDEO', 'Video đầy đủ'],
                ['EVENT_VIDEO_ONLY', 'Chỉ video sự kiện'],
                ['SNAPSHOT_ONLY', 'Chỉ snapshot'],
                ['METADATA_ONLY', 'Chỉ metadata'],
                ['MOTION_INDEX_ONLY', 'Chỉ motion index']
            ], t.data_mode || 'FULL_VIDEO') + '</select></label>' +
            '<label>Hành động khi đầy<select name="tier_' + tier + '_overflow">' + _optionList([
                ['MOVE_TO_NEXT_TIER', 'Chuyển sang tầng tiếp theo'],
                ['DELETE_OLDEST', 'Xóa dữ liệu cũ nhất'],
                ['STOP_RECORDING_AND_ALERT', 'Dừng ghi và cảnh báo']
            ], t.overflow_action || 'MOVE_TO_NEXT_TIER') + '</select></label>' +
            '<label>High watermark %<input name="tier_' + tier + '_high" type="number" min="0" max="100" value="' + _esc(t.high_watermark_percent || 80) + '"></label>' +
            '<label>Critical watermark %<input name="tier_' + tier + '_critical" type="number" min="0" max="100" value="' + _esc(t.critical_watermark_percent || 90) + '"></label>' +
        '</div>';
    }

    function _bindPolicyForm() {
        var form = document.querySelector('.std-policy-form');
        if (!form) return;
        var hotOverflow = form.elements.tier_HOT_overflow;
        var warmOverflow = form.elements.tier_WARM_overflow;

        function setTierConfigEnabled(tier, enabled) {
            var card = form.querySelector('[data-policy-tier="' + tier + '"]');
            if (!card) return;
            card.classList.toggle('std-tier-form-disabled', !enabled);
            Array.prototype.forEach.call(card.querySelectorAll('input,select'), function (el) {
                if (tier === 'HOT') return;
                el.disabled = !enabled;
            });
        }

        function syncTierAvailability() {
            var warmEnabled = hotOverflow && hotOverflow.value === 'MOVE_TO_NEXT_TIER';
            var warmChecked = form.elements.tier_WARM_enabled && form.elements.tier_WARM_enabled.checked;
            var coldEnabled = warmEnabled && (!warmChecked || (warmOverflow && warmOverflow.value === 'MOVE_TO_NEXT_TIER'));
            setTierConfigEnabled('HOT', true);
            setTierConfigEnabled('WARM', warmEnabled);
            setTierConfigEnabled('COLD', coldEnabled);
        }

        if (hotOverflow) hotOverflow.addEventListener('change', syncTierAvailability);
        if (warmOverflow) warmOverflow.addEventListener('change', syncTierAvailability);
        if (form.elements.tier_WARM_enabled) form.elements.tier_WARM_enabled.addEventListener('change', syncTierAvailability);
        syncTierAvailability();
    }

    function _policyTierAvailable(form, tier) {
        if (tier === 'HOT') return true;
        var hotMoves = form.elements.tier_HOT_overflow && form.elements.tier_HOT_overflow.value === 'MOVE_TO_NEXT_TIER';
        if (tier === 'WARM') return hotMoves;
        var warmEnabled = form.elements.tier_WARM_enabled && form.elements.tier_WARM_enabled.checked;
        var warmMoves = form.elements.tier_WARM_overflow && form.elements.tier_WARM_overflow.value === 'MOVE_TO_NEXT_TIER';
        return hotMoves && (!warmEnabled || warmMoves);
    }

    function _openPolicyModal(policy) {
        policy = policy || {};
        var editing = !!policy.id;
        var deletePolicy = policy.delete_policy || {};
        var advancedRules = policy.advanced_rules || {};
        var html = '<form class="std-form std-policy-form">' +
            '<div class="std-section-title">Thông tin chung</div>' +
            '<div class="std-form-grid">' +
                _input('name', 'Tên chính sách', policy.name || '', 'text', true) +
                _input('total_retention_days', 'Tổng thời gian lưu trữ (ngày)', policy.total_retention_days || 30, 'number', true) +
                _input('description', 'Mô tả', policy.description || '', 'text', false) +
            '</div>' +
            '<div class="std-check-row">' +
                _checkbox('enabled', 'Kích hoạt', policy.enabled !== false) +
                _checkbox('allow_camera_override', 'Cho phép camera override', policy.allow_camera_override !== false) +
                _checkbox('protect_event_video', 'Bảo vệ video sự kiện', _bool(policy.protect_event_video)) +
            '</div>' +
            '<div class="std-section-title">Cấu hình tầng HOT / WARM / COLD</div>' +
            '<div class="std-tier-grid">' + _tierFormHtml(policy, 'HOT') + _tierFormHtml(policy, 'WARM') + _tierFormHtml(policy, 'COLD') + '</div>' +
            '<div class="std-section-title">Chính sách xóa dữ liệu</div>' +
            '<div class="std-form-grid">' +
                _input('delete_after_days', 'Xóa sau (ngày)', deletePolicy.delete_after_days || policy.total_retention_days || 30, 'number', true) +
                '<label>Chế độ xóa<select name="delete_mode">' + _optionList([
                    ['DELETE_AUTOMATICALLY', 'Xóa tự động'],
                    ['MARK_EXPIRED_WAIT_APPROVAL', 'Đánh dấu hết hạn, chờ duyệt'],
                    ['MOVE_TO_EXTERNAL_STORAGE', 'Chuyển ra lưu trữ ngoài']
                ], deletePolicy.delete_mode || 'DELETE_AUTOMATICALLY') + '</select></label>' +
            '</div>' +
            '<div class="std-check-col">' +
                _checkbox('skip_protected_video', 'Bỏ qua video được bảo vệ', deletePolicy.skip_protected_video !== false) +
                _checkbox('skip_evidence_video', 'Bỏ qua video bằng chứng', deletePolicy.skip_evidence_video !== false) +
                _checkbox('require_approval_before_delete', 'Yêu cầu phê duyệt trước khi xóa', _bool(deletePolicy.require_approval_before_delete)) +
            '</div>' +
            '<div class="std-section-title">Quy tắc nâng cao</div>' +
            '<div class="std-check-col">' +
                _checkbox('enable_early_move_when_pool_high', 'Chuyển sớm khi vùng lưu trữ đầy', advancedRules.enable_early_move_when_pool_high !== false) +
                _checkbox('prefer_move_no_event_video_first', 'Ưu tiên chuyển video không có sự kiện', advancedRules.prefer_move_no_event_video_first !== false) +
                _checkbox('prefer_keep_event_video_longer', 'Ưu tiên giữ lại video có sự kiện lâu hơn', advancedRules.prefer_keep_event_video_longer !== false) +
                _checkbox('skip_move_if_pool_offline', 'Bỏ qua chuyển tầng nếu pool offline', advancedRules.skip_move_if_pool_offline !== false) +
                _checkbox('alert_when_pool_critical', 'Cảnh báo khi pool critical', advancedRules.alert_when_pool_critical !== false) +
            '</div>' +
            '<div class="std-form-grid std-form-grid-one">' +
                _input('min_segment_age_minutes_before_move', 'Thời gian tối thiểu trước khi chuyển (phút)', advancedRules.min_segment_age_minutes_before_move || 30, 'number', true) +
            '</div>' +
            '<div class="std-form-actions"><button class="std-btn" type="button" data-std-action="modal-close">Cancel</button><button class="std-btn std-btn-primary" type="submit">' + (editing ? 'Update policy' : 'Create policy') + '</button></div>' +
        '</form>';
        _showModal(editing ? 'Update policy' : 'Create policy', html, function (form) {
            var payload = {
                name: _fieldValue(form, 'name'),
                description: _fieldValue(form, 'description'),
                enabled: !!form.elements.enabled.checked,
                total_retention_days: parseInt(_fieldValue(form, 'total_retention_days'), 10) || 30,
                allow_camera_override: !!form.elements.allow_camera_override.checked,
                protect_event_video: !!form.elements.protect_event_video.checked,
                tiers: ['HOT', 'WARM', 'COLD'].map(function (tier) {
                    return {
                        tier: tier,
                        enabled: _policyTierAvailable(form, tier) && !!form.elements['tier_' + tier + '_enabled'].checked,
                        pool_id: _fieldValue(form, 'tier_' + tier + '_pool'),
                        retain_until_days: parseInt(_fieldValue(form, 'tier_' + tier + '_days'), 10) || 0,
                        data_mode: _fieldValue(form, 'tier_' + tier + '_data_mode'),
                        overflow_action: _fieldValue(form, 'tier_' + tier + '_overflow'),
                        high_watermark_percent: parseInt(_fieldValue(form, 'tier_' + tier + '_high'), 10) || 80,
                        critical_watermark_percent: parseInt(_fieldValue(form, 'tier_' + tier + '_critical'), 10) || 90,
                        priority: 'NORMAL'
                    };
                }),
                delete_policy: {
                    delete_mode: _fieldValue(form, 'delete_mode'),
                    delete_after_days: parseInt(_fieldValue(form, 'delete_after_days'), 10) || 30,
                    skip_protected_video: !!form.elements.skip_protected_video.checked,
                    skip_evidence_video: !!form.elements.skip_evidence_video.checked,
                    require_approval_before_delete: !!form.elements.require_approval_before_delete.checked
                },
                advanced_rules: {
                    enable_early_move_when_pool_high: !!form.elements.enable_early_move_when_pool_high.checked,
                    prefer_move_no_event_video_first: !!form.elements.prefer_move_no_event_video_first.checked,
                    prefer_keep_event_video_longer: !!form.elements.prefer_keep_event_video_longer.checked,
                    skip_move_if_pool_offline: !!form.elements.skip_move_if_pool_offline.checked,
                    alert_when_pool_critical: !!form.elements.alert_when_pool_critical.checked,
                    min_segment_age_minutes_before_move: parseInt(_fieldValue(form, 'min_segment_age_minutes_before_move'), 10) || 30
                }
            };
            if (editing) payload.id = policy.id;
            _save(editing ? '/media/api/storage/policy/update' : '/media/api/storage/policy/create', payload, 'Policy đã được lưu');
        });
        _bindPolicyForm();
    }

    function _refreshAssignModalData() {
        _setStatus('', 'Đang cập nhật danh sách policy và camera...');
        return Promise.all([
            _apiStrict('/media/api/storage/policy/list', { page: 0, size: 100 }),
            _apiStrict('/media/api/storage/camera/effectivePolicy/list', {})
        ]).then(function (res) {
            if (!_state.data) _state.data = {};
            _state.data.policies = (res[0] && res[0].items) || [];
            _state.effectivePolicies = (res[1] && res[1].items) || [];
        });
    }

    function _openAssignModal() {
        _refreshAssignModalData().then(function () {
            _setStatus('', '');
            _showAssignModal();
        }).catch(function (err) {
            _setStatus('err', (err && err.message) || 'Không cập nhật được danh sách policy/camera');
        });
    }

    function _showAssignModal() {
        var html = '<form class="std-form">' +
            _cameraDatalistHtml() +
            '<div class="std-form-grid">' +
                '<label>Camera<select name="camera_id" required>' + _cameraOptionsHtml('') + '</select></label>' +
                '<label>Policy<select name="policy_id" required>' + _policyOptionsHtml('') + '</select></label>' +
                _input('override_reason', 'Reason', '', 'text', false) +
            '</div>' +
            '<div class="std-form-actions"><button class="std-btn" type="button" data-std-action="modal-close">Cancel</button><button class="std-btn std-btn-primary" type="submit">Assign camera</button></div>' +
        '</form>';
        _showModal('Assign camera to policy', html, function (form) {
            _save('/media/api/storage/policy/assignCamera', {
                camera_id: _fieldValue(form, 'camera_id'),
                policy_id: _fieldValue(form, 'policy_id'),
                override_reason: _fieldValue(form, 'override_reason')
            }, 'Camera đã được gán policy');
        });
    }

    function _input(name, label, value, type, required) {
        return '<label>' + _esc(label) + '<input name="' + _esc(name) + '" type="' + _esc(type || 'text') + '"' + (required ? ' required' : '') + ' value="' + _esc(value == null ? '' : value) + '"></label>';
    }

    function _select(name, label, options, selected) {
        return '<label>' + _esc(label) + '<select name="' + _esc(name) + '">' + options.map(function (opt) {
            return '<option value="' + _esc(opt) + '"' + (opt === selected ? ' selected' : '') + '>' + _esc(opt) + '</option>';
        }).join('') + '</select></label>';
    }

    function _optionList(options, selected) {
        return options.map(function (opt) {
            return '<option value="' + _esc(opt[0]) + '"' + (opt[0] === selected ? ' selected' : '') + '>' + _esc(opt[1]) + '</option>';
        }).join('');
    }

    function _checkbox(name, label, checked) {
        return '<label class="std-check"><input name="' + _esc(name) + '" type="checkbox"' + (checked ? ' checked' : '') + '><span>' + _esc(label) + '</span></label>';
    }

    function _save(path, payload, okMsg) {
        _state.saving = true;
        _setStatus('', 'Đang lưu...');
        _apiStrict(path, payload).then(function () {
            _closeModal();
            _setStatus('ok', okMsg || 'Đã lưu');
            _state.loading = false;
            return _load();
        }).catch(function (err) {
            _setStatus('err', (err && err.message) || 'Không lưu được thay đổi');
        }).finally(function () {
            _state.saving = false;
        });
    }

    function _deletePool(id) {
        if (!id || !window.confirm('Delete storage pool ' + id + '?')) return;
        _save('/media/api/storage/pool/delete', { id: id }, 'Pool đã được xóa');
    }

    function _deletePolicy(id) {
        if (!id || !window.confirm('Delete storage policy ' + id + '?')) return;
        _save('/media/api/storage/policy/delete', { id: id }, 'Policy đã được xóa');
    }

    function _removeCamera(cameraId) {
        if (!cameraId || !window.confirm('Remove policy assignment from camera ' + cameraId + '?')) return;
        _save('/media/api/storage/policy/removeCamera', { camera_id: cameraId }, 'Camera đã được gỡ khỏi policy');
    }

    function _resolvePlayback(btn) {
        var cameraId = btn.getAttribute('data-camera-id') || _state.selectedCameraId || '';
        var start = parseInt(btn.getAttribute('data-start'), 10) || 0;
        var end = parseInt(btn.getAttribute('data-end'), 10) || 0;
        if (!cameraId || !start || !end || start >= end) {
            _setStatus('err', 'Thiếu thông tin range để resolve playback');
            return;
        }
        _setStatus('', 'Đang kiểm tra restore/playback...');
        _apiStrict('/media/mserver/storage/playback/resolve', {
            camera_id: cameraId,
            start_time: start,
            end_time: end
        }).then(function (data) {
            var status = data.restore_job_status || data.status || '';
            var url = data.playback_url || '';
            _setStatus('ok', url ? ('Playback READY: ' + url) : ('Playback status: ' + (status || 'RESTORE_REQUIRED')));
            if (url && window.navigator && window.navigator.clipboard) {
                window.navigator.clipboard.writeText(url).catch(function () {});
            }
        }).catch(function (err) {
            _setStatus('err', (err && err.message) || 'Không resolve được playback');
        });
    }

    function _approveExpiredRange(rangeId) {
        if (!rangeId || !window.confirm('Approve delete expired range ' + rangeId + '?')) return;
        _save('/media/mserver/storage/expiredSegment/approve', { range_ids: [rangeId] }, 'Range đã được approve xóa');
    }

    function _extendExpiredRange(rangeId) {
        if (!rangeId) return;
        var defaultValue = _dateInputValue(Math.floor(Date.now() / 1000) + 7 * 86400);
        var input = window.prompt('Extend range đến thời điểm nào? (YYYY-MM-DDTHH:mm)', defaultValue);
        if (!input) return;
        var extendUntil = _dateInputSeconds(input);
        if (!extendUntil) {
            _setStatus('err', 'Thời điểm extend không hợp lệ');
            return;
        }
        _save('/media/mserver/storage/expiredSegment/extend', {
            range_ids: [rangeId],
            extend_until: extendUntil
        }, 'Range đã được extend');
    }

    function _table(headers, rows) {
        return '<div class="std-table-wrap"><table class="std-table"><thead><tr>' +
            headers.map(function (h) { return '<th>' + _esc(h) + '</th>'; }).join('') +
            '</tr></thead><tbody>' + rows + '</tbody></table></div>';
    }

    function _injectCss() {
        if (document.getElementById('std-style')) return;
        var style = document.createElement('style');
        style.id = 'std-style';
        style.textContent = [
            '#storage-tier-content{padding-bottom:32px}',
            '.std-head{display:flex;align-items:center;justify-content:space-between;gap:14px;margin-bottom:14px;flex-wrap:wrap}',
            '.std-title{font-size:1.05rem;font-weight:700;color:var(--c-text)}',
            '.std-subtitle{font-size:.78rem;color:var(--c-muted);margin-top:2px}',
            '.std-refresh{display:inline-flex;align-items:center;gap:7px;height:32px;padding:0 13px;border-radius:6px;border:1px solid var(--c-border);background:var(--c-elev);color:var(--c-text);font-size:.8rem;font-weight:700;cursor:pointer}',
            '.std-refresh:hover{border-color:var(--c-accent);background:var(--c-surf)}.std-refresh:disabled{opacity:.55;cursor:default}.std-refresh svg{width:15px;height:15px}',
            '.std-status{margin-bottom:12px;font-size:.78rem;color:var(--c-muted)}.std-status-ok{color:var(--c-success)}.std-status-err{color:var(--c-danger)}',
            '.std-kpis{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px;margin-bottom:12px}',
            '.std-kpi,.std-panel{background:var(--c-surf);border:1px solid var(--c-border);border-radius:8px}',
            '.std-kpi{padding:13px 14px 11px}.std-kpi-label{font-size:.68rem;color:var(--c-muted);font-weight:700;text-transform:uppercase;letter-spacing:.06em}.std-kpi-value{font-size:1.35rem;font-weight:800;font-family:monospace;line-height:1.2;margin-top:4px}.std-kpi-sub{font-size:.72rem;color:var(--c-muted)}',
            '.std-blue{color:#388bfd}.std-green{color:#3fb950}.std-amber{color:#d29922}.std-purple{color:#a371f7}.std-pink{color:#f778ba}.std-ok{color:var(--c-success)}.std-warn{color:var(--c-warn)}.std-danger{color:var(--c-danger)}.std-muted{color:var(--c-muted)}',
            '.std-main-grid{display:grid;grid-template-columns:minmax(280px,1.2fr) minmax(260px,.8fr);gap:12px;margin-bottom:12px}.std-split{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:12px}.std-panel{padding:12px;margin-bottom:12px;min-width:0}.std-panel-title{font-size:.72rem;color:var(--c-muted);font-weight:800;text-transform:uppercase;letter-spacing:.06em;margin-bottom:10px}.std-chart{height:230px;width:100%}',
            '.std-panel-head{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:10px}.std-panel-head .std-panel-title{margin-bottom:0}.std-btn,.std-icon-btn{border:1px solid var(--c-border);background:var(--c-elev);color:var(--c-text);border-radius:6px;font-size:.76rem;font-weight:800;cursor:pointer}.std-btn{height:32px;padding:0 12px}.std-btn:hover,.std-icon-btn:hover{border-color:var(--c-accent);background:var(--c-surf)}.std-btn-primary{border-color:rgba(56,139,253,.55);color:#79c0ff}.std-icon-btn{height:26px;padding:0 8px;white-space:nowrap}.std-danger-btn{color:var(--c-danger);border-color:rgba(248,81,73,.45)}.std-actions{display:flex;align-items:center;gap:6px;flex-wrap:wrap}',
            '.std-job-counts{display:grid;grid-template-columns:repeat(2,minmax(110px,1fr));gap:8px}.std-job-pill{display:flex;align-items:center;justify-content:space-between;border:1px solid currentColor;border-radius:6px;padding:9px 10px;background:rgba(255,255,255,.02)}.std-job-pill span{font-size:.72rem;font-weight:800}.std-job-pill b{font-size:1.1rem;font-family:monospace}',
            '.std-table-wrap{overflow-x:auto;border:1px solid var(--c-border);border-radius:7px}.std-table{width:100%;border-collapse:collapse;font-size:.8rem}.std-table th{padding:8px 10px;text-align:left;font-size:.68rem;color:var(--c-muted);font-weight:800;text-transform:uppercase;letter-spacing:.06em;background:var(--c-elev);border-bottom:1px solid var(--c-border);white-space:nowrap}.std-table td{padding:8px 10px;border-bottom:1px solid var(--c-border);vertical-align:top}.std-table tbody tr:last-child td{border-bottom:none}.std-table tbody tr:hover td{background:rgba(255,255,255,.025)}',
            '.std-name{font-weight:700;color:var(--c-text);white-space:nowrap}.std-id{font-size:.68rem;color:var(--c-muted);font-family:monospace;word-break:break-all;margin-top:2px}.std-num{text-align:right;font-family:monospace;font-weight:700}',
            '.std-tier{display:inline-flex;align-items:center;height:20px;padding:0 7px;border-radius:10px;font-size:.68rem;font-weight:800;border:1px solid currentColor}.std-tier-hot{color:#f778ba}.std-tier-warm{color:#d29922}.std-tier-cold{color:#79c0ff}.std-arrow{color:var(--c-muted);margin:0 4px}',
            '.std-status-chip{display:inline-flex;align-items:center;height:21px;padding:0 8px;border-radius:10px;font-size:.68rem;font-weight:800;border:1px solid currentColor}.std-meter{height:8px;background:var(--c-elev);border-radius:4px;overflow:hidden;min-width:120px}.std-meter span{display:block;height:100%;background:var(--c-accent);border-radius:4px}.std-meter-label{font-size:.68rem;color:var(--c-muted);font-family:monospace;margin-top:4px;white-space:nowrap}.std-empty{padding:18px;color:var(--c-muted);font-size:.8rem;border:1px dashed var(--c-border);border-radius:7px;text-align:center}',
            '.std-camera-toolbar{display:grid;grid-template-columns:minmax(220px,1.2fr) minmax(170px,.8fr) minmax(170px,.8fr) auto auto;gap:10px;align-items:end;margin-bottom:12px}.std-camera-toolbar label{display:flex;flex-direction:column;gap:5px;color:var(--c-muted);font-size:.72rem;font-weight:800;text-transform:uppercase;letter-spacing:.04em}.std-camera-toolbar input,.std-camera-toolbar select{height:32px;border-radius:6px;border:1px solid var(--c-border);background:var(--c-elev);color:var(--c-text);padding:0 9px;font-size:.8rem;font-weight:600;text-transform:none;letter-spacing:0}.std-camera-check{height:32px;justify-content:center;margin:0}.std-mini-kpis{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:8px;margin-bottom:12px}.std-mini-kpi{border:1px solid var(--c-border);border-radius:7px;background:rgba(255,255,255,.02);padding:9px 10px;min-width:0}.std-mini-kpi div{font-size:.66rem;color:var(--c-muted);font-weight:800;text-transform:uppercase;letter-spacing:.06em}.std-mini-kpi b{display:block;margin-top:3px;color:var(--c-text);font-size:.86rem;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.std-camera-result-grid{display:grid;grid-template-columns:minmax(280px,.75fr) minmax(360px,1.25fr);gap:12px}.std-subpanel-title{font-size:.68rem;color:var(--c-muted);font-weight:800;text-transform:uppercase;letter-spacing:.06em;margin:2px 0 8px}',
            '.std-modal-backdrop{position:fixed;inset:0;z-index:1000;background:rgba(1,4,9,.72);display:flex;align-items:flex-start;justify-content:center;padding:42px 16px;overflow:auto}.std-modal{width:min(1180px,calc(100vw - 32px));max-width:100%;background:var(--c-surf);border:1px solid var(--c-border);border-radius:8px;box-shadow:0 18px 60px rgba(0,0,0,.42)}.std-modal-head{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:13px 14px;border-bottom:1px solid var(--c-border)}.std-modal-title{font-size:.95rem;font-weight:800}.std-close{width:28px;height:28px;border-radius:6px;border:1px solid var(--c-border);background:var(--c-elev);color:var(--c-muted);cursor:pointer;font-size:1.1rem;line-height:1}.std-close:hover{color:var(--c-text);border-color:var(--c-accent)}',
            '.std-form{padding:14px}.std-section-title{margin:4px 0 10px;padding:10px 12px;background:var(--c-elev);border-top:1px solid var(--c-border);border-bottom:1px solid var(--c-border);font-size:.8rem;font-weight:800;color:var(--c-text)}.std-form-grid{display:grid;grid-template-columns:repeat(2,minmax(180px,1fr));gap:10px}.std-form-grid-one{grid-template-columns:minmax(180px,1fr) 1fr}.std-form label{display:flex;flex-direction:column;gap:5px;min-width:0;color:var(--c-muted);font-size:.72rem;font-weight:800;text-transform:uppercase;letter-spacing:.04em}.std-form input,.std-form select{box-sizing:border-box;width:100%;min-width:0;height:34px;border-radius:6px;border:1px solid var(--c-border);background:var(--c-elev);color:var(--c-text);padding:0 9px;font-size:.82rem;text-transform:none;letter-spacing:0;font-weight:600}.std-form input:focus,.std-form select:focus{outline:none;border-color:var(--c-accent)}.std-form input:disabled,.std-form select:disabled{opacity:.55;cursor:not-allowed}.std-check-row,.std-check-col{display:flex;gap:14px;flex-wrap:wrap;margin-top:12px}.std-check-col{flex-direction:column;gap:8px}.std-check{flex-direction:row!important;align-items:center;color:var(--c-text)!important;text-transform:none!important;letter-spacing:0!important}.std-check input{flex:0 0 auto;width:16px;height:16px}.std-form-actions{display:flex;justify-content:flex-end;gap:8px;margin-top:14px}.std-test-row{display:flex;align-items:center;gap:10px;margin-top:12px;flex-wrap:wrap}.std-test-result{font-size:.78rem;color:var(--c-muted)}.std-test-ok{color:var(--c-success)}.std-test-err{color:var(--c-danger)}.std-tier-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:10px;margin-top:12px}.std-tier-form{min-width:0;overflow:hidden;border:1px solid var(--c-border);border-radius:7px;padding:10px;background:rgba(255,255,255,.02);display:grid;gap:9px}.std-tier-form-disabled{opacity:.58;background:rgba(255,255,255,.01)}.std-tier-form-head{display:flex;align-items:center;justify-content:space-between;gap:8px;min-width:0;flex-wrap:wrap}.std-tier-form .std-check{margin:0;white-space:nowrap}.std-tier-form label{gap:4px}',
            '@media(max-width:1100px){.std-main-grid,.std-split,.std-form-grid,.std-tier-grid,.std-camera-toolbar,.std-camera-result-grid{grid-template-columns:1fr}.std-chart{height:210px}.std-modal-backdrop{padding:20px 10px}.std-panel-head{align-items:flex-start;flex-direction:column}.std-camera-toolbar{align-items:stretch}.std-camera-check{justify-content:flex-start}}'
        ].join('\n');
        document.head.appendChild(style);
    }

    window.initStorageTierDashboard = function () {
        _injectCss();
        _renderShell();
        _load();
    };

    window.addEventListener('resize', function () {
        if (_state.chart) _state.chart.resize();
    });
})();
