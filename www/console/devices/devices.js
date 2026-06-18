/* ════════════════════════════════════════════
   Devices Tab — Camera list
   ════════════════════════════════════════════ */
(function () {
    'use strict';

    /* ── state ── */
    var _activeSubTab = 'camera';
    var _cameras      = [];
    var _loading      = false;

    /* ── helpers ── */
    function _auth() { return window.S3Auth ? S3Auth.get() : { serverUrl: '', secret: '' }; }

    function _apiFetch(endpoint, params, cb) {
        var a = _auth();
        var url = a.serverUrl + endpoint + '?secret=' + encodeURIComponent(a.secret);
        if (params) {
            for (var k in params) {
                if (Object.prototype.hasOwnProperty.call(params, k)) {
                    url += '&' + encodeURIComponent(k) + '=' + encodeURIComponent(params[k]);
                }
            }
        }
        fetch(url)
            .then(function (r) { return r.json(); })
            .then(function (d) { cb(null, d); })
            .catch(function (e) { cb(e, null); });
    }

    /* ── format helpers ── */
    function _fmtBitrate(bps) {
        if (bps == null || bps < 0) return '—';
        if (bps === 0) return '0 bps';
        if (bps >= 1000000) return (bps / 1000000).toFixed(2) + ' Mbps';
        if (bps >= 1000)    return (bps / 1000).toFixed(1) + ' Kbps';
        return bps + ' bps';
    }

    function _fmtRes(w, h) {
        if (!w && !h) return '—';
        return w + '×' + h;
    }

    function _esc(s) {
        return String(s || '')
            .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
    }

    /* ── sub-tab switcher ── */
    function _switchSubTab(tab) {
        _activeSubTab = tab;
        document.querySelectorAll('.dv-subtab').forEach(function (btn) {
            btn.classList.toggle('active', btn.dataset.tab === tab);
        });
        document.querySelectorAll('.dv-panel').forEach(function (el) {
            el.style.display = el.dataset.panel === tab ? '' : 'none';
        });
    }

    /* ── camera load ── */
    function _loadCameras() {
        if (_loading) return;
        _loading = true;
        _setCameraStatus('loading', null);

        _apiFetch('/media/api/device/statisticsList', null, function (err, data) {
            _loading = false;
            if (err || !data) { _setCameraStatus('error', 'Lỗi kết nối tới server'); return; }
            if (data.code !== 0) { _setCameraStatus('error', data.msg || ('Lỗi: ' + data.code)); return; }
            _cameras = Array.isArray(data.data) ? data.data : [];
            _renderCameras();
        });
    }

    function _setCameraStatus(type, msg) {
        var el = document.getElementById('dv-cam-body');
        if (!el) return;
        if (type === 'loading') {
            el.innerHTML = '<tr><td colspan="9" class="dv-td-status"><span class="dv-spinner"></span> Đang tải…</td></tr>';
        } else if (type === 'error') {
            el.innerHTML = '<tr><td colspan="9" class="dv-td-status dv-td-err">'
                + '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" width="20" height="20"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><circle cx="12" cy="16" r="0.5" fill="currentColor" stroke="none"/></svg>'
                + _esc(msg) + '</td></tr>';
        } else if (type === 'empty') {
            el.innerHTML = '<tr><td colspan="9" class="dv-td-status dv-td-muted">Không có camera nào</td></tr>';
        }
    }

    function _renderCameras() {
        var el = document.getElementById('dv-cam-body');
        if (!el) return;

        var cameras = _cameras;

        // update counter badge
        var badge = document.getElementById('dv-cam-badge');
        if (badge) badge.textContent = cameras.length;

        var online  = cameras.filter(function (c) { return c.status; }).length;
        var offline = cameras.length - online;
        var sumEl = document.getElementById('dv-cam-summary');
        if (sumEl) {
            sumEl.innerHTML = '<span class="dv-sum-item dv-sum-total">Tổng: <b>' + cameras.length + '</b></span>'
                + '<span class="dv-sum-item dv-sum-on">Online: <b>' + online + '</b></span>'
                + '<span class="dv-sum-item dv-sum-off">Offline: <b>' + offline + '</b></span>';
        }

        if (cameras.length === 0) { _setCameraStatus('empty', null); return; }

        var rows = cameras.map(function (cam) {
            var statusCls = cam.status ? 'dv-status-on' : 'dv-status-off';
            var statusLbl = cam.status ? 'Online' : 'Offline';

            var name   = (cam.options && cam.options.name) || cam.deviceId || '—';
            var ip     = (cam.options && cam.options.ip)   || '—';
            var mfr    = (cam.options && cam.options.manufacturer) || '—';
            var model  = (cam.options && cam.options.model) || '—';

            /* Primary stream summary */
            var pri = cam.primaryStream;
            var priCell = pri
                ? '<span class="dv-stream-cell' + (pri.status ? ' dv-stream-on' : ' dv-stream-off') + '">'
                  + _esc(pri.vcodec || '—') + ' ' + _fmtRes(pri.width, pri.height) + ' '
                  + (pri.fps ? pri.fps + 'fps' : '') + '<br>'
                  + '<span class="dv-stream-bw">' + _fmtBitrate(pri.byteSpeed * 8) + '</span>'
                  + '</span>'
                : '<span class="dv-muted">—</span>';

            /* Secondary stream summary */
            var sec = cam.secondaryStream;
            var secCell = sec
                ? '<span class="dv-stream-cell' + (sec.status ? ' dv-stream-on' : ' dv-stream-off') + '">'
                  + _esc(sec.vcodec || '—') + ' ' + _fmtRes(sec.width, sec.height) + ' '
                  + (sec.fps ? sec.fps + 'fps' : '') + '<br>'
                  + '<span class="dv-stream-bw">' + _fmtBitrate(sec.byteSpeed * 8) + '</span>'
                  + '</span>'
                : '<span class="dv-muted">—</span>';

            /* Controller (ONVIF connection) */
            var ctrl = cam.controller;
            var ctrlCell = ctrl
                ? '<span class="' + (ctrl.status ? 'dv-ctrl-on' : 'dv-ctrl-off') + '">'
                  + (ctrl.status ? 'OK' : _esc(ctrl.errMsg || 'Error'))
                  + '</span>'
                : '<span class="dv-muted">N/A</span>';

            /* Error message when offline */
            var errCell = cam.status ? '' : '<div class="dv-errmsg" title="' + _esc(cam.errMsg || '') + '">'
                + _esc((cam.errMsg || '').substring(0, 60) + ((cam.errMsg || '').length > 60 ? '…' : ''))
                + '</div>';

            return '<tr>'
                + '<td class="dv-td-name"><div class="dv-name-wrap"><span class="dv-cam-name">' + _esc(name) + '</span>'
                + '<span class="dv-cam-id">' + _esc(cam.deviceId) + '</span></div></td>'
                + '<td><span class="dv-status-dot ' + statusCls + '"></span><span class="dv-status-lbl ' + statusCls + '">' + statusLbl + '</span>'
                + errCell + '</td>'
                + '<td class="dv-td-ip">' + _esc(ip) + '</td>'
                + '<td>' + _esc(mfr) + '</td>'
                + '<td>' + _esc(model) + '</td>'
                + '<td>' + priCell + '</td>'
                + '<td>' + secCell + '</td>'
                + '<td>' + ctrlCell + '</td>'
                + '</tr>';
        }).join('');

        el.innerHTML = rows;
    }

    /* ── inject CSS ── */
    function _injectCss() {
        if (document.getElementById('dv-style')) return;
        var style = document.createElement('style');
        style.id = 'dv-style';
        style.textContent = [
            '/* ── Devices Tab ── */',
            '.dv-header { display:flex; align-items:center; gap:14px; margin-bottom:18px; flex-wrap:wrap; }',
            '.dv-subtabs { display:flex; gap:2px; background:var(--c-elev); border:1px solid var(--c-border); border-radius:7px; padding:3px; }',
            '.dv-subtab { display:flex; align-items:center; gap:6px; padding:6px 14px; border-radius:5px; border:none; background:none; color:var(--c-muted); font-size:0.82rem; font-weight:600; cursor:pointer; transition:background 0.12s,color 0.12s; }',
            '.dv-subtab:hover { color:var(--c-text); background:rgba(255,255,255,0.05); }',
            '.dv-subtab.active { background:rgba(56,139,253,0.15); color:var(--c-accent); }',
            '.dv-subtab svg { width:15px; height:15px; }',
            '.dv-badge { display:inline-flex; align-items:center; justify-content:center; min-width:18px; height:18px; padding:0 5px; border-radius:9px; font-size:0.66rem; font-weight:700; background:var(--c-elev); border:1px solid var(--c-border); color:var(--c-muted); }',
            '.dv-reload-btn { display:flex; align-items:center; gap:6px; padding:7px 14px; border-radius:6px; background:var(--c-elev); border:1px solid var(--c-border); color:var(--c-text); font-size:0.82rem; font-weight:600; cursor:pointer; transition:background 0.12s,border-color 0.12s; margin-left:auto; }',
            '.dv-reload-btn:hover { background:var(--c-surf); border-color:var(--c-accent); }',
            '.dv-reload-btn svg { width:15px; height:15px; }',
            '.dv-reload-btn:disabled { opacity:0.5; cursor:default; }',
            '.dv-cam-summary { display:flex; gap:14px; font-size:0.8rem; margin-bottom:14px; flex-wrap:wrap; }',
            '.dv-sum-item { display:flex; align-items:center; gap:5px; color:var(--c-muted); }',
            '.dv-sum-item b { color:var(--c-text); }',
            '.dv-sum-on b  { color:var(--c-success); }',
            '.dv-sum-off b { color:var(--c-danger); }',
            '.dv-table-wrap { overflow-x:auto; }',
            '.dv-table { width:100%; border-collapse:collapse; font-size:0.82rem; background:var(--c-surf); border:1px solid var(--c-border); border-radius:8px; overflow:hidden; }',
            '.dv-th { padding:9px 12px; text-align:left; font-size:0.72rem; font-weight:700; color:var(--c-muted); text-transform:uppercase; letter-spacing:0.07em; border-bottom:1px solid var(--c-border); background:rgba(0,0,0,0.15); white-space:nowrap; }',
            '.dv-table tbody tr { border-bottom:1px solid var(--c-border); transition:background 0.1s; vertical-align:top; }',
            '.dv-table tbody tr:last-child { border-bottom:none; }',
            '.dv-table tbody tr:hover { background:rgba(255,255,255,0.025); }',
            '.dv-table td { padding:9px 12px; vertical-align:top; }',
            '.dv-td-name { min-width:160px; }',
            '.dv-td-ip { font-family:monospace; font-size:0.78rem; white-space:nowrap; }',
            '.dv-name-wrap { display:flex; flex-direction:column; gap:2px; }',
            '.dv-cam-name { font-weight:600; color:var(--c-text); }',
            '.dv-cam-id { font-size:0.68rem; font-family:monospace; color:var(--c-muted); word-break:break-all; }',
            '.dv-status-dot { display:inline-block; width:7px; height:7px; border-radius:50%; margin-right:5px; vertical-align:middle; }',
            '.dv-status-on { color:var(--c-success); }',
            '.dv-status-on.dv-status-dot { background:var(--c-success); box-shadow:0 0 0 2px rgba(63,185,80,0.25); }',
            '.dv-status-off { color:var(--c-danger); }',
            '.dv-status-off.dv-status-dot { background:var(--c-danger); }',
            '.dv-status-lbl { font-size:0.78rem; font-weight:600; vertical-align:middle; }',
            '.dv-errmsg { font-size:0.7rem; color:var(--c-muted); margin-top:3px; max-width:200px; word-break:break-word; line-height:1.4; }',
            '.dv-stream-cell { display:inline-flex; flex-direction:column; gap:1px; font-size:0.76rem; line-height:1.4; }',
            '.dv-stream-on { color:var(--c-success); }',
            '.dv-stream-off { color:var(--c-danger); opacity:0.7; }',
            '.dv-stream-bw { font-size:0.68rem; font-family:monospace; opacity:0.8; }',
            '.dv-ctrl-on { color:var(--c-success); font-size:0.76rem; }',
            '.dv-ctrl-off { color:var(--c-danger); font-size:0.7rem; word-break:break-word; max-width:100px; display:inline-block; }',
            '.dv-muted { color:var(--c-muted); font-size:0.78rem; }',
            '.dv-td-status { text-align:center; padding:40px; color:var(--c-muted); font-size:0.85rem; }',
            '.dv-td-err { color:var(--c-danger); display:flex; align-items:center; justify-content:center; gap:8px; }',
            '.dv-td-muted { color:var(--c-muted); }',
            '.dv-spinner { display:inline-block; width:16px; height:16px; border:2px solid var(--c-border); border-top-color:var(--c-accent); border-radius:50%; animation:spin 0.65s linear infinite; vertical-align:middle; margin-right:7px; }',
            '.dv-future-panel { display:flex; flex-direction:column; align-items:center; justify-content:center; gap:12px; padding:60px; color:var(--c-muted); font-size:0.85rem; }',
            '.dv-future-panel svg { width:44px; height:44px; opacity:0.4; }',
        ].join('\n');
        document.head.appendChild(style);
    }

    /* ── render shell ── */
    function _renderShell() {
        var container = document.getElementById('devices-content');
        if (!container) return;

        container.innerHTML =
            '<div class="dv-header">'
          +   '<div class="dv-subtabs">'
          +     '<button class="dv-subtab active" data-tab="camera">'
          +       '<svg viewBox="0 0 24 24" fill="currentColor"><path d="M17 10.5V7c0-.55-.45-1-1-1H4c-.55 0-1 .45-1 1v10c0 .55.45 1 1 1h12c.55 0 1-.45 1-1v-3.5l4 4v-11l-4 4z"/></svg>'
          +       'Camera <span class="dv-badge" id="dv-cam-badge">—</span>'
          +     '</button>'
          +   '</div>'
          +   '<button class="dv-reload-btn" id="dv-reload-btn">'
          +     '<svg viewBox="0 0 24 24" fill="currentColor"><path d="M17.65 6.35C16.2 4.9 14.21 4 12 4c-4.42 0-7.99 3.58-7.99 8s3.57 8 7.99 8c3.73 0 6.84-2.55 7.73-6h-2.08c-.82 2.33-3.04 4-5.65 4-3.31 0-6-2.69-6-6s2.69-6 6-6c1.66 0 3.14.69 4.22 1.78L13 11h7V4l-2.35 2.35z"/></svg>'
          +     'Reload'
          +   '</button>'
          + '</div>'

          /* ── Camera panel ── */
          + '<div class="dv-panel" data-panel="camera">'
          +   '<div class="dv-cam-summary" id="dv-cam-summary"></div>'
          +   '<div class="dv-table-wrap">'
          +     '<table class="dv-table">'
          +       '<thead><tr>'
          +         '<th class="dv-th">Tên / ID</th>'
          +         '<th class="dv-th">Trạng thái</th>'
          +         '<th class="dv-th">IP</th>'
          +         '<th class="dv-th">Hãng</th>'
          +         '<th class="dv-th">Model</th>'
          +         '<th class="dv-th">Stream chính</th>'
          +         '<th class="dv-th">Stream phụ</th>'
          +         '<th class="dv-th">Controller</th>'
          +       '</tr></thead>'
          +       '<tbody id="dv-cam-body">'
          +         '<tr><td colspan="8" class="dv-td-status"><span class="dv-spinner"></span> Đang tải…</td></tr>'
          +       '</tbody>'
          +     '</table>'
          +   '</div>'
          + '</div>';

        /* bind events */
        document.querySelectorAll('.dv-subtab').forEach(function (btn) {
            btn.addEventListener('click', function () { _switchSubTab(this.dataset.tab); });
        });
        document.getElementById('dv-reload-btn').addEventListener('click', function () {
            var btn = this;
            btn.disabled = true;
            btn.style.opacity = '0.5';
            _loadCameras();
            setTimeout(function () { btn.disabled = false; btn.style.opacity = ''; }, 1200);
        });
    }

    /* ── public entry point ── */
    window.initDevices = function () {
        _injectCss();
        _renderShell();
        _loadCameras();
    };
})();
