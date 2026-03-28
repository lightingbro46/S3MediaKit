// =============================================================================
// tabs/mjpeg/mjpeg.js — MJPEG / Image debug tab
// =============================================================================

function initMjpeg() {
    const urlEl   = document.getElementById('mjpeg-url');
    const frameEl = document.getElementById('mjpeg-frame');
    const loadBtn = document.getElementById('mjpeg-load');
    const stopBtn = document.getElementById('mjpeg-stop');
    const logEl   = document.getElementById('mjpeg-log');
    const BLANK   = 'data:image/gif;base64,R0lGODlhAQABAAD/ACwAAAAAAQABAAACADs=';

    function mlog(cls, msg) {
        if (!logEl) return;
        const d = document.createElement('div');
        d.className = cls;
        const ts = new Date().toLocaleTimeString();
        d.textContent = '[' + ts + '] ' + msg;
        logEl.appendChild(d);
        logEl.scrollTop = logEl.scrollHeight;
    }

    function load() {
        const url = urlEl.value.trim();
        if (!url) { mlog('ml-warn', 'URL trống.'); return; }
        mlog('ml-info', 'Connecting → ' + url);
        frameEl.src = url;
        frameEl.classList.remove('no-src');
    }

    function stop() {
        mlog('ml-info', 'Stopped.');
        frameEl.src = BLANK;
        frameEl.classList.add('no-src');
    }

    frameEl.addEventListener('load', function () {
        if (frameEl.src === BLANK || frameEl.classList.contains('no-src')) return;
        mlog('ml-ok', 'Frame loaded OK — ' + frameEl.naturalWidth + 'x' + frameEl.naturalHeight);
    });

    frameEl.addEventListener('error', function () {
        if (frameEl.src === BLANK) return;
        mlog('ml-error', 'Load error / connection failed: ' + frameEl.src);
        frameEl.classList.add('no-src');
    });

    loadBtn.addEventListener('click', load);
    stopBtn.addEventListener('click', stop);
    urlEl.addEventListener('keydown', function (e) { if (e.key === 'Enter') load(); });
}
