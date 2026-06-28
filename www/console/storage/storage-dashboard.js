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
        chart: null,
        loading: false
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
                    '<div class="std-panel-title">Pools</div>' +
                    '<div id="std-pools"></div>' +
                '</div>' +
                '<div class="std-split">' +
                    '<div class="std-panel">' +
                        '<div class="std-panel-title">Policies</div>' +
                        '<div id="std-policies"></div>' +
                    '</div>' +
                    '<div class="std-panel">' +
                        '<div class="std-panel-title">Camera được cấu hình</div>' +
                        '<div id="std-cameras"></div>' +
                    '</div>' +
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
            S3Auth.apiPost('/media/mserver/storage/dashboard/detail', {}).then(function (res) { return res.data || {}; }),
            S3Auth.apiPost('/media/mserver/storage/dashboard/summary', {}).then(function (res) { return res.data || {}; }),
            _api('/media/mserver/storage/alert/list', { page: 0, size: 20 }),
            _api('/media/mserver/storage/restoreJob/list', { page: 0, size: 10 }),
            _api('/media/mserver/storage/tieringJob/list', { page: 0, size: 10 }),
            _api('/media/mserver/storage/expiredSegment/list', { page: 0, size: 10 }),
            _api('/media/mserver/storage/protected/list', { page: 0, size: 10 })
        ]).then(function (res) {
                _state.data = res[0] || {};
                _state.summary = res[1] || {};
                _state.alerts = (res[2] && res[2].items) || [];
                _state.restoreJobs = (res[3] && res[3].items) || [];
                _state.tieringJobs = (res[4] && res[4].items) || [];
                _state.expiredSegments = (res[5] && res[5].items) || [];
                _state.protectedVideos = (res[6] && res[6].items) || [];
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

    function _renderData(data, docSummary) {
        var body = document.getElementById('std-body');
        if (body) body.style.display = '';
        var summary = docSummary || {};
        _renderKpis(summary, data);
        _renderTierChart(summary, data.summary || {});
        _renderJobCounts(data.job_counts || {}, summary);
        _renderTierSummary(summary.tier_summary || []);
        _renderAlerts(_state.alerts, summary.alerts || []);
        _renderPools(data.pools || []);
        _renderPolicies(data.policies || []);
        _renderCameras(data.configured_cameras || []);
        _renderJobs(_state.tieringJobs.length ? _state.tieringJobs : (data.recent_jobs || []));
        _renderRestoreJobs(_state.restoreJobs);
        _renderExpiredSegments(_state.expiredSegments);
        _renderProtectedVideos(_state.protectedVideos);
    }

    function _renderKpis(summary, data) {
        var pools = data.pools || [];
        var policies = data.policies || [];
        var cameras = data.configured_cameras || [];
        var total = _num(summary.total_bytes || (data.summary && data.summary.total_bytes));
        var used = _num(summary.used_bytes || (data.summary && data.summary.total_used_bytes));
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

    function _renderTierChart(summary, legacySummary) {
        var el = document.getElementById('std-tier-chart');
        if (!el || !window.echarts) return;
        if (!_state.chart) _state.chart = echarts.init(el, null, { renderer: 'canvas' });
        var tiers = _tierMap(summary.tier_summary || [], legacySummary && legacySummary.tiers);
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

    function _tierMap(list, legacy) {
        if (legacy) return legacy;
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
            '</tr>';
        }).join('');
        document.getElementById('std-pools').innerHTML = _table(
            ['Pool', 'Tier', 'Type', 'Health', 'Usage', 'State', 'Last check'], rows);
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
            '</tr>';
        }).join('');
        document.getElementById('std-policies').innerHTML = _table(
            ['Policy', 'State', 'Retention', 'Tiers', 'Cameras'], rows);
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
            '</tr>';
        }).join('');
        document.getElementById('std-cameras').innerHTML = _table(
            ['Camera', 'IP', 'Policy áp dụng', 'Policy state', 'Assigned'], rows);
    }

    function _renderJobs(jobs) {
        if (!jobs.length) {
            document.getElementById('std-jobs').innerHTML = '<div class="std-empty">Không có job gần đây</div>';
            return;
        }
        var rows = jobs.map(function (j) {
            var total = _num(j.total_bytes || j.bytes_total);
            var processed = _num(j.processed_bytes || j.bytes_moved);
            var pct = j.progress_percent != null ? _num(j.progress_percent) : (total > 0 ? processed * 100 / total : 0);
            return '<tr>' +
                '<td><div class="std-name">' + _esc(j.job_id) + '</div><div class="std-id">' + _esc(j.camera_name || j.camera_id) + '</div></td>' +
                '<td>' + _tierBadge(j.source_tier) + ' <span class="std-arrow">→</span> ' + _tierBadge(j.target_tier) + '</td>' +
                '<td><span class="std-status-chip ' + _statusClass(j.status) + '">' + _esc(j.status) + '</span></td>' +
                '<td><div class="std-meter"><span style="width:' + Math.min(100, pct).toFixed(1) + '%"></span></div><div class="std-meter-label">' + _fmtBytes(processed) + ' / ' + _fmtBytes(total) + '</div></td>' +
                '<td>' + _fmtTs(j.created_at) + '</td>' +
                '<td>' + _esc(j.error_message || j.error_code || '') + '</td>' +
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
            return '<tr>' +
                '<td><div class="std-name">' + _esc(s.camera_name || s.camera_id) + '</div><div class="std-id">' + _esc(s.segment_id) + '</div></td>' +
                '<td>' + _tierBadge(s.tier) + '</td>' +
                '<td>' + _fmtTs(s.start_time) + '<div class="std-id">' + _fmtTs(s.end_time) + '</div></td>' +
                '<td class="std-num">' + _fmtBytes(s.size_bytes) + '</td>' +
                '<td>' + _esc(s.protected ? 'Protected' : 'Normal') + '</td>' +
                '<td>' + _fmtTs(s.expired_at) + '</td>' +
            '</tr>';
        }).join('');
        document.getElementById('std-expired-segments').innerHTML = _table(
            ['Segment', 'Tier', 'Time range', 'Size', 'Flag', 'Expired'], rows);
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
            '.std-job-counts{display:grid;grid-template-columns:repeat(2,minmax(110px,1fr));gap:8px}.std-job-pill{display:flex;align-items:center;justify-content:space-between;border:1px solid currentColor;border-radius:6px;padding:9px 10px;background:rgba(255,255,255,.02)}.std-job-pill span{font-size:.72rem;font-weight:800}.std-job-pill b{font-size:1.1rem;font-family:monospace}',
            '.std-table-wrap{overflow-x:auto;border:1px solid var(--c-border);border-radius:7px}.std-table{width:100%;border-collapse:collapse;font-size:.8rem}.std-table th{padding:8px 10px;text-align:left;font-size:.68rem;color:var(--c-muted);font-weight:800;text-transform:uppercase;letter-spacing:.06em;background:var(--c-elev);border-bottom:1px solid var(--c-border);white-space:nowrap}.std-table td{padding:8px 10px;border-bottom:1px solid var(--c-border);vertical-align:top}.std-table tbody tr:last-child td{border-bottom:none}.std-table tbody tr:hover td{background:rgba(255,255,255,.025)}',
            '.std-name{font-weight:700;color:var(--c-text);white-space:nowrap}.std-id{font-size:.68rem;color:var(--c-muted);font-family:monospace;word-break:break-all;margin-top:2px}.std-num{text-align:right;font-family:monospace;font-weight:700}',
            '.std-tier{display:inline-flex;align-items:center;height:20px;padding:0 7px;border-radius:10px;font-size:.68rem;font-weight:800;border:1px solid currentColor}.std-tier-hot{color:#f778ba}.std-tier-warm{color:#d29922}.std-tier-cold{color:#79c0ff}.std-arrow{color:var(--c-muted);margin:0 4px}',
            '.std-status-chip{display:inline-flex;align-items:center;height:21px;padding:0 8px;border-radius:10px;font-size:.68rem;font-weight:800;border:1px solid currentColor}.std-meter{height:8px;background:var(--c-elev);border-radius:4px;overflow:hidden;min-width:120px}.std-meter span{display:block;height:100%;background:var(--c-accent);border-radius:4px}.std-meter-label{font-size:.68rem;color:var(--c-muted);font-family:monospace;margin-top:4px;white-space:nowrap}.std-empty{padding:18px;color:var(--c-muted);font-size:.8rem;border:1px dashed var(--c-border);border-radius:7px;text-align:center}',
            '@media(max-width:1100px){.std-main-grid,.std-split{grid-template-columns:1fr}.std-chart{height:210px}}'
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
