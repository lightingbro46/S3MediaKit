// =============================================================================
// cluster/cluster.js — Cluster Node Monitoring Tab
// =============================================================================

var _clusterState = {
    nodes:       [],
    nodeStats:   {},
    pollTimer:   null,
    initialized: false,
};

function initCluster() {
    var auth = window.S3Auth && S3Auth.get();
    if (!auth) return;
    if (!_clusterState.initialized) {
        _clusterState.initialized = true;
        _buildClusterLayout();
        _injectClusterCSS();
    }
    _loadNodes();
}

function _buildClusterLayout() {
    var el = document.getElementById('cluster-content');
    if (!el) return;
    el.innerHTML =
        '<div class="cl-toolbar">' +
            '<div class="cl-title-block">' +
                '<svg class="cl-icon" viewBox="0 0 24 24" fill="currentColor"><path d="M17 12h-5v5h5v-5zM16 1v2H8V1H6v2H5c-1.11 0-1.99.9-1.99 2L3 19c0 1.1.89 2 2 2h14c1.1 0 2-.9 2-2V5c0-1.1-.9-2-2-2h-1V1h-2zm3 18H5V8h14v11z"/></svg>' +
                '<span id="cl-node-count">—</span> nodes trong cluster' +
            '</div>' +
            '<div class="cl-toolbar-right">' +
                '<span class="cl-ts" id="cl-last-update">—</span>' +
                '<button class="cl-btn" onclick="initCluster()">&#8635; Làm mới</button>' +
                '<button class="cl-btn primary" onclick="_clusterAddNodeDialog()">+ Thêm node</button>' +
            '</div>' +
        '</div>' +
        '<div class="cl-summary" id="cl-summary"></div>' +
        '<div class="cl-grid" id="cl-grid"><div class="cl-loading"><span class="cl-spinner"></span> Đang tải…</div></div>' +
        '<div class="cl-section-title" style="margin-top:24px">Bảng trạng thái tổng hợp</div>' +
        '<div id="cl-db-sync-wrap"><p class="cl-note">Đang tải…</p></div>' +
        '<div class="cl-section-title" style="margin-top:24px">Đồng bộ Database (transaction_sequence / ack_log / log count)</div>' +
        '<div id="cl-txn-wrap"><p class="cl-note">Đang tải dữ liệu đồng bộ…</p></div>' +
        '<div class="cl-dialog-backdrop" id="cl-dialog-backdrop" onclick="_closeNodeDialog()"></div>' +
        '<div class="cl-dialog" id="cl-dialog">' +
            '<div class="cl-dialog-hd">Thêm / Sửa Node</div>' +
            '<div class="cl-form">' +
                '<label>Node ID</label><input id="cl-d-id" class="cl-input" type="text" placeholder="node-01">' +
                '<label>Tên hiển thị</label><input id="cl-d-name" class="cl-input" type="text" placeholder="Node 01">' +
                '<label>IP / Domain</label><input id="cl-d-ip" class="cl-input" type="text" placeholder="192.168.1.100">' +
                '<label>HTTP Port</label><input id="cl-d-port" class="cl-input" type="number" value="80">' +
            '</div>' +
            '<div class="cl-dialog-footer">' +
                '<button class="cl-btn" onclick="_closeNodeDialog()">Huỷ</button>' +
                '<button class="cl-btn primary" onclick="_saveNodeFromDialog()">Lưu</button>' +
            '</div>' +
        '</div>';
}

