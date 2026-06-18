/* ════════════════════════════════════════════
   Files Browser Tab
   ════════════════════════════════════════════ */
(function () {
    'use strict';

    /* ── state ── */
    var _currentPath = '';        // relative path inside www root
    var _pathStack   = [];        // breadcrumb stack [{name, path}]
    var _sortCol     = 'name';    // 'name' | 'size' | 'mtime'
    var _sortAsc     = true;

    /* ── helpers ── */
    function _auth() { return window.S3Auth ? S3Auth.get() : { serverUrl: '', secret: '' }; }

    function _api(endpoint, params, cb) {
        var a = _auth();
        var url = a.serverUrl + endpoint + '?secret=' + encodeURIComponent(a.secret);
        for (var k in params) {
            if (Object.prototype.hasOwnProperty.call(params, k)) {
                url += '&' + encodeURIComponent(k) + '=' + encodeURIComponent(params[k]);
            }
        }
        fetch(url)
            .then(function (r) { return r.json(); })
            .then(function (d) { cb(null, d); })
            .catch(function (e) { cb(e, null); });
    }

    /* ── format helpers ── */
    function _fmtSize(bytes) {
        if (bytes < 0) return '—';
        if (bytes === 0) return '0 B';
        var units = ['B', 'KB', 'MB', 'GB', 'TB'];
        var i = Math.floor(Math.log(bytes) / Math.log(1024));
        i = Math.min(i, units.length - 1);
        return (bytes / Math.pow(1024, i)).toFixed(i > 0 ? 1 : 0) + ' ' + units[i];
    }

    function _fmtTime(epoch) {
        if (!epoch) return '—';
        var d = new Date(epoch * 1000);
        return d.toLocaleDateString() + ' ' + d.toLocaleTimeString();
    }

    /* ── render ── */
    function _render(data) {
        var container = document.getElementById('files-content');
        if (!container) return;

        var entries = (data && data.data && data.data.entries) ? data.data.entries : [];
        var root    = (data && data.data && data.data.root)    ? data.data.root    : '';
        var curPath = (data && data.data) ? (data.data.path || '') : '';

        /* Sort */
        entries = entries.slice().sort(function (a, b) {
            // dirs first
            if (a.isDir !== b.isDir) return a.isDir ? -1 : 1;
            var va, vb;
            if (_sortCol === 'size')  { va = a.size  || 0; vb = b.size  || 0; }
            else if (_sortCol === 'mtime') { va = a.mtime || 0; vb = b.mtime || 0; }
            else { va = (a.name || '').toLowerCase(); vb = (b.name || '').toLowerCase(); }
            if (va < vb) return _sortAsc ? -1 :  1;
            if (va > vb) return _sortAsc ?  1 : -1;
            return 0;
        });

        /* Breadcrumb */
        var crumbs = [{ name: 'www', path: '' }];
        if (curPath) {
            var parts = curPath.split('/').filter(Boolean);
            var acc = '';
            parts.forEach(function (p) {
                acc = acc ? acc + '/' + p : p;
                crumbs.push({ name: p, path: acc });
            });
        }

        var crumbHtml = crumbs.map(function (c, i) {
            if (i === crumbs.length - 1) {
                return '<span class="fb-crumb fb-crumb-cur">' + _esc(c.name) + '</span>';
            }
            return '<button class="fb-crumb fb-crumb-link" data-path="' + _esc(c.path) + '">' + _esc(c.name) + '</button>'
                 + '<span class="fb-crumb-sep">/</span>';
        }).join('');

        /* Sort arrow helper */
        function _arrow(col) {
            if (_sortCol !== col) return '<span class="fb-sort-arrow fb-sort-off">↕</span>';
            return '<span class="fb-sort-arrow">' + (_sortAsc ? '↑' : '↓') + '</span>';
        }

        /* Table rows */
        var rowsHtml = '';
        entries.forEach(function (e) {
            var icon = e.isDir
                ? '<svg class="fb-icon fb-icon-dir" viewBox="0 0 24 24" fill="currentColor"><path d="M10 4H4c-1.1 0-2 .9-2 2v12c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V8c0-1.1-.9-2-2-2h-8l-2-2z"/></svg>'
                : '<svg class="fb-icon fb-icon-file" viewBox="0 0 24 24" fill="currentColor"><path d="M14 2H6c-1.1 0-2 .9-2 2v16c0 1.1.9 2 2 2h12c1.1 0 2-.9 2-2V8l-6-6zm2 16H8v-2h8v2zm0-4H8v-2h8v2zm-3-5V3.5L18.5 9H13z"/></svg>';

            var nameCell;
            if (e.isDir) {
                nameCell = '<button class="fb-name-btn fb-dir-btn" data-path="' + _esc(e.path) + '">'
                         + icon + _esc(e.name) + '</button>';
            } else {
                nameCell = '<span class="fb-name-file">' + icon + _esc(e.name) + '</span>';
            }

            var dlBtn = e.isDir ? '' :
                '<button class="fb-dl-btn" data-path="' + _esc(e.path) + '" data-name="' + _esc(e.name) + '" title="Tải về">'
              + '<svg viewBox="0 0 24 24" fill="currentColor"><path d="M19 9h-4V3H9v6H5l7 7 7-7zM5 18v2h14v-2H5z"/></svg>'
              + '</button>';

            rowsHtml += '<tr>'
                + '<td class="fb-td-name">' + nameCell + '</td>'
                + '<td class="fb-td-size">' + (e.isDir ? '—' : _fmtSize(e.size)) + '</td>'
                + '<td class="fb-td-time">' + _fmtTime(e.mtime) + '</td>'
                + '<td class="fb-td-act">' + dlBtn + '</td>'
                + '</tr>';
        });

        if (!rowsHtml) {
            rowsHtml = '<tr><td colspan="4" class="fb-empty">Thư mục trống</td></tr>';
        }

        container.innerHTML =
            '<div class="fb-toolbar">'
          +   '<div class="fb-breadcrumb">' + crumbHtml + '</div>'
          +   '<div class="fb-toolbar-right">'
          +     '<span class="fb-root-badge" title="' + _esc(root) + '">' + _esc(root) + '</span>'
          +     '<button class="fb-refresh-btn" title="Làm mới">'
          +       '<svg viewBox="0 0 24 24" fill="currentColor"><path d="M17.65 6.35C16.2 4.9 14.21 4 12 4c-4.42 0-7.99 3.58-7.99 8s3.57 8 7.99 8c3.73 0 6.84-2.55 7.73-6h-2.08c-.82 2.33-3.04 4-5.65 4-3.31 0-6-2.69-6-6s2.69-6 6-6c1.66 0 3.14.69 4.22 1.78L13 11h7V4l-2.35 2.35z"/></svg>'
          +       ' Làm mới'
          +     '</button>'
          +   '</div>'
          + '</div>'
          + '<div class="fb-table-wrap">'
          +   '<table class="fb-table">'
          +     '<thead><tr>'
          +       '<th class="fb-th fb-th-name fb-sortable" data-col="name">Tên ' + _arrow('name') + '</th>'
          +       '<th class="fb-th fb-th-size fb-sortable" data-col="size">Kích thước ' + _arrow('size') + '</th>'
          +       '<th class="fb-th fb-th-time fb-sortable" data-col="mtime">Chỉnh sửa ' + _arrow('mtime') + '</th>'
          +       '<th class="fb-th fb-th-act"></th>'
          +     '</tr></thead>'
          +     '<tbody>' + rowsHtml + '</tbody>'
          +   '</table>'
          + '</div>';

        /* bind events */
        container.querySelectorAll('.fb-crumb-link').forEach(function (btn) {
            btn.addEventListener('click', function () { _navigate(this.dataset.path); });
        });
        container.querySelectorAll('.fb-dir-btn').forEach(function (btn) {
            btn.addEventListener('click', function () { _navigate(this.dataset.path); });
        });
        container.querySelectorAll('.fb-dl-btn').forEach(function (btn) {
            btn.addEventListener('click', function () { _download(this.dataset.path, this.dataset.name); });
        });
        container.querySelectorAll('.fb-sortable').forEach(function (th) {
            th.addEventListener('click', function () {
                var col = this.dataset.col;
                if (_sortCol === col) { _sortAsc = !_sortAsc; }
                else { _sortCol = col; _sortAsc = true; }
                _load(_currentPath);
            });
        });
        container.querySelector('.fb-refresh-btn').addEventListener('click', function () {
            _load(_currentPath);
        });
    }

    function _showLoading() {
        var c = document.getElementById('files-content');
        if (c) c.innerHTML = '<div class="fb-loading"><span class="fb-spinner"></span> Đang tải…</div>';
    }

    function _showError(msg) {
        var c = document.getElementById('files-content');
        if (c) c.innerHTML = '<div class="fb-error">'
            + '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><circle cx="12" cy="16" r="0.5" fill="currentColor" stroke="none"/></svg>'
            + '<span>' + _esc(msg) + '</span></div>';
    }

    function _navigate(path) {
        _currentPath = path || '';
        _load(_currentPath);
    }

    function _load(path) {
        _showLoading();
        _api('/media/api/listFiles', { path: path }, function (err, data) {
            if (err || !data) { _showError('Lỗi kết nối tới server'); return; }
            if (data.code !== 0) { _showError(data.msg || 'Server trả về lỗi: ' + data.code); return; }
            _render(data);
        });
    }

    function _download(relPath, name) {
        var a = _auth();
        var url = a.serverUrl + '/media/api/serveFile'
            + '?secret=' + encodeURIComponent(a.secret)
            + '&path='   + encodeURIComponent(relPath)
            + '&save_name=' + encodeURIComponent(name);
        var link = document.createElement('a');
        link.href = url;
        link.download = name;
        document.body.appendChild(link);
        link.click();
        document.body.removeChild(link);
    }

    function _esc(s) {
        return String(s)
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;')
            .replace(/"/g, '&quot;');
    }

    /* ── inject CSS ── */
    function _injectCss() {
        if (document.getElementById('fb-style')) return;
        var style = document.createElement('style');
        style.id = 'fb-style';
        style.textContent = [
            '/* ── Files Browser ── */',
            '.fb-toolbar { display:flex; align-items:center; gap:10px; margin-bottom:14px; flex-wrap:wrap; }',
            '.fb-breadcrumb { display:flex; align-items:center; flex-wrap:wrap; gap:2px; flex:1; min-width:0; }',
            '.fb-crumb { background:none; border:none; cursor:pointer; font-size:0.82rem; color:var(--c-accent); padding:2px 5px; border-radius:4px; }',
            '.fb-crumb:hover { background:rgba(56,139,253,0.12); }',
            '.fb-crumb-cur { font-size:0.82rem; font-weight:700; color:var(--c-text); padding:2px 5px; }',
            '.fb-crumb-sep { font-size:0.82rem; color:var(--c-muted); padding:0 1px; }',
            '.fb-toolbar-right { display:flex; align-items:center; gap:8px; flex-shrink:0; }',
            '.fb-root-badge { font-size:0.68rem; font-family:monospace; color:var(--c-muted); background:var(--c-elev); border:1px solid var(--c-border); border-radius:4px; padding:2px 8px; max-width:220px; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }',
            '.fb-refresh-btn { display:flex; align-items:center; gap:5px; padding:6px 12px; border-radius:6px; background:var(--c-elev); border:1px solid var(--c-border); color:var(--c-text); font-size:0.8rem; font-weight:600; cursor:pointer; transition:background 0.12s,border-color 0.12s; }',
            '.fb-refresh-btn svg { width:14px; height:14px; }',
            '.fb-refresh-btn:hover { background:var(--c-surf); border-color:var(--c-accent); }',
            '.fb-table-wrap { overflow-x:auto; }',
            '.fb-table { width:100%; border-collapse:collapse; font-size:0.84rem; background:var(--c-surf); border:1px solid var(--c-border); border-radius:8px; overflow:hidden; }',
            '.fb-th { padding:10px 14px; text-align:left; font-size:0.74rem; font-weight:600; color:var(--c-muted); text-transform:uppercase; letter-spacing:0.07em; border-bottom:1px solid var(--c-border); background:rgba(0,0,0,0.15); white-space:nowrap; }',
            '.fb-sortable { cursor:pointer; user-select:none; }',
            '.fb-sortable:hover { color:var(--c-text); }',
            '.fb-sort-arrow { margin-left:4px; opacity:0.9; }',
            '.fb-sort-off { opacity:0.3; }',
            '.fb-th-size { width:110px; }',
            '.fb-th-time { width:170px; }',
            '.fb-th-act  { width:56px; }',
            '.fb-table tbody tr { border-bottom:1px solid var(--c-border); transition:background 0.1s; }',
            '.fb-table tbody tr:last-child { border-bottom:none; }',
            '.fb-table tbody tr:hover { background:rgba(255,255,255,0.03); }',
            '.fb-td-name { padding:8px 14px; }',
            '.fb-td-size,.fb-td-time { padding:8px 14px; font-family:monospace; font-size:0.8rem; color:var(--c-muted); white-space:nowrap; }',
            '.fb-td-act  { padding:8px 10px; text-align:center; }',
            '.fb-icon { width:16px; height:16px; vertical-align:-3px; margin-right:6px; flex-shrink:0; }',
            '.fb-icon-dir  { color:#e8a87c; }',
            '.fb-icon-file { color:var(--c-muted); }',
            '.fb-name-btn { background:none; border:none; cursor:pointer; color:var(--c-accent); font-size:0.84rem; padding:0; display:flex; align-items:center; gap:0; }',
            '.fb-name-btn:hover { text-decoration:underline; }',
            '.fb-name-file { display:flex; align-items:center; color:var(--c-text); font-size:0.84rem; }',
            '.fb-dl-btn { display:flex; align-items:center; justify-content:center; width:30px; height:30px; border-radius:5px; background:none; border:1px solid var(--c-border); color:var(--c-muted); cursor:pointer; transition:background 0.12s,color 0.12s,border-color 0.12s; }',
            '.fb-dl-btn svg { width:16px; height:16px; }',
            '.fb-dl-btn:hover { background:rgba(56,139,253,0.12); border-color:var(--c-accent); color:var(--c-accent); }',
            '.fb-empty { padding:40px; text-align:center; color:var(--c-muted); font-size:0.84rem; }',
            '.fb-loading { display:flex; align-items:center; gap:10px; padding:48px; color:var(--c-muted); font-size:0.85rem; }',
            '.fb-spinner { display:inline-block; width:18px; height:18px; border:2px solid var(--c-border); border-top-color:var(--c-accent); border-radius:50%; animation:spin 0.65s linear infinite; flex-shrink:0; }',
            '.fb-error { display:flex; flex-direction:column; align-items:center; gap:12px; padding:48px; color:var(--c-danger); font-size:0.85rem; text-align:center; }',
            '.fb-error svg { width:40px; height:40px; }',
        ].join('\n');
        document.head.appendChild(style);
    }

    /* ── public entry point ── */
    window.initFiles = function () {
        _injectCss();
        _currentPath = '';
        _sortCol = 'name';
        _sortAsc = true;
        _load('');
    };
})();
