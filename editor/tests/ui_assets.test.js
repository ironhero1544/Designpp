// Copyright 2026 The Design++ Authors

import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import test from 'node:test';

test('empty editor overlay honors the hidden state', async () => {
  const styles = await readFile(
      new URL('../src/styles.css', import.meta.url), 'utf8');
  assert.match(styles, /#empty\[hidden\]\s*\{\s*display:\s*none;/u);
});
