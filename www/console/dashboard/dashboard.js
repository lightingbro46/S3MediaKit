// =============================================================================
// dashboard/dashboard.js — Real-time Monitoring Dashboard
// APIs used (secret-based):
//   /index/api/getThreadsLoad      → EventPoller thread load + fd_count + delay
//   /index/api/getWorkThreadsLoad  → WorkThread load
//   /index/api/getStatistic        → Object counters (MediaSource, TcpSession, …)
//   /media/api/systemStatistic     → CPU/RAM/HDD/Net
// =============================================================================

// ────────────────────────────────────────────────────────────────────────────
// Ring-buffer for sparkline time-series
// ────────────────────────────────────────────────────────────────────────────
function RingBuffer(size) {
    this.buf = [];
    this.max = size;
    this.push = function (v) {
        this.buf.push(v);
        if (this.buf.length > this.max) this.buf.shift();
    };
    this.values = function () { return this.buf.slice(); };
}

// ────────────────────────────────────────────────────────────────────────────
// SVG sparkline renderer
// ────────────────────────────────────────────────────────────────────────────
function renderSparkline(svgEl, data, color, maxVal) {
    if (!svgEl) return;
    const W = svgEl.clientWidth || 220;
    const H = svgEl.clientHeight || 44;
    const pts = data;
    const n   = pts.length;
    if (n < 2) return;
    const cap  = maxVal || (Math.max(...pts, 1));
    const xs   = i => (i / (n - 1)) * W;
    const ys   = v => H - Math.max(2, (v / cap) * (H - 4));
    const d    = pts.map((v, i) => (i === 0 ? 'M' : 'L') + xs(i).toFixed(1) + ',' + ys(v).toFixed(1)).join(' ');
    svgEl.innerHTML =
        '<defs><linearGradient id="sg_' + svgEl.id + '" x1="0" y1="0" x2="0" y2="1">' +
        '<stop offset="0%" stop-color="' + color + '" stop-opacity="0.25"/>' +
        '<stop offset="100%" stop-color="' + color + '" stop-opacity="0.02"/></linearGradient></defs>' +
        '<path d="' + d + ' L' + xs(n-1) + ',' + H + ' L0,' + H + ' Z" fill="url(#sg_' + svgEl.id + ')"/>' +
        '<path d="' + d + '" fill="none" stroke="' + color + '" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/>' +
        '<circle cx="' + xs(n-1) + '" cy="' + ys(pts[n-1]) + '" r="2.5" fill="' + color + '"/>';
}

// ────────────────────────────────────────────────────────────────────────────
// Donut/ring gauge
// ────────────────────────────────────────────────────────────────────────────
function renderGauge(svgEl, pct, color) {
    if (!svgEl) return;
    const r   = 28;
    const cx  = 36; const cy = 36;
    const circ = 2 * Math.PI * r;
    const dash = Math.max(0, Math.min(1, pct / 100)) * circ;
    svgEl.innerHTML =
        '<circle cx="' + cx + '" cy="' + cy + '" r="' + r + '" fill="none" stroke="#21262d" stroke-width="7"/>' +
        '<circle cx="' + cx + '" cy="' + cy + '" r="' + r + '" fill="none" stroke="' + color + '" stroke-width="7"' +
        ' stroke-dasharray="' + dash.toFixed(2) + ' ' + circ.toFixed(2) + '"' +
        ' stroke-dashoffset="' + (circ * 0.25).toFixed(2) + '" stroke-linecap="round"' +
        ' style="transition:stroke-dasharray 0.6s ease"/>';
}

// ────────────────────────────────────────────────────────────────────────────
// Thread load bar row
// ────────────────────────────────────────────────────────────────────────────
function renderThreadBars(containerId, threads) {
    const el = document.getElementById(containerId);
    if (!el) return;
    el.innerHTML = threads.map(t => {
        const pct  = Math.min(100, Math.max(0, t.load || 0));
        const delay = t.delay != null ? t.delay + 'ms' : '';
        const fd   = t.fd_count != null ? t.fd_count + ' fds' : '';
        const col  = pct > 80 ? '#f85149' : pct > 50 ? '#d29922' : '#3fb950';
        return '<div class="thr-row">' +
            '<span class="thr-name">' + (t.name || 'thread') + '</span>' +
            '<div class="thr-bar-wrap"><div class="thr-bar" style="width:' + pct + '%;background:' + col + '">' +
            '</div></div>' +
            '<span class="thr-pct">' + pct + '%</span>' +
            (delay ? '<span class="thr-meta">' + delay + '</span>' : '') +
            (fd    ? '<span class="thr-meta">' + fd    + '</span>' : '') +
            '</div>';
    }).join('');
}