async function _loadNodes() {
    var auth = S3Auth.get();
    if (!auth) return;

    var cfg = window._s3Config;
    if (!cfg) {
        try {
            var r = await S3Auth.apiFetch('/index/api/getServerConfig');
            cfg = (r.data && r.data[0]) ? r.data[0] : {};
            window._s3Config = cfg;
        } catch (e) {
            _setClusterError('Không lấy được cấu hình: ' + e.message);
            return;
        }
    }

    var peerListJson = cfg['peer.peer_list'] || '[]';
    var peerList = [];
    try { peerList = JSON.parse(peerListJson); } catch(_) {}

    var selfParsed = _parseUrl(auth.serverUrl);
    var selfId = cfg['general.mediaServerId'] || cfg['mediaServerId'] || 'self';
    // Look up self name from peer_list (each node registers itself in the list)
    var selfEntry = peerList.find(function(p) { return p.id === selfId; });
    var selfName = (selfEntry && selfEntry.name) ? selfEntry.name : selfId;

    var nodes = [{ id: selfId, name: selfName, ip: selfParsed.hostname, httpPort: selfParsed.port, isSelf: true }];
    peerList.forEach(function(p) {
        if (p.id && p.id !== selfId) {
            nodes.push({ id: p.id, name: p.name || p.id, ip: p.ip || p.domain || '', httpPort: p.http_port || p.httpPort || 80, isSelf: false });
        }
    });

    _clusterState.nodes = nodes;
    _renderNodeSkeleton(nodes);
    _pollAllNodes(nodes, auth.secret);

    if (_clusterState.pollTimer) clearInterval(_clusterState.pollTimer);
    _clusterState.pollTimer = setInterval(function() { _pollAllNodes(_clusterState.nodes, auth.secret); }, 5000);
}

function _renderNodeSkeleton(nodes) {
    var grid = document.getElementById('cl-grid');
    if (!grid) return;
    var cntEl = document.getElementById('cl-node-count');
    if (cntEl) cntEl.textContent = nodes.length;

    if (!nodes.length) {
        grid.innerHTML = '<div class="cl-empty"><p>Không có node nào. Cấu hình <code>peer.peer_list</code> trong config.ini</p></div>';
        return;
    }
    grid.innerHTML = nodes.map(function(n) {
        return '<div class="cl-node-card" id="cl-node-' + n.id + '">' +
            '<div class="cl-node-header">' +
                '<div class="cl-node-status-dot checking" id="cl-dot-' + n.id + '"></div>' +
                '<div class="cl-node-info">' +
                    '<div class="cl-node-name">' + _esc(n.name) + (n.isSelf ? ' <span class="cl-self-badge">SELF</span>' : '') + '</div>' +
                    '<div class="cl-node-addr" id="cl-addr-' + n.id + '">' + _esc(n.ip) + ':' + n.httpPort + '</div>' +
                '</div>' +
                '<span class="cl-node-status-lbl checking" id="cl-status-' + n.id + '">Checking…</span>' +
            '</div>' +
            '<div class="cl-node-metrics" id="cl-metrics-' + n.id + '">' +
                '<div class="cl-metric"><span class="cl-m-lbl">ID</span><span class="cl-m-val mono" id="cl-id-' + n.id + '">' + _esc(n.id.substring(0,12)) + '</span></div>' +
                '<div class="cl-metric"><span class="cl-m-lbl">Streams</span><span class="cl-m-val" id="cl-streams-' + n.id + '">—</span></div>' +
                '<div class="cl-metric"><span class="cl-m-lbl">Sessions</span><span class="cl-m-val" id="cl-sess-' + n.id + '">—</span></div>' +
                '<div class="cl-metric"><span class="cl-m-lbl">EP Avg Load</span><span class="cl-m-val" id="cl-ep-' + n.id + '">—</span></div>' +
            '</div>' +
            '<div class="cl-node-poller" id="cl-poller-' + n.id + '"></div>' +
            '<div class="cl-node-footer">' +
                '<span class="cl-m-lbl" id="cl-version-' + n.id + '">—</span>' +
                '<span class="cl-m-lbl" id="cl-latency-' + n.id + '">—</span>' +
            '</div>' +
        '</div>';
    }).join('');
}

async function _pollAllNodes(nodes, secret) {
    var results = await Promise.allSettled(nodes.map(function(n) { return _pollNode(n, secret); }));
    var online = 0; var offline = 0;
    results.forEach(function(r, i) {
        if (r.status === 'fulfilled') { online++; _applyNodeData(nodes[i], r.value); }
        else { offline++; _applyNodeOffline(nodes[i], r.reason); }
    });
    _updateSummary(online, offline);
    _updateSyncTable(nodes);
    var tsEl = document.getElementById('cl-last-update');
    if (tsEl) tsEl.textContent = 'Cập nhật: ' + new Date().toLocaleTimeString();
}

