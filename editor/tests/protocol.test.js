// Copyright 2026 The Design++ Authors

import test from 'node:test';
import assert from 'node:assert/strict';

import {parseNativeMessage, protocolVersion} from '../src/protocol.js';
import {hdlKeywords} from '../src/hdl_language.js';

test('protocol rejects an unknown type and mismatched session', () => {
  assert.equal(parseNativeMessage({
    protocol: protocolVersion,
    type: 'execute_script',
    session_id: 'one',
  }, 'one'), null);
  assert.equal(parseNativeMessage({
    protocol: protocolVersion,
    type: 'initialize',
    session_id: 'two',
  }, 'one'), null);
});

test('protocol accepts a valid native message', () => {
  const message = parseNativeMessage({
    protocol: protocolVersion,
    type: 'open_document',
    session_id: 'one',
  }, 'one');
  assert.equal(message.type, 'open_document');
});

test('HDL tokenizer keyword set contains structural SystemVerilog tokens', () => {
  for (const keyword of ['module', 'interface', 'package', 'class', 'always_ff']) {
    assert.ok(hdlKeywords.includes(keyword));
  }
});