// ────────────────────────────────────────────────────────────────────────────
// Global state
// ────────────────────────────────────────────────────────────────────────────
var _dashState = {
    series: {
        tcpSessions:   new RingBuffer(60),
        mediaSources:  new RingBuffer(60),
        udpSessions:   new RingBuffer(60),
        rtpPackets:    new RingBuffer(60),
        cpuProc:       new RingBuffer(60),
        ramProc:       new RingBuffer(60),
        netRx:         new RingBuffer(60),
        netTx:         new RingBuffer(60),
    },
    pollerTimer: null,
    sysStatAvail: null,  // null=unknown, true, false
    lastEventThreads: [],
    lastWorkThreads: [],
    initialized: false,
};
var _epAvgSeries = new RingBuffer(60);
var _wtAvgSeries = new RingBuffer(60);

// ────────────────────────────────────────────────────────────────────────────
// Dashboard entry point
// ────────────────────────────────────────────────────────────────────────────
function initDashboard() {
    var auth = window.S3Auth && S3Auth.get();
    if (!auth) return;

    if (!_dashState.initialized) {
        _dashState.initialized = true;
        _buildDashboardLayout();
        _startPolling();
    } else {
        // Already init, just re-render static info and refresh
        _updateStaticInfo();
    }
}

function _buildDashboardLayout() {
    var el = document.getElementById('dashboard-content');
    if (!el) return;
    var auth = S3Auth.get() || {};
    var srv  = auth.serverUrl || '—';

    el.innerHTML =
        // ── Row 1: Quick-stat cards ──────────────────────────────────────
        '<div class="dash-row-title">Thống kê nhanh</div>' +
        '<div class="dash-cards-row" id="dash-quick-cards">' +
            _qcard('dash-card-tcp',    'TCP Sessions',    '—', '#388bfd') +
            _qcard('dash-card-media',  'Active Streams',  '—', '#3fb950') +
            _qcard('dash-card-viewers','Viewers',          '—', '#d29922') +
            _qcard('dash-card-udp',    'UDP Sessions',    '—', '#8b949e') +
            _qcard('dash-card-fd',     'File Descriptors','—', '#79c0ff') +
        '</div>' +

        // ── Row 2: System resources (CPU/RAM/HDD) ──────────────────────
        '<div class="dash-row-title" style="margin-top:22px">' +
            'Tài nguyên hệ thống ' +
            '<span id="sysstat-badge" class="dash-badge-info" title="Endpoint /media/api/systemStatistic cần api.secret trong config.ini">loading…</span>' +
        '</div>' +
        '<div class="dash-sys-row">' +
            _sysGauge('dash-cpu-gauge',   'CPU',     '—%') +
            _sysGauge('dash-ram-gauge',   'RAM',     '—%') +
            _sysGauge('dash-disk-gauge',  'Disk',    '—%') +
            _netCard() +
        '</div>' +

        // ── Row 3: Time-series sparklines ──────────────────────────────
        '<div class="dash-row-title" style="margin-top:22px">Xu hướng theo thời gian (3s/điểm)</div>' +
        '<div class="dash-spark-row">' +
            _sparkCard('dash-spark-tcp',   'TCP Sessions',   '#388bfd') +
            _sparkCard('dash-spark-media', 'Active Streams', '#3fb950') +
            _sparkCard('dash-spark-net',   'Net RX Mbps',    '#e3b341') +
        '</div>' +

        // ── Row 4: Event Poller threads ────────────────────────────────
        '<div class="dash-row-title" style="margin-top:22px">' +
            'Event Poller Threads ' +
            '<span id="dash-ep-avg-badge" class="dash-badge-info">avg —%</span>' +
            '<button class="dash-collapse-btn" onclick="_toggleThreadPanel(\'dash-ep-detail\')" id="dash-ep-toggle">▶ Chi tiết</button>' +
        '</div>' +
        '<div class="dash-thread-panel" id="dash-panel-ep">' +
            '<div id="dash-ep-sparkline-wrap" class="dash-spark-mini-wrap">' +
                '<svg id="dash-ep-spark" class="dash-spark-svg" style="height:44px"></svg>' +
                '<div class="dash-spark-lbl">Avg load %</div>' +
            '</div>' +
        '</div>' +
        '<div class="dash-thread-detail" id="dash-ep-detail" style="display:none">' +
            '<div id="dash-ep-bars" class="dash-ep-bars-wrap"><div class="dash-thr-loading">Đang tải…</div></div>' +
        '</div>' +

        // ── Row 5: Work threads ────────────────────────────────────────────────
        '<div class="dash-row-title" style="margin-top:22px">' +
            'Work Threads ' +
            '<span id="dash-wt-avg-badge" class="dash-badge-info">avg —%</span>' +
            '<button class="dash-collapse-btn" onclick="_toggleThreadPanel(\'dash-wt-detail\')" id="dash-wt-toggle">▶ Chi tiết</button>' +
        '</div>' +
        '<div class="dash-thread-panel" id="dash-panel-wt">' +
            '<div id="dash-wt-sparkline-wrap" class="dash-spark-mini-wrap">' +
                '<svg id="dash-wt-spark" class="dash-spark-svg" style="height:44px"></svg>' +
                '<div class="dash-spark-lbl">Avg load %</div>' +
            '</div>' +
        '</div>' +
        '<div class="dash-thread-detail" id="dash-wt-detail" style="display:none">' +
            '<div id="dash-wt-bars" class="dash-wt-bars-wrap"><div class="dash-thr-loading">Đang tải…</div></div>' +
        '</div>' +

        // ── Row 6: Device viewer table ─────────────────────────────────
        '<div class="dash-row-title" style="margin-top:22px">Viewer theo thiết bị</div>' +
        '<div id="dash-viewer-table-wrap">' +
            '<p class="dash-note" id="dash-viewer-note">Chưa có dữ liệu</p>' +
        '</div>' +

        // ── Bottom toolbar ─────────────────────────────────────────────
        '<div class="dash-footer-bar">' +
            '<span class="dash-footer-ts" id="dash-last-update">Chưa cập nhật</span>' +
            '<button class="dash-btn" onclick="_dashRefresh()">↺ Làm mới ngay</button>' +
        '</div>';

    _updateStaticInfo();
    _injectDashCSS();
}