async function _pollNode(node, secret) {
    var base = _buildBase(node);
    var t0   = Date.now();

    var hc = await _fetchJson(base + '/media/mserver/healthcheck');
    var result = { online: true, latency: Date.now() - t0, mediaServerId: hc && hc.data ? hc.data.mediaServerId : node.id };

    try { var desc = await _fetchJson(base + '/media/mserver/description'); if (desc && desc.data) { result.version = desc.data.version || ''; } } catch(_) {}
    try { var ep = await _fetchJson(base + '/index/api/getThreadsLoad?secret=' + encodeURIComponent(secret)); result.threads = ep && ep.data ? ep.data : []; } catch(_) { result.threads = []; }
    try { var st = await _fetchJson(base + '/index/api/getStatistic?secret=' + encodeURIComponent(secret)); result.stat = st && st.data ? st.data : {}; } catch(_) { result.stat = {}; }
    try { var sy = await _fetchJson(base + '/media/mserver/systemStatistic?secret=' + encodeURIComponent(secret)); result.sysStat = sy && sy.code === 0 ? sy.data : null; } catch(_) { result.sysStat = null; }
    try { var ss = await _fetchJson(base + '/media/mserver/getSyncStatus?secret=' + encodeURIComponent(secret)); result.syncStatus = ss && ss.code === 0 ? ss.data : null; } catch(_) { result.syncStatus = null; }

    return result;
}

function _applyNodeData(node, data) {
    var id = node.id;
    _nodeStatus(id, 'online', 'Online (' + data.latency + 'ms)');
    if (data.stat) { _setText2('cl-streams-' + id, data.stat.MediaSource || 0); _setText2('cl-sess-' + id, data.stat.TcpSession || 0); }
    if (data.threads && data.threads.length) {
        var avg = Math.round(data.threads.reduce(function(s,t){return s+(t.load||0);},0) / data.threads.length);
        _setText2('cl-ep-' + id, avg + '%');
        _renderNodeThreadBars('cl-poller-' + id, data.threads);
    }
    if (data.version) _setText2('cl-version-' + id, data.version.substring(0,32));
    _setText2('cl-latency-' + id, data.latency + 'ms RTT');
    _clusterState.nodeStats[id] = { online: true, latency: data.latency, updatedAt: Date.now(), version: data.version || '', stat: data.stat, sysStat: data.sysStat, syncStatus: data.syncStatus || null };
}

function _applyNodeOffline(node, err) {
    _nodeStatus(node.id, 'offline', 'Offline');
    _clusterState.nodeStats[node.id] = { online: false, updatedAt: Date.now(), error: err ? (err.message || String(err)).substring(0,60) : 'Offline' };
}

function _nodeStatus(nid, cls, lbl) {
    var dot = document.getElementById('cl-dot-' + nid);
    var lab = document.getElementById('cl-status-' + nid);
    if (dot) dot.className = 'cl-node-status-dot ' + cls;
    if (lab) { lab.textContent = lbl; lab.className = 'cl-node-status-lbl ' + cls; }
}

function _renderNodeThreadBars(elId, threads) {
    var el = document.getElementById(elId);
    if (!el) return;
    el.innerHTML = threads.map(function(t) {
        var pct = Math.min(100, Math.max(0, t.load || 0));
        var col = pct > 80 ? '#f85149' : pct > 50 ? '#d29922' : '#3fb950';
        return '<div class="cl-thr-row"><span class="cl-thr-name">' + _esc(t.name||'') + '</span>' +
            '<div class="cl-thr-bar-wrap"><div class="cl-thr-bar" style="width:' + pct + '%;background:' + col + '"></div></div>' +
            '<span class="cl-thr-pct">' + pct + '%</span></div>';
    }).join('');
}

