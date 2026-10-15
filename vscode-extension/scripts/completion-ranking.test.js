const assert = require('node:assert/strict');
const { test } = require('node:test');
const { preserveCompletionRanking } = require('../out/completionRanking');

test('preserves server order without changing insertion or mutating cached items', () => {
    const input = [
        { label: 'FarObject', sortText: 'heimdall:0000000000', insertText: 'FarObject' },
        { label: { label: 'FooGlobal' }, sortText: 'heimdall:0000000001', filterText: 'FooGlobal' },
    ];
    const ranked = preserveCompletionRanking(input, 'Fo');
    assert.deepEqual(ranked.map(item => item.filterText), ['Fo_FarObject', 'Fo_FooGlobal']);
    assert.equal(ranked[0].insertText, 'FarObject');
    assert.equal(input[0].filterText, undefined);
    assert.equal(input[1].filterText, 'FooGlobal');
});

test('keeps specialized completions and empty lists unchanged', () => {
    assert.equal(preserveCompletionRanking([], 'a'), undefined);
    assert.equal(preserveCompletionRanking([{ label: 'api.hpp', sortText: '000001' }], 'a'), undefined);
});

test('supports empty prefixes and override labels', () => {
    const input = [{ label: 'void Run() override', filterText: 'Run', sortText: 'heimdall:0000000000' }];
    assert.equal(preserveCompletionRanking(input, '')[0].filterText, 'Run');
    assert.equal(preserveCompletionRanking(input, 'R')[0].filterText, 'R_Run');
});