function _qcard(id, label, val, col) {
    return '<div class="dash-qcard" id="' + id + '">' +
        '<div class="dash-qcard-val" style="color:' + col + '">' + val + '</div>' +
        '<div class="dash-qcard-lbl">' + label + '</div>' +
    '</div>';
}
function _sysGauge(id, label, init) {
    return '<div class="dash-sys-gauge" id="' + id + '">' +
        '<svg class="dash-gauge-svg" viewBox="0 0 72 72" id="' + id + '_svg"></svg>' +
        '<div class="dash-gauge-pct" id="' + id + '_pct">' + init + '</div>' +
        '<div class="dash-gauge-lbl">' + label + '</div>' +
    '</div>';
}
function _netCard() {
    return '<div class="dash-net-card" id="dash-net-card">' +
        '<div class="dash-net-row"><span class="dash-net-lbl">⬇ RX</span><span class="dash-net-val" id="dash-net-rx">—</span></div>' +
        '<div class="dash-net-row"><span class="dash-net-lbl">⬆ TX</span><span class="dash-net-val" id="dash-net-tx">—</span></div>' +
        '<div class="dash-net-iface" id="dash-net-iface">Interface: —</div>' +
    '</div>';
}
function _sparkCard(id, label, color) {
    return '<div class="dash-spark-card">' +
        '<svg class="dash-spark-svg" id="' + id + '" style="height:56px"></svg>' +
        '<div class="dash-spark-footer">' +
            '<span class="dash-spark-lbl">' + label + '</span>' +
            '<span class="dash-spark-val" id="' + id + '_val">—</span>' +
        '</div>' +
    '</div>';
}