function _updateSummary(online, offline) {
    var el = document.getElementById('cl-summary');
    if (!el) return;
    var total = online + offline;
    var health = total ? Math.round(online/total*100) : 0;
    var cls = health === 100 ? 'ok' : health >= 50 ? 'warn' : 'err';
    el.innerHTML = '<span class="cl-sum-badge ' + cls + '">' + online + '/' + total + ' Online</span>' +
        '<span class="cl-sum-badge info">' + health + '% Healthy</span>';
}

function _updateSyncTable(nodes) {
    var wrap = document.getElementById('cl-db-sync-wrap');
    if (!wrap) return;
    var rows = nodes.map(function(n) {
        var s = _clusterState.nodeStats[n.id] || {};
        var st = s.online ? '<span class="cl-sum-badge ok" style="font-size:0.68rem;padding:1px 7px">Online</span>' : '<span class="cl-sum-badge err" style="font-size:0.68rem;padding:1px 7px">Offline</span>';
        var ts = s.updatedAt ? new Date(s.updatedAt).toLocaleTimeString() : '—';
        var streams = s.stat ? (s.stat.MediaSource || 0) : '—';
        var sess    = s.stat ? (s.stat.TcpSession  || 0) : '—';
        var cpu     = s.sysStat && s.sysStat.cpu ? Math.round(s.sysStat.cpu.usage_pct) + '%' : '—';
        var ram     = s.sysStat && s.sysStat.ram ? Math.round(s.sysStat.ram.usage_pct) + '%' : '—';
        return '<tr><td><strong>' + _esc(n.name) + '</strong>' + (n.isSelf?' <span class="cl-self-badge">SELF</span>':'') + '</td>' +
            '<td class="mono">' + _esc(n.ip) + ':' + n.httpPort + '</td>' +
            '<td class="tc">' + st + '</td>' +
            '<td class="tc mono">' + (s.latency != null ? s.latency + 'ms' : '—') + '</td>' +
            '<td class="tc">' + streams + '</td><td class="tc">' + sess + '</td>' +
            '<td class="tc">' + cpu + '</td><td class="tc">' + ram + '</td>' +
            '<td class="mono">' + ts + '</td></tr>';
    }).join('');
    wrap.innerHTML = '<div class="cl-table-scroll"><table class="cl-table">' +
        '<thead><tr><th>Node</th><th>Địa chỉ</th><th>Trạng thái</th><th>Latency</th><th>Streams</th><th>Sessions</th><th>CPU</th><th>RAM</th><th>Cập nhật</th></tr></thead>' +
        '<tbody>' + rows + '</tbody></table></div>' +
        '<p class="cl-note">CPU/RAM chỉ hiển thị khi node có <code>manager.enableAuthorize=0</code></p>';
    _updateTxnTable(nodes);
}

