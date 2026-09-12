'use strict';

const assert = require('assert');

require('../../../www/console/player/pc-restore.js');

const utils = global.PlayerRestoreUtils;

function period(startTime, duration, restoreRequired, mediaServerId) {
    return {
        startTime,
        duration,
        tier: restoreRequired ? 'COLD' : 'HOT',
        restoreRequired,
        mediaServerId: mediaServerId || 'node-1'
    };
}

function snapshot(status, ready) {
    return { playback_status: status, playback_ready: !!ready };
}

assert.ok(utils, 'PlayerRestoreUtils must be exported');

const hours = [];
hours[0] = [
    period(1000, 120, true),
    period(1120, 180, true),
    period(1400, 100, false)
];
hours[1] = [period(1300, 300, true)];
assert.deepStrictEqual(utils.normalizeRestoreRanges(hours), [{
    startTime: 1000,
    endTime: 1600,
    tier: 'COLD',
    mediaServerId: 'node-1',
    restoreRequired: true
}], 'adjacent and overlapping restore periods must be merged');

const differentServers = [[
    period(2000, 300, true, 'node-1'),
    period(2300, 300, true, 'node-2')
]];
assert.strictEqual(utils.normalizeRestoreRanges(differentServers).length, 2,
    'ranges from different media servers must remain separate');

const backendShape = [[{
    startTime: 2500,
    duration: 60,
    tier: 'COLD',
    restore_required: true,
    mediaServerId: 'node-1'
}]];
assert.strictEqual(utils.normalizeRestoreRanges(backendShape).length, 1,
    'the backend restore_required field must be recognized');

const separatedRanges = [[
    period(2600, 60, true),
    period(2661, 60, true)
]];
assert.strictEqual(utils.normalizeRestoreRanges(separatedRanges).length, 2,
    'a recording gap must keep restore ranges separate');

const interleavedServers = [[
    period(3000, 100, true, 'node-1'),
    period(3020, 20, true, 'node-2'),
    period(3100, 100, true, 'node-1')
]];
assert.deepStrictEqual(utils.normalizeRestoreRanges(interleavedServers).filter(function (range) {
    return range.mediaServerId === 'node-1';
}).map(function (range) {
    return [range.startTime, range.endTime];
}), [[3000, 3200]], 'another server must not prevent merging a contiguous range');

const chunks = utils.splitRestoreRange({ startTime: 1000, endTime: 1720 }, 300);
assert.deepStrictEqual(chunks.map(function (chunk) {
    return [chunk.startTime, chunk.endTime];
}), [[1000, 1300], [1300, 1600], [1600, 1720]],
'a restore range must be split into 5 minute chunks with a truncated tail');

assert.strictEqual(utils.findChunkIndex(chunks, 1000), 0, 'range start belongs to first chunk');
assert.strictEqual(utils.findChunkIndex(chunks, 1299.9), 0, 'timestamp before boundary remains in current chunk');
assert.strictEqual(utils.findChunkIndex(chunks, 1300), 1, 'chunk end is exclusive');
assert.strictEqual(utils.findChunkIndex(chunks, 1720), -1, 'range end is exclusive');
assert.strictEqual(utils.prefetchThreshold(chunks[0]), 1240, 'prefetch starts at 80 percent');
assert.strictEqual(utils.prefetchThreshold(chunks[2]), 1696, 'tail chunk uses its actual duration');

assert.strictEqual(utils.playbackStatus({ status: 'PENDING' }), 'RESTORING');
assert.strictEqual(utils.playbackStatus({ status: 'RUNNING' }), 'RESTORING');
assert.strictEqual(utils.playbackStatus({ status: 'DONE', playback_ready: true }), 'READY');
assert.strictEqual(utils.playbackStatus({ status: 'DONE', playback_ready: false }), 'NOT_READY');
assert.strictEqual(utils.isReady(snapshot('READY', true)), true);
assert.strictEqual(utils.isReady(snapshot('READY', false)), false,
    'READY status alone must not enable playback');

chunks[1].snapshot = snapshot('RESTORING', false);
assert.strictEqual(utils.boundaryDecision(chunks[0], chunks[1], 1299), 'PLAY');
assert.strictEqual(utils.boundaryDecision(chunks[0], chunks[1], 1300), 'BLOCK');
chunks[1].snapshot = snapshot('READY', true);
assert.strictEqual(utils.boundaryDecision(chunks[0], chunks[1], 1300), 'ADVANCE');
assert.strictEqual(utils.boundaryDecision(chunks[2], null, 1720), 'COMPLETE');

console.log('player restore unit tests passed');
