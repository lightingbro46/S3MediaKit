// =============================================================================
// config/config.js — Configuration editor tab
// =============================================================================

// Section display order (other sections appear alphabetically after these)
const CFG_SECTION_ORDER = ['api','http','general','protocol','hls','rtmp','rtc','rtp','rtp_proxy','ffmpeg','hook','cluster','record','multicast'];

// Human-readable descriptions for well-known keys
const CFG_DESCRIPTIONS = {
    'api.apiDebug':          'In chi tiết nội dung và phản hồi của từng HTTP request (1=bật, 0=tắt)',
    'api.secret':            '⚠ Secret key xác thực API. Thay đổi sẽ yêu cầu đăng nhập lại.',
    'api.snapRoot':          'Thư mục lưu ảnh snapshot',
    'api.defaultSnap':       'Ảnh mặc định khi snapshot chưa được tạo',
    'api.downloadRoot':      'Thư mục gốc cho API tải file',
    'http.port':             'Cổng HTTP server',
    'http.sslport':          'Cổng HTTPS server',
    'http.rootPath':         'Thư mục gốc của HTTP file server',
    'http.charSet':          'Encoding ký tự HTTP',
    'http.keepAliveSecond':  'Thời gian timeout kết nối HTTP (giây)',
    'http.allow_cross_domains': 'Cho phép tất cả CORS request (1=bật)',
    'http.allow_ip_range':   'Danh sách IP được phép truy cập API (trống = không giới hạn)',
    'http.dirMenu':          'Hiển thị danh sách thư mục (1=bật)',
    'general.mediaServerId': 'ID duy nhất của media server',
    'general.enableVhost':   'Kích hoạt virtual hosting (1=bật)',
    'general.maxStreamWaitMS': 'Thời gian tối đa chờ stream (ms)',
    'general.mergeWriteMS':  'Kích thước cache merge-write (ms). 0=tắt',
    'general.listen_ip':     'IP network card cần bind (mặc định :: = tất cả)',
    'protocol.enable_hls':   'Bật chuyển đổi sang HLS (mpegts)',
    'protocol.enable_hls_fmp4': 'Bật chuyển đổi sang HLS (fmp4)',
    'protocol.enable_mp4':   'Bật ghi MP4',
    'protocol.enable_rtsp':  'Bật RTSP/WebRTC',
    'protocol.enable_rtmp':  'Bật RTMP/FLV',
    'protocol.enable_fmp4':  'Bật HTTP-fMP4/WS-fMP4',
    'protocol.mp4_save_path': 'Đường dẫn lưu file MP4',
    'protocol.hls_save_path': 'Đường dẫn lưu file HLS',
    'protocol.mp4_max_second': 'Kích thước slice MP4 tối đa (giây)',
    'protocol.continue_push_ms': 'Thời gian chờ kết nối lại push stream (ms)',
    'hls.segDur':            'Thời lượng tối đa mỗi slice HLS (giây)',
    'hls.segNum':            'Số slice giữ trong m3u8 (0=lưu toàn bộ)',
    'hls.segRetain':         'Số slice giữ trên disk sau khi xoá khỏi m3u8',
    'hls.deleteDelaySec':    'Delay xoá file HLS live (giây)',
    'rtmp.port':             'Cổng RTMP server',
    'rtmp.sslport':          'Cổng RTMPS server',
    'rtmp.handshakeSecond':  'Timeout bắt tay RTMP (giây)',
    'rtmp.keepAliveSecond':  'Timeout kết nối RTMP không hoạt động (giây)',
    'rtc.port':              'Cổng UDP WebRTC',
    'rtc.tcpPort':           'Cổng TCP WebRTC',
    'rtc.signalingPort':     'Cổng Signaling WebRTC',
    'rtc.externIP':          'IP public của server (dùng cho NAT)',
    'rtc.enableTurn':        'Bật TURN service trên cổng STUN/TURN',
    'rtp_proxy.port':        'Cổng UDP/TCP proxy RTP',
    'rtp_proxy.port_range':  'Dải cổng ngẫu nhiên cho RTP (ít nhất 36 cổng)',
    'ffmpeg.bin':            'Đường dẫn thực thi FFmpeg (chỉ đọc)',
    'ffmpeg.cmd':            'Template lệnh FFmpeg pull/push stream',
    'ffmpeg.snap':           'Template lệnh FFmpeg chụp ảnh',
    'ffmpeg.log':            'Đường dẫn log FFmpeg (trống=không log)',
    'ffmpeg.restart_sec':    'Tự động restart FFmpeg sau N giây (0=tắt)',
    'hook.enable':           'Kích hoạt xác thực qua hook event',
    'hook.on_server_started': 'URL callback khi server khởi động',
    'hook.on_server_keepalive': 'URL heartbeat của server',
    'hook.alive_interval':   'Khoảng thời gian gửi keepalive (giây)',
    'hook.api_url':          'Base URL của API backend',
    'record.appName':        'App name cho on-demand MP4',
    'record.fileRepeat':     'Lặp lại file MP4 khi phát (1=bật)',
};