function _updateStaticInfo() {
    var auth = S3Auth.get() || {};
    var cfg  = window._s3Config || {};
    if (auth.serverUrl) {
        var urlEl = document.getElementById('srv-url');
        if (urlEl) urlEl.textContent = auth.serverUrl.replace(/^https?:\/\//, '');
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Polling
// ────────────────────────────────────────────────────────────────────────────

function _toggleThreadPanel(detailId) {
    var detail = document.getElementById(detailId);
    if (!detail) return;
    var isOpen = detail.style.display !== 'none';
    detail.style.display = isOpen ? 'none' : 'block';
    // Update toggle button label
    var btnId = detailId === 'dash-ep-detail' ? 'dash-ep-toggle' : 'dash-wt-toggle';
    var btn = document.getElementById(btnId);
    if (btn) btn.textContent = isOpen ? '▶ Chi tiết' : '▼ Thu gọn';
}var _epAvgSeries = new RingBuffer(60);

function _startPolling() {
    _poll();
    _dashState.pollerTimer = setInterval(_poll, 3000);
}

async function _poll() {
    try {
        var results = await Promise.allSettled([
            S3Auth.apiFetch('/index/api/getStatistic'),
            S3Auth.apiFetch('/index/api/getThreadsLoad'),
            S3Auth.apiFetch('/index/api/getWorkThreadsLoad'),
            _fetchSystemStat(),
        ]);

        var statRes   = results[0].status === 'fulfilled' ? results[0].value : null;
        var epRes     = results[1].status === 'fulfilled' ? results[1].value : null;
        var wtRes     = results[2].status === 'fulfilled' ? results[2].value : null;
        var sysRes    = results[3].status === 'fulfilled' ? results[3].value : null;

        if (statRes)  _applyStatistic(statRes.data || {});
        if (epRes)    _applyEventPoller(epRes.data || []);
        if (wtRes)    _applyWorkThreads(wtRes.data || []);
        if (sysRes)   _applySysStat(sysRes);
        else if (_dashState.sysStatAvail === null) {
            _dashState.sysStatAvail = false;
            var badgeEl = document.getElementById('sysstat-badge');
            if (badgeEl) { badgeEl.textContent = 'Cần enableAuthorize=0'; badgeEl.className = 'dash-badge-warn'; badgeEl.title = 'Endpoint /media/api/systemStatistic cần api.secret trong config.ini'; }
        }

        var ts = new Date().toLocaleTimeString();
        var tsEl = document.getElementById('dash-last-update');
        if (tsEl) tsEl.textContent = 'Cập nhật lúc ' + ts;

    } catch (e) { /* silent */ }
}

async function _fetchSystemStat() {
    // Try without JWT — works when enableAuthorize=0
    var auth = S3Auth.get();
    if (!auth) throw new Error('no auth');
    var url = auth.serverUrl + '/media/api/systemStatistic?secret=' + encodeURIComponent(auth.secret);
    var r = await fetch(url, { credentials: 'omit' });
    var d = await r.json();
    if (d.code !== 0) throw new Error('not available');
    return d.data;
}

function _applyStatistic(d) {
    var tcp   = d.TcpSession   || 0;
    var media = d.MediaSource  || 0;
    var udp   = d.UdpSession   || 0;
    var rtp   = d.RtpPacket    || 0;
    var sock  = d.Socket       || 0;

    _setText('dash-card-tcp',    tcp);
    _setText('dash-card-media',  media);
    _setText('dash-card-udp',    udp);

    _dashState.series.tcpSessions.push(tcp);
    _dashState.series.mediaSources.push(media);
    _dashState.series.udpSessions.push(udp);

    _updateSparkline('dash-spark-tcp',   _dashState.series.tcpSessions.values(),  '#388bfd');
    _updateSparkline('dash-spark-media', _dashState.series.mediaSources.values(), '#3fb950');

    _setVal('dash-spark-tcp_val',   tcp);
    _setVal('dash-spark-media_val', media);
}

function _applyEventPoller(threads) {
    _dashState.lastEventThreads = threads;
    renderThreadBars('dash-ep-bars', threads);

    var avg = threads.length
        ? Math.round(threads.reduce(function(s, t) { return s + (t.load || 0); }, 0) / threads.length)
        : 0;
    _epAvgSeries.push(avg);
    _updateSparkline('dash-ep-spark', _epAvgSeries.values(), '#388bfd', 100);

    var badge = document.getElementById('dash-ep-avg-badge');
    if (badge) {
        badge.textContent = 'avg ' + avg + '%  (' + threads.length + ' threads)';
        badge.className = avg > 80 ? 'dash-badge-warn' : 'dash-badge-info';
    }

    // Combine fd_count into quick card
    var totalFd = threads.reduce(function(s, t) { return s + (t.fd_count || 0); }, 0);
    _setText('dash-card-fd', totalFd);
}

function _applyWorkThreads(threads) {
    _dashState.lastWorkThreads = threads;
    renderThreadBars('dash-wt-bars', threads);

    var avg = threads.length
        ? Math.round(threads.reduce(function(s, t) { return s + (t.load || 0); }, 0) / threads.length)
        : 0;
    _wtAvgSeries.push(avg);
    _updateSparkline('dash-wt-spark', _wtAvgSeries.values(), '#3fb950', 100);

    var badge = document.getElementById('dash-wt-avg-badge');
    if (badge) {
        badge.textContent = 'avg ' + avg + '%  (' + threads.length + ' threads)';
        badge.className = avg > 80 ? 'dash-badge-warn' : 'dash-badge-info';
    }
}

function _applySysStat(d) {
    var badgeEl = document.getElementById('sysstat-badge');

    // CPU
    if (d.cpu) {
        var cpuPct = Math.round(parseFloat(d.cpu.usage_pct) || 0);
        var procPct = Math.round(parseFloat(d.cpu.proc_usage_pct) || 0);
        _setText('dash-cpu-gauge_pct', cpuPct + '%');
        renderGauge(document.getElementById('dash-cpu-gauge_svg'), cpuPct, cpuPct > 80 ? '#f85149' : cpuPct > 50 ? '#d29922' : '#3fb950');
        _dashState.series.cpuProc.push(procPct);
        if (_dashState.sysStatAvail !== true) {
            _dashState.sysStatAvail = true;
            if (badgeEl) { badgeEl.textContent = 'live'; badgeEl.className = 'dash-badge-ok'; }
        }
    }

    // RAM
    if (d.ram) {
        var ramPct = Math.round(parseFloat(d.ram.usage_pct) || 0);
        var ramUsed = _fmtBytes(d.ram.used || 0);
        var ramTotal = _fmtBytes(d.ram.total || 0);
        _setText('dash-ram-gauge_pct', ramPct + '%');
        renderGauge(document.getElementById('dash-ram-gauge_svg'), ramPct, ramPct > 85 ? '#f85149' : ramPct > 60 ? '#d29922' : '#64b5f6');
        _dashState.series.ramProc.push(ramPct);
    }

    // Disk — first partition
    if (d.disks && d.disks.length) {
        var dk = d.disks[0];
        var dkPct = Math.round(parseFloat(dk.used_pct) || 0);
        _setText('dash-disk-gauge_pct', dkPct + '%');
        renderGauge(document.getElementById('dash-disk-gauge_svg'), dkPct, dkPct > 90 ? '#f85149' : dkPct > 70 ? '#d29922' : '#8b949e');
    }

    // Network — sum all interfaces (rx_mbps/tx_mbps are strings from sanitize_for_json)
    if (d.nets != null) {
        if (d.nets.length) {
            var rxTotal = 0; var txTotal = 0; var ifaces = [];
            d.nets.forEach(function(n) { rxTotal += parseFloat(n.rx_mbps) || 0; txTotal += parseFloat(n.tx_mbps) || 0; ifaces.push(n.name); });
            _setVal('dash-net-rx', rxTotal.toFixed(2) + ' Mbps');
            _setVal('dash-net-tx', txTotal.toFixed(2) + ' Mbps');
            _setVal('dash-net-iface', ifaces.join(', '));
            _dashState.series.netRx.push(rxTotal);
            _dashState.series.netTx.push(txTotal);
            _updateSparkline('dash-spark-net', _dashState.series.netRx.values(), '#e3b341');
            _setVal('dash-spark-net_val', rxTotal.toFixed(2) + ' Mbps');
        } else {
            _setVal('dash-net-rx', 'N/A');
            _setVal('dash-net-tx', 'N/A');
            _setVal('dash-net-iface', 'Không có interface');
        }
    }

    // Viewers (from reader data)
    if (d.reader) {
        var tv = d.reader.totalStreamCount || 0;
        var lv = d.reader.liveStreamCount || 0;
        var pb = d.reader.playbackStreamCount || 0;
        _setText('dash-card-viewers', tv);
        _renderViewerTable(d.reader);
    }
}

function _renderViewerTable(reader) {
    var wrap = document.getElementById('dash-viewer-table-wrap');
    if (!wrap) return;
    var devices = reader.devices || [];
    if (!devices.length) {
        wrap.innerHTML = '<p class="dash-note">Không có viewer đang xem</p>';
        return;
    }
    var rows = devices.map(function(dev) {
        return '<tr>' +
            '<td>' + (dev.name || dev.ipAddress || '—') + '</td>' +
            '<td>' + (dev.ipAddress || '—') + '</td>' +
            '<td class="tc">' + (dev.liveStreamCount != null ? dev.liveStreamCount : (dev.live_count || 0)) + '</td>' +
            '<td class="tc">' + (dev.playbackStreamCount != null ? dev.playbackStreamCount : (dev.playback_count || 0)) + '</td>' +
            '<td class="tc fw">' + (dev.totalStreamCount != null ? dev.totalStreamCount : (dev.total_count || 0)) + '</td>' +
        '</tr>';
    }).join('');
    wrap.innerHTML =
        '<div class="dash-summary-row" style="margin-bottom:8px">' +
            '<span class="dash-badge-info">Live: ' + (reader.liveStreamCount || 0) + '</span>' +
            '<span class="dash-badge-ok" style="margin-left:6px">Playback: ' + (reader.playbackStreamCount || 0) + '</span>' +
            '<span class="dash-badge-warn" style="margin-left:6px">Devices xem: ' + (reader.activeViewingDeviceCount || 0) + '</span>' +
        '</div>' +
        '<div class="dash-table-scroll"><table class="dash-table">' +
            '<thead><tr><th>Thiết bị</th><th>IP</th><th>Live</th><th>Playback</th><th>Tổng</th></tr></thead>' +
            '<tbody>' + rows + '</tbody>' +
        '</table></div>';
}

// ────────────────────────────────────────────────────────────────────────────
// helpers
// ────────────────────────────────────────────────────────────────────────────
function _setText(id, v) {
    var el = document.getElementById(id);
    if (!el) return;
    // For quick cards, find .dash-qcard-val child
    var valEl = el.querySelector('.dash-qcard-val') || el;
    valEl.textContent = v;
}
function _setVal(id, v) {
    var el = document.getElementById(id);
    if (el) el.textContent = v;
}
function _updateSparkline(id, data, color, maxVal) {
    var el = document.getElementById(id);
    if (el) renderSparkline(el, data, color, maxVal);
}
function _fmtBytes(b) {
    if (b > 1073741824) return (b / 1073741824).toFixed(1) + ' GB';
    if (b > 1048576)    return (b / 1048576).toFixed(0)    + ' MB';
    return (b / 1024).toFixed(0) + ' KB';
}
function _dashRefresh() {
    _poll();
}

// ────────────────────────────────────────────────────────────────────────────
// Inline CSS injection (dashboard-specific styles)
// ────────────────────────────────────────────────────────────────────────────
function _injectDashCSS() {
    if (document.getElementById('dash-styles')) return;
    var s = document.createElement('style');
    s.id  = 'dash-styles';
    s.textContent = `
    /* ─── Dashboard layout ─────────────────────────────────────── */
    #dashboard-content { padding-bottom: 32px; }
    .dash-row-title {
        font-size: 0.74rem; font-weight: 700; color: var(--c-muted);
        text-transform: uppercase; letter-spacing: 0.07em;
        margin-bottom: 10px; display: flex; align-items: center; gap: 8px;
    }
    .dash-badge-ok   { display:inline-block;padding:1px 7px;border-radius:10px;font-size:0.68rem;font-weight:600;background:rgba(63,185,80,0.15);color:var(--c-success);border:1px solid rgba(63,185,80,0.3); }
    .dash-badge-warn { display:inline-block;padding:1px 7px;border-radius:10px;font-size:0.68rem;font-weight:600;background:rgba(210,153,34,0.15);color:var(--c-warn);border:1px solid rgba(210,153,34,0.3); }
    .dash-badge-info { display:inline-block;padding:1px 7px;border-radius:10px;font-size:0.68rem;font-weight:600;background:rgba(56,139,253,0.12);color:var(--c-accent);border:1px solid rgba(56,139,253,0.25); }
    .dash-note { font-size:0.78rem; color: var(--c-muted); padding: 12px 0; }

    /* ─── Quick cards ───────────────────────────────────────────── */
    .dash-cards-row {
        display: grid;
        grid-template-columns: repeat(auto-fill, minmax(140px, 1fr));
        gap: 10px; margin-bottom: 0;
    }
    .dash-qcard {
        background: var(--c-surf); border: 1px solid var(--c-border);
        border-radius: 8px; padding: 14px 14px 10px;
        display: flex; flex-direction: column; gap: 4px;
        transition: border-color 0.15s;
    }
    .dash-qcard:hover { border-color: rgba(56,139,253,0.35); }
    .dash-qcard-val { font-size: 1.5rem; font-weight: 700; font-family: monospace; line-height: 1; }
    .dash-qcard-lbl { font-size: 0.7rem; color: var(--c-muted); font-weight: 600; text-transform: uppercase; letter-spacing: 0.06em; }

    /* ─── System gauges ─────────────────────────────────────────── */
    .dash-sys-row {
        display: flex; gap: 14px; flex-wrap: wrap;
    }
    .dash-sys-gauge {
        background: var(--c-surf); border: 1px solid var(--c-border);
        border-radius: 8px; padding: 14px 18px 10px;
        display: flex; flex-direction: column; align-items: center; gap: 4px;
        min-width: 96px;
    }
    .dash-gauge-svg { width: 72px; height: 72px; }
    .dash-gauge-pct { font-size: 1rem; font-weight: 700; font-family: monospace; color: var(--c-text); }
    .dash-gauge-lbl { font-size: 0.68rem; color: var(--c-muted); text-transform: uppercase; letter-spacing: 0.07em; font-weight: 600; }
    .dash-net-card {
        background: var(--c-surf); border: 1px solid var(--c-border);
        border-radius: 8px; padding: 14px 18px;
        display: flex; flex-direction: column; justify-content: center; gap: 8px;
        min-width: 160px;
    }
    .dash-net-row { display: flex; align-items: center; gap: 10px; }
    .dash-net-lbl { font-size: 0.74rem; color: var(--c-muted); font-weight: 600; min-width: 28px; }
    .dash-net-val { font-size: 0.9rem; font-weight: 700; font-family: monospace; color: var(--c-text); }
    .dash-net-iface { font-size: 0.68rem; color: var(--c-muted); font-family: monospace; }

    /* ─── Sparklines ────────────────────────────────────────────── */
    .dash-spark-row { display: grid; grid-template-columns: repeat(auto-fill, minmax(200px,1fr)); gap: 10px; }
    .dash-spark-card {
        background: var(--c-surf); border: 1px solid var(--c-border);
        border-radius: 8px; overflow: hidden;
    }
    .dash-spark-svg { width: 100%; display: block; }
    .dash-spark-footer { display: flex; align-items: center; justify-content: space-between; padding: 6px 10px 8px; }
    .dash-spark-lbl { font-size: 0.7rem; color: var(--c-muted); font-weight: 600; text-transform: uppercase; letter-spacing: 0.05em; }
    .dash-spark-val { font-size: 0.84rem; font-weight: 700; font-family: monospace; color: var(--c-text); }
    .dash-spark-mini-wrap { display: flex; flex-direction: column; gap: 4px; }

    /* ─── Thread panels ──────────────────────────────────────────── */
    .dash-thread-panel {
        background: var(--c-surf); border: 1px solid var(--c-border);
        border-radius: 8px; padding: 10px 16px;
        display: flex; gap: 16px; align-items: flex-start; flex-wrap: wrap;
    }
    .dash-thread-detail {
        background: var(--c-surf); border: 1px solid var(--c-border);
        border-top: none; border-radius: 0 0 8px 8px;
        padding: 10px 16px 14px; margin-top: -4px;
    }
    .dash-collapse-btn {
        display: inline-flex; align-items: center; gap: 4px;
        padding: 2px 9px; border-radius: 4px; font-size: 0.7rem; font-weight: 600;
        cursor: pointer; border: 1px solid var(--c-border); background: var(--c-elev);
        color: var(--c-muted); transition: background 0.1s, color 0.1s;
    }
    .dash-collapse-btn:hover { background: var(--c-surf); color: var(--c-text); border-color: var(--c-accent); }
    .dash-ep-bars-wrap, .dash-wt-bars-wrap { flex: 1; min-width: 200px; display: flex; flex-direction: column; gap: 5px; }
    .dash-spark-mini-wrap { flex: 0 0 220px; }
    .dash-thr-loading { font-size: 0.78rem; color: var(--c-muted); }
    .thr-row {
        display: flex; align-items: center; gap: 8px;
        font-size: 0.74rem; font-family: monospace;
    }
    .thr-name { color: var(--c-text); min-width: 80px; flex-shrink: 0; font-size: 0.7rem; }
    .thr-bar-wrap { flex: 1; height: 10px; background: var(--c-elev); border-radius: 5px; overflow: hidden; }
    .thr-bar { height: 100%; border-radius: 5px; transition: width 0.4s ease; }
    .thr-pct { color: var(--c-text); min-width: 36px; text-align: right; font-size: 0.72rem; }
    .thr-meta { color: var(--c-muted); font-size: 0.66rem; min-width: 55px; }

    /* ─── Viewer table ────────────────────────────────────────────── */
    .dash-table-scroll { overflow-x: auto; border-radius: 6px; border: 1px solid var(--c-border); }
    .dash-table { width: 100%; border-collapse: collapse; font-size: 0.8rem; }
    .dash-table th {
        padding: 8px 12px; text-align: left; font-size: 0.7rem; font-weight: 600;
        color: var(--c-muted); text-transform: uppercase; letter-spacing: 0.05em;
        background: var(--c-elev); border-bottom: 1px solid var(--c-border);
    }
    .dash-table td { padding: 7px 12px; border-bottom: 1px solid var(--c-border); color: var(--c-text); }
    .dash-table tbody tr:last-child td { border-bottom: none; }
    .dash-table tbody tr:hover td { background: rgba(255,255,255,0.02); }
    .tc { text-align: center; }
    .fw { font-weight: 700; }
    .dash-summary-row { display: flex; flex-wrap: wrap; gap: 6px; }

    /* ─── Footer ─────────────────────────────────────────────────── */
    .dash-footer-bar {
        display: flex; align-items: center; justify-content: space-between;
        margin-top: 20px; padding-top: 14px; border-top: 1px solid var(--c-border);
    }
    .dash-footer-ts { font-size: 0.72rem; color: var(--c-muted); font-family: monospace; }
    .dash-btn {
        display: inline-flex; align-items: center; gap: 6px;
        padding: 6px 12px; border-radius: 5px; font-size: 0.78rem; font-weight: 600;
        cursor: pointer; border: 1px solid var(--c-border); background: var(--c-elev);
        color: var(--c-text); transition: background 0.12s, border-color 0.12s;
    }
    .dash-btn:hover { background: var(--c-surf); border-color: var(--c-accent); }
    `;
    document.head.appendChild(s);
}