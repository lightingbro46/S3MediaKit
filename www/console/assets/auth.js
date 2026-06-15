/**
 * S3MediaKit Console — Authentication utilities
 * Session data stored in sessionStorage: { serverUrl, secret }
 * Server URL remembered in localStorage across sessions.
 */
(function (global) {
    const AUTH_KEY  = 's3mk_auth';
    const SRV_KEY   = 's3mk_server';

    /**
     * Detect the console base path from the current URL.
     * Works for /console/ or any prefix like /media1/console/
     */
    function _consoleBase() {
        var p = location.pathname;
        var idx = p.indexOf('/console');
        if (idx !== -1) return p.substring(0, idx) + '/console/';
        return '/console/';
    }

    /** Expose base for use by other pages (login.html, index.html) */
    window.consoleBase = _consoleBase;

    const Auth = {
        /** Return current auth { serverUrl, secret } or null */
        get() {
            try { return JSON.parse(sessionStorage.getItem(AUTH_KEY)) || null; }
            catch (_) { return null; }
        },

        /** Save auth to session */
        set(serverUrl, secret) {
            const url = serverUrl.replace(/\/+$/, '');
            sessionStorage.setItem(AUTH_KEY, JSON.stringify({ serverUrl: url, secret }));
        },

        /** Clear session */
        clear() { sessionStorage.removeItem(AUTH_KEY); },

        /** Redirect to login if no session; return auth or null */
        require() {
            const auth = this.get();
            if (!auth || !auth.secret) {
                window.location.replace(_consoleBase() + 'login.html');
                return null;
            }
            return auth;
        },

        /** Persist server URL in localStorage */
        rememberServer(url) { localStorage.setItem(SRV_KEY, url); },
        getRememberedServer() { return localStorage.getItem(SRV_KEY) || ''; },

        /**
         * Verify credentials by calling getServerConfig.
         * Saves session on success, throws on failure.
         */
        async login(serverUrl, secret) {
            const base = serverUrl.replace(/\/+$/, '');
            const apiUrl = `${base}/index/api/getServerConfig?secret=${encodeURIComponent(secret)}`;
            let resp;
            try {
                resp = await fetch(apiUrl, { credentials: 'omit' });
            } catch (e) {
                throw new Error('Không kết nối được tới server: ' + e.message);
            }
            if (!resp.ok) throw new Error(`HTTP ${resp.status} — kiểm tra địa chỉ server`);
            let data;
            try { data = await resp.json(); } catch (_) { throw new Error('Phản hồi không hợp lệ từ server'); }
            if (data.code !== 0) throw new Error('Secret key không đúng');
            this.set(base, secret);
            return data;
        },

        /**
         * Make an authenticated GET API call.
         * @param {string} path  e.g. '/index/api/getServerConfig'
         * @param {Object} [params] extra query params
         */
        async apiFetch(path, params) {
            const auth = this.get();
            if (!auth) throw new Error('Not authenticated');
            const url = new URL(auth.serverUrl + path);
            url.searchParams.set('secret', auth.secret);
            if (params) {
                Object.keys(params).forEach(k => url.searchParams.set(k, String(params[k])));
            }
            let resp;
            try { resp = await fetch(url.toString(), { credentials: 'omit' }); }
            catch (e) { throw new Error('Network error: ' + e.message); }
            if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
            const data = await resp.json();
            if (data.code !== 0) throw new Error(data.msg || `API error code ${data.code}`);
            return data;
        },

        /**
         * Make an authenticated POST API call (config values in JSON body).
         * Secret is appended as a query param so it is definitely parsed.
         * @param {string} path  e.g. '/index/api/setServerConfig'
         * @param {Object} body  config key→value map
         */
        async apiPost(path, body) {
            const auth = this.get();
            if (!auth) throw new Error('Not authenticated');
            const url = `${auth.serverUrl}${path}?secret=${encodeURIComponent(auth.secret)}`;
            let resp;
            try {
                resp = await fetch(url, {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify(body),
                    credentials: 'omit',
                });
            } catch (e) { throw new Error('Network error: ' + e.message); }
            if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
            const data = await resp.json();
            if (data.code !== 0) throw new Error(data.msg || `API error code ${data.code}`);
            return data;
        },

        /** Return full API URL string for a given path (no secret appended) */
        apiBase() {
            const auth = this.get();
            return auth ? auth.serverUrl : '';
        },
    };

    global.S3Auth = Auth;
})(window);