// Keys that are read-only (server does not allow setting them)
const CFG_READONLY = new Set(['ffmpeg.bin']);

// Sections that should be collapsed by default (less critical)
const CFG_COLLAPSED_DEFAULT = new Set(['multicast', 'cluster', 'rtp_proxy', 'hook']);

// ─── Helpers ──────────────────────────────────────────────────────────────────

function esc(s) {
    return String(s)
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;');
}

// ─── Main ─────────────────────────────────────────────────────────────────────

function initConfig() {
    const container = document.getElementById('config-content');
    if (!container) return;
    const auth = window.S3Auth.require();
    if (!auth) return;

    _loadConfig(container);
}

async function _loadConfig(container) {
    container.innerHTML = '<div class="cfg-loading"><span class="cfg-spinner"></span> Đang tải cấu hình…</div>';
    try {
        const data = await S3Auth.apiFetch('/index/api/getServerConfig');
        const raw  = (data.data && data.data[0]) ? data.data[0] : {};
        _renderConfig(container, raw);
    } catch (err) {
        container.innerHTML = `
            <div class="cfg-error-box">
                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round">
                    <circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/>
                    <circle cx="12" cy="16" r=".5" fill="currentColor" stroke="none"/>
                </svg>
                <span>${esc(err.message)}</span>
                <button class="cfg-btn-secondary" onclick="initConfig()">Thử lại</button>
            </div>`;
    }
}

function _renderConfig(container, config) {
    // Group by section (prefix before first '.')
    const sections = {};
    Object.keys(config).forEach(key => {
        const dot = key.indexOf('.');
        const sec = dot >= 0 ? key.substring(0, dot) : 'other';
        const name = dot >= 0 ? key.substring(dot + 1) : key;
        if (!sections[sec]) sections[sec] = [];
        sections[sec].push({ key, name, value: String(config[key]) });
    });

    // Sort sections: defined order first, then alphabetical
    const sortedSections = Object.keys(sections).sort((a, b) => {
        const ia = CFG_SECTION_ORDER.indexOf(a);
        const ib = CFG_SECTION_ORDER.indexOf(b);
        if (ia >= 0 && ib >= 0) return ia - ib;
        if (ia >= 0) return -1;
        if (ib >= 0) return 1;
        return a.localeCompare(b);
    });

    const sectionsHtml = sortedSections.map(sec => {
        const items   = sections[sec];
        const defCol  = CFG_COLLAPSED_DEFAULT.has(sec);
        const changed = items.filter(i => CFG_READONLY.has(i.key) === false).length;
        return `
        <div class="cfg-section${defCol ? ' collapsed' : ''}" id="cfg-sec-${esc(sec)}">
            <button class="cfg-section-hd" onclick="cfgToggleSection('${esc(sec)}')" type="button">
                <span class="cfg-sec-name">[${esc(sec)}]</span>
                <span class="cfg-sec-meta">${items.length} keys</span>
                <svg class="cfg-chevron" viewBox="0 0 24 24" fill="currentColor"><path d="M7 10l5 5 5-5z"/></svg>
            </button>
            <div class="cfg-section-body" id="cfg-body-${esc(sec)}">
                <div class="cfg-items">
                    ${items.map(item => _renderItem(item)).join('')}
                </div>
                <div class="cfg-section-footer">
                    <button class="cfg-btn-save" onclick="cfgSaveSection('${esc(sec)}')" type="button">
                        <svg viewBox="0 0 24 24" fill="currentColor" width="14" height="14"><path d="M17 3H5a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2V7l-4-4zm-5 16a3 3 0 1 1 0-6 3 3 0 0 1 0 6zm3-10H5V5h10v4z"/></svg>
                        Lưu [${esc(sec)}]
                    </button>
                </div>
            </div>
        </div>`;
    }).join('');

    container.innerHTML = `
        <div class="cfg-toolbar">
            <div class="cfg-search-wrap">
                <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" width="15" height="15"><circle cx="11" cy="11" r="8"/><line x1="21" y1="21" x2="16.65" y2="16.65"/></svg>
                <input class="cfg-search" type="text" placeholder="Tìm kiếm cấu hình…" oninput="cfgFilter(this.value)" id="cfg-search-input">
            </div>
            <div class="cfg-toolbar-actions">
                <button class="cfg-btn-secondary" onclick="initConfig()" type="button">
                    <svg viewBox="0 0 24 24" fill="currentColor" width="14" height="14"><path d="M17.65 6.35A7.958 7.958 0 0 0 12 4c-4.42 0-7.99 3.58-7.99 8s3.57 8 7.99 8c3.73 0 6.84-2.55 7.73-6h-2.08A5.99 5.99 0 0 1 12 18c-3.31 0-6-2.69-6-6s2.69-6 6-6c1.66 0 3.14.69 4.22 1.78L13 11h7V4l-2.35 2.35z"/></svg>
                    Tải lại
                </button>
                <button class="cfg-btn-restart" onclick="cfgShowRestartDialog()" type="button">
                    <svg viewBox="0 0 24 24" fill="currentColor" width="14" height="14"><path d="M13 3a9 9 0 1 0 7.5 13.92l-1.73-1a7 7 0 1 1-5.77-9.89V9l4-4-4-4v2.03z"/></svg>
                    Restart
                </button>
                <button class="cfg-btn-save-all" onclick="cfgSaveAll()" type="button">
                    <svg viewBox="0 0 24 24" fill="currentColor" width="14" height="14"><path d="M17 3H5a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2V7l-4-4zm-5 16a3 3 0 1 1 0-6 3 3 0 0 1 0 6zm3-10H5V5h10v4z"/></svg>
                    Lưu tất cả
                </button>
            </div>
        </div>
        <div class="cfg-sections-wrap" id="cfg-sections-wrap">
            ${sectionsHtml}
        </div>
        <div id="cfg-toast" class="cfg-toast" role="alert" aria-live="polite"></div>
    `;

    // Store original values
    window._cfgOriginal = Object.assign({}, config);
}