function _updateTxnTable(nodes) {
    var wrap = document.getElementById('cl-txn-wrap');
    if (!wrap) return;

    // Build per-node sync sections
    var html = '';
    nodes.forEach(function(n) {
        var s   = _clusterState.nodeStats[n.id] || {};
        var ss  = s.syncStatus;
        var nodeLabel = _esc(n.name) + (n.isSelf ? ' <span class="cl-self-badge">SELF</span>' : '');

        if (!s.online) {
            html += '<div class="cl-txn-node"><div class="cl-txn-node-hd">' + nodeLabel + ' <span class="cl-sum-badge err" style="font-size:.66rem">Offline</span></div></div>';
            return;
        }
        if (!ss) {
            html += '<div class="cl-txn-node"><div class="cl-txn-node-hd">' + nodeLabel + ' <span class="cl-sum-badge warn" style="font-size:.66rem">getSyncStatus không khả dụng</span></div></div>';
            return;
        }

        // ── transaction_sequence table ──────────────────────────
        var seqRows = (ss.transaction_sequence || []).map(function(seq) {
            return '<tr><td class="mono">' + _esc(seq.peer_guid.substring(0,14)) + '</td>' +
                '<td class="mono">' + _esc(seq.db_guid.substring(0,14)) + '</td>' +
                '<td class="tc fw">' + (seq.sequence || 0) + '</td></tr>';
        }).join('');
        if (!seqRows) seqRows = '<tr><td colspan="3" class="tc cl-note">Chưa có dữ liệu</td></tr>';

        // ── transaction_ack_log table ───────────────────────────
        var ackRows = (ss.transaction_ack_log || []).map(function(a) {
            var dt = a.updated_at ? new Date(a.updated_at).toLocaleTimeString() : '—';
            return '<tr>' +
                '<td class="mono">' + _esc(a.peer_guid.substring(0,14)) + '</td>' +
                '<td class="mono">' + _esc(a.src_peer_guid.substring(0,14)) + '</td>' +
                '<td class="tc fw">' + (a.acked_seq || 0) + '</td>' +
                '<td class="mono">' + dt + '</td></tr>';
        }).join('');
        if (!ackRows) ackRows = '<tr><td colspan="4" class="tc cl-note">Chưa có peer nào ack</td></tr>';

        // ── transaction_log counts ──────────────────────────────
        var logCounts = ss.transaction_log_counts || [];
        var totalLogRows = logCounts.reduce(function(s, lc) { return s + (lc.count || 0); }, 0);
        var logCountRows = logCounts.map(function(lc) {
            return '<tr><td class="mono">' + _esc(lc.peer_guid.substring(0,14)) + '</td>' +
                '<td class="mono">' + _esc(lc.db_guid.substring(0,14)) + '</td>' +
                '<td class="tc fw">' + lc.count + '</td></tr>';
        }).join('');
        if (!logCountRows) logCountRows = '<tr><td colspan="3" class="tc cl-note">Log trống</td></tr>';

        html +=
            '<div class="cl-txn-node">' +
                '<div class="cl-txn-node-hd">' +
                    nodeLabel +
                    ' <span class="cl-txn-meta mono">' + _esc((ss.mediaServerId||'').substring(0,16)) + '</span>' +
                    ' <span class="cl-txn-meta mono">db: ' + _esc((ss.db_guid||'').substring(0,12)) + '</span>' +
                    ' <span class="cl-sum-badge info" style="font-size:.66rem">log total: ' + totalLogRows + '</span>' +
                '</div>' +
                '<div class="cl-txn-grid">' +
                    '<div class="cl-txn-panel">' +
                        '<div class="cl-txn-panel-title">transaction_sequence (cursors nhận được)</div>' +
                        '<div class="cl-table-scroll"><table class="cl-table">' +
                            '<thead><tr><th>peer_guid</th><th>db_guid</th><th>sequence</th></tr></thead>' +
                            '<tbody>' + seqRows + '</tbody>' +
                        '</table></div>' +
                    '</div>' +
                    '<div class="cl-txn-panel">' +
                        '<div class="cl-txn-panel-title">transaction_peer_ack_log (peer đã pull đến đây)</div>' +
                        '<div class="cl-table-scroll"><table class="cl-table">' +
                            '<thead><tr><th>peer (puller)</th><th>src_peer (source)</th><th>acked_seq</th><th>updated_at</th></tr></thead>' +
                            '<tbody>' + ackRows + '</tbody>' +
                        '</table></div>' +
                    '</div>' +
                    '<div class="cl-txn-panel">' +
                        '<div class="cl-txn-panel-title">transaction_log (rows còn trong DB)</div>' +
                        '<div class="cl-table-scroll"><table class="cl-table">' +
                            '<thead><tr><th>peer_guid</th><th>db_guid</th><th>count</th></tr></thead>' +
                            '<tbody>' + logCountRows + '</tbody>' +
                        '</table></div>' +
                    '</div>' +
                '</div>' +
            '</div>';
    });
    wrap.innerHTML = html || '<p class="cl-note">Không có dữ liệu</p>';
}

