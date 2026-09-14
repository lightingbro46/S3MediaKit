// =============================================================================
// pc-restore.js — Restore-aware replay coordinator for COLD timeline periods.
// Owns chunking, restoreJob create/detail polling, prefetch and blocking overlay.
// =============================================================================

(function (global) {
    'use strict';

    var CHUNK_SECONDS = 300;

    function _number(value) {
        var n = Number(value);
        return isFinite(n) ? n : 0;
    }

    function _periodStart(period) {
        return _number(period && period.startTime);
    }

    function _periodEnd(period) {
        return _periodStart(period) + Math.max(0, _number(period && period.duration));
    }

    function normalizeRestoreRanges(hours) {
        var ranges = [];
        (hours || []).forEach(function (periods) {
            (periods || []).forEach(function (period) {
                if (!(period && (period.restoreRequired === true || period.restore_required === true))) return;
                var start = _periodStart(period);
                var end = _periodEnd(period);
                if (!start || end <= start) return;
                ranges.push({
                    startTime: start,
                    endTime: end,
                    tier: period.tier || 'COLD',
                    mediaServerId: period.mediaServerId || '',
                    restoreRequired: true
                });
            });
        });
        var grouped = {};
        ranges.forEach(function (range) {
            var key = range.mediaServerId + '\n' + range.tier;
            grouped[key] = grouped[key] || [];
            grouped[key].push(range);
        });
        var merged = [];
        Object.keys(grouped).forEach(function (key) {
            grouped[key].sort(function (a, b) {
                if (a.startTime !== b.startTime) return a.startTime - b.startTime;
                return a.endTime - b.endTime;
            });
            grouped[key].forEach(function (range) {
                var last = merged.length ? merged[merged.length - 1] : null;
                if (last && last.mediaServerId === range.mediaServerId &&
                    last.tier === range.tier && range.startTime <= last.endTime) {
                    last.endTime = Math.max(last.endTime, range.endTime);
                    return;
                }
                merged.push(range);
            });
        });
        merged.sort(function (a, b) { return a.startTime - b.startTime; });
        return merged;
    }

    function splitRestoreRange(range, chunkSeconds, selectedWallTime) {
        var size = Math.max(1, Math.floor(_number(chunkSeconds) || CHUNK_SECONDS));
        var chunks = [];
        if (!range || range.endTime <= range.startTime) return chunks;
        var requestedStart = Math.floor(_number(selectedWallTime));
        var firstStart = requestedStart >= range.startTime && requestedStart < range.endTime
            ? requestedStart : range.startTime;
        for (var start = firstStart; start < range.endTime; start += size) {
            chunks.push({
                startTime: start,
                endTime: Math.min(range.endTime, start + size),
                jobId: '',
                snapshot: null,
                status: 'IDLE',
                pollTimer: null,
                requestInFlight: false,
                prefetchStarted: false,
                error: ''
            });
        }
        return chunks;
    }

    function findChunkIndex(chunks, wallTime) {
        var stamp = _number(wallTime);
        for (var i = 0; i < (chunks || []).length; ++i) {
            if (stamp >= chunks[i].startTime && stamp < chunks[i].endTime) return i;
        }
        return -1;
    }

    function prefetchThreshold(chunk) {
        if (!chunk) return 0;
        return chunk.startTime + (chunk.endTime - chunk.startTime) * 0.8;
    }

    function playbackStatus(snapshot) {
        var status = String((snapshot && snapshot.playback_status) || '').toUpperCase();
        if (status) return status;
        var jobStatus = String((snapshot && snapshot.status) || '').toUpperCase();
        if (jobStatus === 'PENDING' || jobStatus === 'RUNNING') return 'RESTORING';
        if (jobStatus === 'DONE') {
            return snapshot && snapshot.playback_ready ? 'READY' : 'NOT_READY';
        }
        return jobStatus || 'RESTORING';
    }

    function isReady(snapshot) {
        return playbackStatus(snapshot) === 'READY' && !!(snapshot && snapshot.playback_ready);
    }

    function boundaryDecision(current, next, wallTime) {
        if (!current || _number(wallTime) < current.endTime) return 'PLAY';
        if (!next) return 'COMPLETE';
        return isReady(next.snapshot) ? 'ADVANCE' : 'BLOCK';
    }

    global.PlayerRestoreUtils = {
        CHUNK_SECONDS: CHUNK_SECONDS,
        normalizeRestoreRanges: normalizeRestoreRanges,
        splitRestoreRange: splitRestoreRange,
        findChunkIndex: findChunkIndex,
        prefetchThreshold: prefetchThreshold,
        playbackStatus: playbackStatus,
        isReady: isReady,
        boundaryDecision: boundaryDecision
    };

    global.initPlaybackRestore = function (player, state) {
        var overlay = document.getElementById('player-restore');
        var donut = document.getElementById('player-restore-donut');
        var percentEl = document.getElementById('player-restore-percent');
        var statusEl = document.getElementById('player-restore-status');
        var captionEl = document.getElementById('player-restore-caption');
        var jobEl = document.getElementById('player-restore-job');
        var retryBtn = document.getElementById('player-restore-retry');
        var stopBtn = document.getElementById('player-restore-stop');
        var session = null;
        var generation = 0;
        var visibleChunk = null;

        function _clearChunkTimer(chunk) {
            if (chunk && chunk.pollTimer) window.clearTimeout(chunk.pollTimer);
            if (chunk) chunk.pollTimer = null;
        }

        function _clearSessionTimers(target) {
            if (!target) return;
            (target.chunks || []).forEach(_clearChunkTimer);
        }

        function _hideOverlay() {
            visibleChunk = null;
            if (overlay) overlay.classList.remove('show');
        }

        function _terminalCaption(status) {
            if (status === 'FAILED') return 'Không thể khôi phục dữ liệu';
            if (status === 'NOT_READY') return 'Dữ liệu khôi phục không còn đầy đủ';
            if (status === 'CANCELLED') return 'Đã hủy khôi phục dữ liệu';
            if (status === 'EXPIRED') return 'Dữ liệu tạm đã hết hạn';
            return '';
        }

        function _renderOverlay(chunk) {
            if (!overlay || !chunk) return;
            visibleChunk = chunk;
            var snapshot = chunk.snapshot || {};
            var status = chunk.status === 'CREATING' ? 'RESTORING' : playbackStatus(snapshot);
            if (chunk.status === 'FAILED' && !snapshot.playback_status) status = 'FAILED';
            var total = Math.max(0, parseInt(snapshot.total_segment_count, 10) || 0);
            var restored = Math.max(0, parseInt(snapshot.restored_segment_count, 10) || 0);
            var pct = Math.max(0, Math.min(100, _number(snapshot.segment_progress_percent)));
            var determinate = total > 0;
            var caption = 'Đang khôi phục dữ liệu ... ' + restored + '/' + total;
            var terminalCaption = _terminalCaption(status);
            if (terminalCaption) caption = terminalCaption;
            if (status === 'READY') {
                pct = 100;
                caption = 'Dữ liệu đã sẵn sàng';
            }

            donut.style.setProperty('--restore-progress', pct.toFixed(1) + '%');
            donut.classList.toggle('indeterminate', !determinate && status === 'RESTORING');
            donut.setAttribute('aria-valuenow', String(Math.round(pct)));
            donut.setAttribute('aria-label', determinate
                ? 'Đã khôi phục ' + restored + ' trên ' + total + ' đoạn dữ liệu'
                : 'Đang xác định số đoạn dữ liệu cần khôi phục');
            percentEl.textContent = determinate ? Math.round(pct) + '%' : '…';
            statusEl.textContent = status;
            captionEl.textContent = caption;
            jobEl.textContent = (snapshot.job_id || chunk.jobId || '') +
                (chunk.error ? ' · ' + chunk.error : '');
            retryBtn.classList.toggle('show', !!terminalCaption);
            retryBtn.textContent = status === 'FAILED' ? 'Thử lại' : 'Khôi phục lại';
            overlay.classList.add('show');
        }

        function invalidateSession() {
            generation += 1;
            _clearSessionTimers(session);
            session = null;
            _hideOverlay();
        }

        function _apiFormPost(path, body) {
            var auth = global.S3Auth && global.S3Auth.get();
            if (!auth) return Promise.reject(new Error('Not authenticated'));
            var form = new URLSearchParams();
            Object.keys(body || {}).forEach(function (key) {
                if (body[key] != null) form.set(key, String(body[key]));
            });
            return fetch(auth.serverUrl + path + '?secret=' + encodeURIComponent(auth.secret), {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' },
                body: form.toString(),
                credentials: 'omit'
            }).then(function (response) {
                if (!response.ok) throw new Error('HTTP ' + response.status);
                return response.json();
            }).then(function (result) {
                if (result.code !== 0) throw new Error(result.msg || 'API error code ' + result.code);
                return result.data || {};
            });
        }

        function _apiDetail(jobId) {
            var auth = global.S3Auth && global.S3Auth.get();
            if (!auth) return Promise.reject(new Error('Not authenticated'));
            var url = auth.serverUrl + '/media/mserver/storage/restoreJob/detail'
                + '?secret=' + encodeURIComponent(auth.secret)
                + '&job_id=' + encodeURIComponent(jobId);
            return fetch(url, { credentials: 'omit' }).then(function (response) {
                return response.json().then(function (result) {
                    if (!response.ok || result.code !== 0) {
                        var error = new Error(result.msg || 'HTTP ' + response.status);
                        error.apiCode = result.code;
                        error.retryable = response.status >= 500;
                        throw error;
                    }
                    return result;
                });
            }).then(function (result) {
                return result.data || {};
            });
        }

        function _isCurrent(target, chunk, sessionGeneration) {
            return session === target && generation === sessionGeneration &&
                target.chunks.indexOf(chunk) !== -1;
        }

        function _schedulePoll(target, chunk, chunkIndex, sessionGeneration, delay) {
            _clearChunkTimer(chunk);
            if (!_isCurrent(target, chunk, sessionGeneration) || !chunk.jobId) return;
            var wait = document.hidden ? Math.max(delay, 5000) : delay;
            wait += Math.floor(Math.random() * 250);
            chunk.pollTimer = window.setTimeout(function () {
                _pollChunk(target, chunk, chunkIndex, sessionGeneration);
            }, wait);
        }

        function _resumeReadyChunk(target, chunkIndex) {
            var chunk = target.chunks[chunkIndex];
            target.currentChunkIndex = chunkIndex;
            target.blocked = false;
            target.waitingChunkIndex = -1;
            target.awaitingPlayback = true;
            _renderOverlay(chunk);
            state.seekToWallTime(target.started ? chunk.startTime : target.selectedWallTime);
            target.started = true;
        }

        function _applySnapshot(target, chunk, chunkIndex, sessionGeneration, snapshot) {
            if (!_isCurrent(target, chunk, sessionGeneration)) return;
            chunk.snapshot = snapshot;
            chunk.jobId = snapshot.job_id || chunk.jobId;
            chunk.status = playbackStatus(snapshot);
            chunk.error = '';
            if (visibleChunk === chunk || (!target.started && chunkIndex === target.currentChunkIndex) ||
                (target.blocked && chunkIndex === target.waitingChunkIndex)) {
                _renderOverlay(chunk);
            }

            if (isReady(snapshot)) {
                _clearChunkTimer(chunk);
                if (!target.started && chunkIndex === target.currentChunkIndex) {
                    _resumeReadyChunk(target, chunkIndex);
                } else if (target.blocked && chunkIndex === target.waitingChunkIndex) {
                    _resumeReadyChunk(target, chunkIndex);
                }
                return;
            }
            if (chunk.status === 'RESTORING') {
                _schedulePoll(target, chunk, chunkIndex, sessionGeneration, 2000);
            } else {
                _clearChunkTimer(chunk);
            }
        }

        function _pollChunk(target, chunk, chunkIndex, sessionGeneration) {
            if (!_isCurrent(target, chunk, sessionGeneration) || chunk.requestInFlight || !chunk.jobId) return;
            chunk.requestInFlight = true;
            _apiDetail(chunk.jobId).then(function (snapshot) {
                if (!_isCurrent(target, chunk, sessionGeneration)) return;
                chunk.requestInFlight = false;
                _applySnapshot(target, chunk, chunkIndex, sessionGeneration, snapshot);
            }).catch(function (error) {
                if (!_isCurrent(target, chunk, sessionGeneration)) return;
                chunk.requestInFlight = false;
                chunk.error = (error && error.message) || 'Không cập nhật được trạng thái restore';
                if (error && error.retryable === false) {
                    chunk.status = error.apiCode === 914018 ? 'NOT_READY' : 'FAILED';
                    chunk.snapshot = {
                        playback_status: chunk.status,
                        playback_ready: false,
                        job_id: chunk.jobId
                    };
                    _clearChunkTimer(chunk);
                    if (visibleChunk === chunk || (target.blocked && chunkIndex === target.waitingChunkIndex)) {
                        _renderOverlay(chunk);
                    }
                    return;
                }
                if (visibleChunk === chunk) _renderOverlay(chunk);
                _schedulePoll(target, chunk, chunkIndex, sessionGeneration, 5000);
            });
        }

        function _restoreChunk(target, chunkIndex, blocking) {
            var chunk = target.chunks[chunkIndex];
            if (!chunk) return;
            if (blocking) {
                target.blocked = true;
                target.waitingChunkIndex = chunkIndex;
                _renderOverlay(chunk);
            }
            if (isReady(chunk.snapshot)) {
                if (blocking) _resumeReadyChunk(target, chunkIndex);
                return;
            }
            if (chunk.status === 'FAILED' || chunk.status === 'NOT_READY' ||
                chunk.status === 'CANCELLED' || chunk.status === 'EXPIRED') return;
            if (chunk.requestInFlight || chunk.status === 'RESTORING' || chunk.status === 'CREATING') return;

            var sessionGeneration = target.generation;
            chunk.status = 'CREATING';
            chunk.error = '';
            chunk.requestInFlight = true;
            if (blocking) _renderOverlay(chunk);
            var request = {
                camera_id: target.cameraId,
                stream_id: target.streamId,
                start_time: chunk.startTime,
                end_time: chunk.endTime,
                target_tier: 'HOT',
                reason: 'PLAYBACK'
            };
            _apiFormPost('/media/mserver/storage/restoreJob/create', request).then(function (snapshot) {
                if (!_isCurrent(target, chunk, sessionGeneration)) return;
                chunk.requestInFlight = false;
                if (!snapshot.job_id) throw new Error('API không trả về restore job hợp lệ');
                _applySnapshot(target, chunk, chunkIndex, sessionGeneration, snapshot);
            }).catch(function (error) {
                if (!_isCurrent(target, chunk, sessionGeneration)) return;
                chunk.requestInFlight = false;
                chunk.status = 'FAILED';
                chunk.snapshot = { playback_status: 'FAILED', playback_ready: false };
                chunk.error = (error && error.message) || 'Không tạo được restore job';
                if (blocking || visibleChunk === chunk) _renderOverlay(chunk);
            });
        }

        function _prefetchNext(target) {
            var nextIndex = target.currentChunkIndex + 1;
            var next = target.chunks[nextIndex];
            if (!next || next.prefetchStarted) return;
            next.prefetchStarted = true;
            _restoreChunk(target, nextIndex, false);
        }

        function _blockForNext(target, nextIndex) {
            var next = target.chunks[nextIndex];
            if (!next) return;
            var video = player.videoEl;
            if (video && !video.paused) video.pause();
            target.blocked = true;
            target.waitingChunkIndex = nextIndex;
            _renderOverlay(next);
            _restoreChunk(target, nextIndex, true);
        }

        function _finishRestoreRange(target) {
            if (session !== target) return;
            _clearSessionTimers(target);
            session = null;
            _hideOverlay();
        }

        function _onPlaybackTime(elapsedMs) {
            var target = session;
            if (!target || !target.started || target.blocked || state.mode !== 'replay') return;
            var wallTime = state.replayStartTime + _number(elapsedMs) / 1000;
            target.lastWallTime = wallTime;
            var current = target.chunks[target.currentChunkIndex];
            if (!current) return;

            if (wallTime >= prefetchThreshold(current)) _prefetchNext(target);
            if (wallTime < current.endTime) return;

            var nextIndex = target.currentChunkIndex + 1;
            var next = target.chunks[nextIndex];
            var decision = boundaryDecision(current, next, wallTime);
            if (decision === 'COMPLETE') {
                _finishRestoreRange(target);
                return;
            }
            if (decision === 'ADVANCE') {
                target.currentChunkIndex = nextIndex;
                if (wallTime >= prefetchThreshold(next)) _prefetchNext(target);
            } else {
                _blockForNext(target, nextIndex);
            }
        }

        function _onPlayerState(playerState) {
            var target = session;
            if (!target) return;
            if (playerState === 'connecting' && target.awaitingPlayback) {
                _hideOverlay();
                return;
            }
            if (playerState === 'playing' && target.awaitingPlayback) {
                target.awaitingPlayback = false;
                _hideOverlay();
                return;
            }
            if (playerState === 'error' && target.awaitingPlayback) {
                target.awaitingPlayback = false;
                _hideOverlay();
                return;
            }
            if (playerState !== 'stopped' || target.blocked || !target.started) return;
            var current = target.chunks[target.currentChunkIndex];
            if (!current || target.lastWallTime < current.endTime - 2) return;
            var nextIndex = target.currentChunkIndex + 1;
            var next = target.chunks[nextIndex];
            if (!next) {
                _finishRestoreRange(target);
            } else if (isReady(next.snapshot)) {
                _resumeReadyChunk(target, nextIndex);
            } else {
                _blockForNext(target, nextIndex);
            }
        }

        function requestReplayAtWallTime(wallTime) {
            var restoreRange = state.findRestoreRange ? state.findRestoreRange(wallTime) : null;
            if (!restoreRange) {
                invalidateSession();
                state.seekToWallTime(wallTime);
                return;
            }

            invalidateSession();
            // The first restore window starts at the exact seek position. This
            // avoids downloading the unused beginning of a large COLD range;
            // subsequent windows remain contiguous for prefetch.
            var chunks = splitRestoreRange(restoreRange, CHUNK_SECONDS, wallTime);
            var chunkIndex = findChunkIndex(chunks, wallTime);
            if (chunkIndex < 0) return;
            var cameraId = (document.getElementById('camera-id').value || '').trim();
            var streamId = state.getActiveStreamId ? state.getActiveStreamId() : '';
            session = {
                generation: generation,
                cameraId: cameraId,
                streamId: streamId,
                restoreRange: restoreRange,
                chunks: chunks,
                currentChunkIndex: chunkIndex,
                waitingChunkIndex: chunkIndex,
                selectedWallTime: Math.floor(_number(wallTime)),
                lastWallTime: Math.floor(_number(wallTime)),
                started: false,
                blocked: true,
                awaitingPlayback: false
            };
            var video = player.videoEl;
            if (video && !video.paused) video.pause();
            _restoreChunk(session, chunkIndex, true);
        }

        state.requestReplayAtWallTime = requestReplayAtWallTime;
        state.invalidateRestoreSession = invalidateSession;
        state.onUserPlaybackStop = invalidateSession;

        if (retryBtn) retryBtn.addEventListener('click', function () {
            if (!session || !visibleChunk) return;
            var index = session.chunks.indexOf(visibleChunk);
            if (index < 0) return;
            _clearChunkTimer(visibleChunk);
            visibleChunk.jobId = '';
            visibleChunk.snapshot = null;
            visibleChunk.status = 'IDLE';
            visibleChunk.error = '';
            _restoreChunk(session, index, true);
        });
        if (stopBtn) stopBtn.addEventListener('click', function () {
            invalidateSession();
            state.stopped = true;
            player.stop();
            if (state.hideNoData) state.hideNoData();
            if (state.clearFrame) state.clearFrame();
            if (state.showStoppedOverlay) state.showStoppedOverlay();
        });

        var cameraInput = document.getElementById('camera-id');
        if (cameraInput) cameraInput.addEventListener('input', invalidateSession);
        document.addEventListener('visibilitychange', function () {
            if (!session) return;
            [session.currentChunkIndex, session.currentChunkIndex + 1].forEach(function (index) {
                var chunk = session && session.chunks[index];
                if (!chunk || playbackStatus(chunk.snapshot) !== 'RESTORING') return;
                _schedulePoll(session, chunk, index, session.generation, document.hidden ? 5000 : 0);
            });
        });
        player.on('timeupdate', _onPlaybackTime);
        player.on('statechange', _onPlayerState);
    };
})(typeof window !== 'undefined' ? window : globalThis);