function _renderItem(item) {
    const isReadonly = CFG_READONLY.has(item.key);
    const isSecret   = item.key === 'api.secret';
    const desc       = CFG_DESCRIPTIONS[item.key] || '';
    const isLong     = item.value.length > 80 || item.value.includes('%s') || item.value.includes('\n');

    const inputEl = isLong
        ? `<textarea class="cfg-input${isReadonly ? ' cfg-input-ro' : ''}" rows="3"
                data-key="${esc(item.key)}"
                data-orig="${esc(item.value)}"
                ${isReadonly ? 'readonly' : ''}
                oninput="cfgMarkChanged(this)">${esc(item.value)}</textarea>`
        : `<input type="text" class="cfg-input${isReadonly ? ' cfg-input-ro' : ''}"
                data-key="${esc(item.key)}"
                data-orig="${esc(item.value)}"
                value="${esc(item.value)}"
                ${isReadonly ? 'readonly' : ''}
                oninput="cfgMarkChanged(this)">`;

    return `
    <div class="cfg-item" data-key="${esc(item.key)}" data-sec="${esc(item.key.split('.')[0])}">
        <div class="cfg-item-meta">
            <span class="cfg-key-name">${esc(item.name)}</span>
            ${isReadonly ? '<span class="cfg-badge-ro">read-only</span>' : ''}
            ${isSecret   ? '<span class="cfg-badge-warn" title="Thay đổi secret yêu cầu đăng nhập lại">⚠ sensitive</span>' : ''}
        </div>
        ${desc ? `<div class="cfg-desc">${esc(desc)}</div>` : ''}
        <div class="cfg-input-row">
            ${inputEl}
            <button class="cfg-reset-btn" title="Khôi phục giá trị gốc"
                    onclick="cfgResetItem(this)" type="button" style="display:none">↩</button>
        </div>
    </div>`;
}

// ─── Interactivity ────────────────────────────────────────────────────────────

function cfgToggleSection(sec) {
    const el = document.getElementById('cfg-sec-' + sec);
    if (el) el.classList.toggle('collapsed');
}