function _clusterAddNodeDialog() {
    var bd = document.getElementById('cl-dialog-backdrop'); var dg = document.getElementById('cl-dialog');
    if (bd) bd.style.display = 'block'; if (dg) dg.style.display = 'flex';
}
function _closeNodeDialog() {
    var bd = document.getElementById('cl-dialog-backdrop'); var dg = document.getElementById('cl-dialog');
    if (bd) bd.style.display = 'none'; if (dg) dg.style.display = 'none';
}
async function _saveNodeFromDialog() {
    var id   = (document.getElementById('cl-d-id')||{}).value || '';
    var name = (document.getElementById('cl-d-name')||{}).value || id;
    var ip   = (document.getElementById('cl-d-ip')||{}).value || '';
    var port = parseInt((document.getElementById('cl-d-port')||{}).value || '80', 10);
    if (!id || !ip) { alert('Cần nhập ID và IP'); return; }
    var cfg = window._s3Config || {};
    var peerList = []; try { peerList = JSON.parse(cfg['peer.peer_list'] || '[]'); } catch(_) {}
    var idx = peerList.findIndex(function(p){return p.id===id;});
    var entry = {id:id, name:name, ip:ip, http_port:port};
    if (idx>=0) peerList[idx] = entry; else peerList.push(entry);
    try {
        await S3Auth.apiPost('/index/api/setServerConfig', {'peer.peer_list': JSON.stringify(peerList)});
        window._s3Config['peer.peer_list'] = JSON.stringify(peerList);
        _closeNodeDialog();
        initCluster();
    } catch(e) { alert('Lỗi: ' + e.message); }
}