function cfgMarkChanged(input) {
    const orig = input.dataset.orig;
    const changed = input.value !== orig;
    input.classList.toggle('cfg-input-changed', changed);
    const resetBtn = input.parentElement.querySelector('.cfg-reset-btn');
    if (resetBtn) resetBtn.style.display = changed ? 'flex' : 'none';
}

function cfgResetItem(btn) {
    const row   = btn.parentElement;
    const input = row.querySelector('.cfg-input');
    if (!input) return;
    input.value = input.dataset.orig;
    if (input.tagName === 'TEXTAREA') input.textContent = input.dataset.orig;
    cfgMarkChanged(input);
}

function cfgFilter(query) {
    const q = query.trim().toLowerCase();
    document.querySelectorAll('.cfg-item').forEach(item => {
        const key  = (item.dataset.key  || '').toLowerCase();
        const desc = item.querySelector('.cfg-desc');
        const descText = desc ? desc.textContent.toLowerCase() : '';
        const match = !q || key.includes(q) || descText.includes(q);
        item.style.display = match ? '' : 'none';
    });
    // Hide/show empty sections
    document.querySelectorAll('.cfg-section').forEach(sec => {
        const visItems = sec.querySelectorAll('.cfg-item:not([style*="display: none"])');
        sec.style.display = (visItems.length === 0 && q) ? 'none' : '';
        if (q && visItems.length > 0) sec.classList.remove('collapsed');
    });
}

async function cfgSaveSection(sec) {
    const body = document.getElementById('cfg-body-' + sec);
    if (!body) return;

    const inputs  = body.querySelectorAll('.cfg-input:not([readonly])');
    const payload = {};
    inputs.forEach(inp => { payload[inp.dataset.key] = inp.value; });

    await _cfgSave(payload, '[' + sec + ']', () => {
        inputs.forEach(inp => {
            inp.dataset.orig = inp.value;
            inp.classList.remove('cfg-input-changed');
            const resetBtn = inp.parentElement.querySelector('.cfg-reset-btn');
            if (resetBtn) resetBtn.style.display = 'none';
        });
        // If secret changed, update session
        const secInput = body.querySelector('.cfg-input[data-key="api.secret"]');
        if (secInput) {
            const auth = S3Auth.get();
            if (auth) S3Auth.set(auth.serverUrl, secInput.value);
        }
    });
}

async function cfgSaveAll() {
    const inputs  = document.querySelectorAll('.cfg-input:not([readonly])');
    const payload = {};
    inputs.forEach(inp => { payload[inp.dataset.key] = inp.value; });

    await _cfgSave(payload, 'toàn bộ cấu hình', () => {
        inputs.forEach(inp => {
            inp.dataset.orig = inp.value;
            inp.classList.remove('cfg-input-changed');
            const resetBtn = inp.parentElement.querySelector('.cfg-reset-btn');
            if (resetBtn) resetBtn.style.display = 'none';
        });
        // Update secret in session if changed
        const secInput = document.querySelector('.cfg-input[data-key="api.secret"]');
        if (secInput) {
            const auth = S3Auth.get();
            if (auth) S3Auth.set(auth.serverUrl, secInput.value);
        }
    });
}

async function _cfgSave(payload, label, onSuccess) {
    if (Object.keys(payload).length === 0) {
        cfgToast('Không có gì thay đổi', 'info');
        return;
    }
    try {
        await S3Auth.apiPost('/index/api/setServerConfig', payload);
        if (onSuccess) onSuccess();
        cfgToast('Đã lưu ' + label, 'success');
    } catch (err) {
        cfgToast('Lỗi khi lưu: ' + err.message, 'error');
    }
}

let _toastTimer;
function cfgToast(msg, type) {
    const el = document.getElementById('cfg-toast');
    if (!el) return;
    el.textContent = msg;
    el.className   = 'cfg-toast cfg-toast-' + type + ' show';
    clearTimeout(_toastTimer);
    _toastTimer = setTimeout(() => el.classList.remove('show'), 3500);
}

// ── Restart dialog ────────────────────────────────────────────────────────────