function _buildBase(n) {
    var proto = n.clientUseSsl ? 'https' : 'http';
    var port  = n.httpPort || 80;
    var host  = n.domain || n.ip;
    return (proto==='http'&&port===80)||(proto==='https'&&port===443) ? proto+'://'+host : proto+'://'+host+':'+port;
}
function _parseUrl(url) {
    try { var u = new URL(url); return {hostname: u.hostname, port: parseInt(u.port||(u.protocol==='https:'?'443':'80'),10)}; }
    catch(_) { return {hostname:'localhost',port:80}; }
}
async function _fetchJson(url) {
    var r = await fetch(url, {credentials:'omit'});
    if (!r.ok) throw new Error('HTTP '+r.status);
    return r.json();
}
function _setText2(id, v) { var el=document.getElementById(id); if(el) el.textContent=v; }
function _esc(s) { return String(s||'').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;'); }
function _setClusterError(msg) { var g=document.getElementById('cl-grid'); if(g) g.innerHTML='<div class="cl-error">'+_esc(msg)+'</div>'; }

function _injectClusterCSS() {
    if (document.getElementById('cluster-styles')) return;
    var s = document.createElement('style');
    s.id = 'cluster-styles';
    s.textContent = [
    '#cluster-content{padding-bottom:32px}',
    '.cl-toolbar{display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:12px;margin-bottom:14px}',
    '.cl-title-block{display:flex;align-items:center;gap:8px;font-size:.92rem;font-weight:600;color:var(--c-text)}',
    '.cl-icon{width:18px;height:18px;color:var(--c-accent);flex-shrink:0}',
    '.cl-toolbar-right{display:flex;align-items:center;gap:8px;flex-wrap:wrap}',
    '.cl-ts{font-size:.72rem;color:var(--c-muted);font-family:monospace}',
    '.cl-btn{display:inline-flex;align-items:center;gap:5px;padding:6px 12px;border-radius:5px;font-size:.78rem;font-weight:600;cursor:pointer;border:1px solid var(--c-border);background:var(--c-elev);color:var(--c-text);transition:background .12s,border-color .12s}',
    '.cl-btn:hover{background:var(--c-surf);border-color:var(--c-accent)}',
    '.cl-btn.primary{background:var(--c-accent);border-color:var(--c-accent);color:#fff}',
    '.cl-btn.primary:hover{background:#1f6feb}',
    '.cl-summary{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:16px}',
    '.cl-sum-badge{display:inline-flex;align-items:center;gap:4px;padding:3px 10px;border-radius:20px;font-size:.74rem;font-weight:700;border:1px solid transparent}',
    '.cl-sum-badge.ok{background:rgba(63,185,80,.12);color:var(--c-success);border-color:rgba(63,185,80,.3)}',
    '.cl-sum-badge.warn{background:rgba(210,153,34,.12);color:var(--c-warn);border-color:rgba(210,153,34,.3)}',
    '.cl-sum-badge.err{background:rgba(248,81,73,.12);color:var(--c-danger);border-color:rgba(248,81,73,.3)}',
    '.cl-sum-badge.info{background:rgba(56,139,253,.12);color:var(--c-accent);border-color:rgba(56,139,253,.25)}',
    '.cl-self-badge{font-size:.6rem;background:rgba(56,139,253,.15);color:var(--c-accent);border:1px solid rgba(56,139,253,.3);border-radius:4px;padding:0 4px;vertical-align:middle;font-weight:700;letter-spacing:.04em}',
    '.cl-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(280px,1fr));gap:12px}',
    '.cl-loading{display:flex;align-items:center;gap:10px;padding:24px;color:var(--c-muted);font-size:.85rem}',
    '.cl-empty{display:flex;flex-direction:column;align-items:center;gap:10px;padding:48px;color:var(--c-muted);text-align:center}',
    '.cl-error{padding:16px;color:var(--c-danger);font-size:.82rem}',
    '.cl-spinner{display:inline-block;width:16px;height:16px;border:2px solid var(--c-border);border-top-color:var(--c-accent);border-radius:50%;animation:cl-spin .65s linear infinite}',
    '@keyframes cl-spin{to{transform:rotate(360deg)}}',
    '.cl-node-card{background:var(--c-surf);border:1px solid var(--c-border);border-radius:8px;overflow:hidden;transition:border-color .15s,box-shadow .15s}',
    '.cl-node-card:hover{border-color:rgba(56,139,253,.35);box-shadow:0 0 0 3px rgba(56,139,253,.06)}',
    '.cl-node-header{display:flex;align-items:center;gap:10px;padding:12px 14px 10px;border-bottom:1px solid var(--c-border)}',
    '.cl-node-status-dot{width:10px;height:10px;border-radius:50%;flex-shrink:0;transition:background .3s}',
    '.cl-node-status-dot.online{background:var(--c-success);box-shadow:0 0 0 3px rgba(63,185,80,.2)}',
    '.cl-node-status-dot.offline{background:var(--c-danger)}',
    '.cl-node-status-dot.checking{background:var(--c-muted);animation:cl-pulse 1.2s step-start infinite}',
    '@keyframes cl-pulse{0%,100%{opacity:1}50%{opacity:.3}}',
    '.cl-node-info{flex:1;min-width:0}',
    '.cl-node-name{font-size:.86rem;font-weight:700;color:var(--c-text);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}',
    '.cl-node-addr{font-size:.7rem;color:var(--c-muted);font-family:monospace}',
    '.cl-node-status-lbl{font-size:.7rem;font-weight:600;white-space:nowrap;flex-shrink:0}',
    '.cl-node-status-lbl.online{color:var(--c-success)}',
    '.cl-node-status-lbl.offline{color:var(--c-danger)}',
    '.cl-node-status-lbl.checking{color:var(--c-muted)}',
    '.cl-node-metrics{display:grid;grid-template-columns:repeat(2,1fr)}',
    '.cl-metric{padding:7px 14px;display:flex;flex-direction:column;gap:2px;border-bottom:1px solid var(--c-border)}',
    '.cl-metric:nth-child(odd){border-right:1px solid var(--c-border)}',
    '.cl-m-lbl{font-size:.66rem;color:var(--c-muted);text-transform:uppercase;letter-spacing:.06em;font-weight:600}',
    '.cl-m-val{font-size:.9rem;font-weight:700;color:var(--c-text)}',
    '.cl-m-val.mono{font-family:monospace;font-size:.72rem}',
    '.cl-node-poller{padding:8px 14px;display:flex;flex-direction:column;gap:4px}',
    '.cl-thr-row{display:flex;align-items:center;gap:6px;font-size:.7rem}',
    '.cl-thr-name{color:var(--c-muted);min-width:70px;font-family:monospace;font-size:.66rem;flex-shrink:0}',
    '.cl-thr-bar-wrap{flex:1;height:6px;background:var(--c-elev);border-radius:3px;overflow:hidden}',
    '.cl-thr-bar{height:100%;border-radius:3px;transition:width .4s ease}',
    '.cl-thr-pct{font-size:.66rem;color:var(--c-text);min-width:28px;text-align:right}',
    '.cl-node-footer{display:flex;align-items:center;justify-content:space-between;padding:6px 14px 8px;font-size:.68rem;border-top:1px solid var(--c-border);color:var(--c-muted)}',
    '.cl-section-title{font-size:.74rem;font-weight:700;color:var(--c-muted);text-transform:uppercase;letter-spacing:.07em;margin-bottom:10px}',
    '.cl-note{font-size:.75rem;color:var(--c-muted);margin-top:4px}',
    'code{background:var(--c-elev);padding:1px 5px;border-radius:4px;font-size:.85em}',
    '.cl-table-scroll{overflow-x:auto;border-radius:6px;border:1px solid var(--c-border)}',
    '.cl-table{width:100%;border-collapse:collapse;font-size:.8rem}',
    '.cl-table th{padding:8px 12px;text-align:left;font-size:.68rem;font-weight:600;color:var(--c-muted);text-transform:uppercase;letter-spacing:.05em;background:var(--c-elev);border-bottom:1px solid var(--c-border);white-space:nowrap}',
    '.cl-table td{padding:7px 12px;border-bottom:1px solid var(--c-border);color:var(--c-text);white-space:nowrap}',
    '.cl-table tbody tr:last-child td{border-bottom:none}',
    '.cl-table tbody tr:hover td{background:rgba(255,255,255,.02)}',
    '.tc{text-align:left}.mono{font-family:monospace;font-size:.76rem}',
    '.cl-dialog-backdrop{display:none;position:fixed;inset:0;background:rgba(0,0,0,.6);z-index:1000}',
    '.cl-dialog{display:none;position:fixed;top:50%;left:50%;transform:translate(-50%,-50%);z-index:1001;background:var(--c-surf);border:1px solid var(--c-border);border-radius:10px;width:min(400px,95vw);flex-direction:column;box-shadow:0 8px 40px rgba(0,0,0,.5)}',
    '.cl-dialog-hd{padding:16px 20px;font-size:.9rem;font-weight:700;border-bottom:1px solid var(--c-border)}',
    '.cl-form{padding:16px 20px;display:flex;flex-direction:column;gap:6px}',
    '.cl-form label{font-size:.78rem;color:var(--c-muted);font-weight:600}',
    '.cl-input{padding:8px 10px;background:var(--c-elev);border:1px solid var(--c-border);color:var(--c-text);border-radius:5px;font-size:.85rem;outline:none;transition:border-color .15s}',
    '.cl-input:focus{border-color:var(--c-accent)}',
    '.cl-dialog-footer{display:flex;justify-content:flex-end;gap:8px;padding:12px 20px;border-top:1px solid var(--c-border)}',
    /* Transaction sync section */
    '.cl-txn-node{background:var(--c-surf);border:1px solid var(--c-border);border-radius:8px;padding:14px 16px;margin-bottom:14px}',
    '.cl-txn-node-hd{display:flex;align-items:center;gap:8px;flex-wrap:wrap;font-size:.84rem;font-weight:700;color:var(--c-text);margin-bottom:10px}',
    '.cl-txn-meta{font-size:.68rem;color:var(--c-muted);background:var(--c-elev);padding:1px 6px;border-radius:4px;border:1px solid var(--c-border)}',
    '.cl-txn-grid{display:flex;flex-direction:column;gap:10px}',
    '.cl-txn-panel{display:flex;flex-direction:column;gap:4px}',
    '.cl-txn-panel-title{font-size:.68rem;font-weight:600;color:var(--c-muted);text-transform:uppercase;letter-spacing:.05em;margin-bottom:4px}',
    ].join('\n');
    document.head.appendChild(s);
}