function cfgShowRestartDialog() {
    if (document.getElementById('cfg-restart-overlay')) return;

    const overlay = document.createElement('div');
    overlay.id = 'cfg-restart-overlay';
    overlay.className = 'cfg-restart-overlay';
    overlay.innerHTML = `
        <div class="cfg-restart-dialog" role="dialog" aria-modal="true" aria-labelledby="cfg-restart-title">
            <div class="cfg-restart-hd">
                <svg viewBox="0 0 24 24" fill="currentColor" width="20" height="20"><path d="M13 3a9 9 0 1 0 7.5 13.92l-1.73-1a7 7 0 1 1-5.77-9.89V9l4-4-4-4v2.03z"/></svg>
                <span id="cfg-restart-title">Xác nhận khởi động lại</span>
            </div>
            <p class="cfg-restart-desc">Nhập <strong>secret key</strong> để xác nhận restart server.<br>Server sẽ tự động khởi động lại sau 1 giây.</p>
            <div class="cfg-restart-field">
                <label class="cfg-restart-lbl" for="cfg-restart-secret">Secret key</label>
                <input id="cfg-restart-secret" type="password" class="cfg-restart-input" placeholder="Nhập secret key…" autocomplete="current-password">
                <div id="cfg-restart-err" class="cfg-restart-err" hidden></div>
            </div>
            <div class="cfg-restart-footer">
                <button class="cfg-restart-cancel" onclick="cfgCloseRestartDialog()" type="button">Huỷ</button>
                <button class="cfg-restart-confirm" onclick="cfgDoRestart()" type="button">
                    <svg viewBox="0 0 24 24" fill="currentColor" width="14" height="14"><path d="M13 3a9 9 0 1 0 7.5 13.92l-1.73-1a7 7 0 1 1-5.77-9.89V9l4-4-4-4v2.03z"/></svg>
                    Restart
                </button>
            </div>
        </div>`;
    document.body.appendChild(overlay);

    // close on backdrop click
    overlay.addEventListener('click', function (e) {
        if (e.target === overlay) cfgCloseRestartDialog();
    });

    // submit on Enter
    overlay.querySelector('#cfg-restart-secret').addEventListener('keydown', function (e) {
        if (e.key === 'Enter') cfgDoRestart();
        if (e.key === 'Escape') cfgCloseRestartDialog();
    });

    setTimeout(function () { overlay.querySelector('#cfg-restart-secret').focus(); }, 60);
}

function cfgCloseRestartDialog() {
    const el = document.getElementById('cfg-restart-overlay');
    if (el) el.remove();
}

async function cfgDoRestart() {
    const input = document.getElementById('cfg-restart-secret');
    const errEl = document.getElementById('cfg-restart-err');
    if (!input) return;

    const secret = input.value.trim();
    if (!secret) {
        errEl.textContent = 'Vui lòng nhập secret key.';
        errEl.hidden = false;
        input.focus();
        return;
    }

    // verify against stored secret
    const auth = window.S3Auth ? S3Auth.get() : null;
    if (auth && auth.secret && secret !== auth.secret) {
        errEl.textContent = 'Secret key không đúng. Vui lòng thử lại.';
        errEl.hidden = false;
        input.value = '';
        input.focus();
        return;
    }

    const confirmBtn = document.querySelector('.cfg-restart-confirm');
    if (confirmBtn) { confirmBtn.disabled = true; confirmBtn.textContent = 'Đang gửi…'; }

    try {
        const a = auth || { serverUrl: '', secret: '' };
        const url = a.serverUrl + '/index/api/restartServer?secret=' + encodeURIComponent(secret);
        const resp = await fetch(url);
        const data = await resp.json();
        if (data.code === 0) {
            cfgCloseRestartDialog();
            cfgToast('Server đang khởi động lại…', 'info');
        } else {
            errEl.textContent = data.msg || ('Lỗi: code=' + data.code);
            errEl.hidden = false;
            if (confirmBtn) { confirmBtn.disabled = false; confirmBtn.innerHTML = '<svg viewBox="0 0 24 24" fill="currentColor" width="14" height="14"><path d="M13 3a9 9 0 1 0 7.5 13.92l-1.73-1a7 7 0 1 1-5.77-9.89V9l4-4-4-4v2.03z"/></svg> Restart'; }
        }
    } catch (err) {
        errEl.textContent = 'Lỗi kết nối: ' + err.message;
        errEl.hidden = false;
        if (confirmBtn) { confirmBtn.disabled = false; confirmBtn.innerHTML = '<svg viewBox="0 0 24 24" fill="currentColor" width="14" height="14"><path d="M13 3a9 9 0 1 0 7.5 13.92l-1.73-1a7 7 0 1 1-5.77-9.89V9l4-4-4-4v2.03z"/></svg> Restart'; }
    }
}
