// Copyright 2026 The Design++ Authors

import {copyFile, mkdir} from 'node:fs/promises';
import path from 'node:path';
import {build} from 'esbuild';

const outputDirectory = process.argv[2] ?? 'dist';
await mkdir(outputDirectory, {recursive: true});
await build({
  entryPoints: ['src/main.js'],
  bundle: true,
  format: 'esm',
  platform: 'browser',
  target: 'chrome120',
  outfile: path.join(outputDirectory, 'main.js'),
  loader: {'.ttf': 'dataurl'},
  sourcemap: false,
  minify: true,
});
await build({
  entryPoints: [
    'node_modules/monaco-editor/esm/vs/editor/editor.worker.js',
  ],
  bundle: true,
  format: 'esm',
  platform: 'browser',
  target: 'chrome120',
  outfile: path.join(outputDirectory, 'editor.worker.js'),
  sourcemap: false,
  minify: true,
});
await Promise.all([
  copyFile('src/index.html', path.join(outputDirectory, 'index.html')),
  copyFile('src/styles.css', path.join(outputDirectory, 'styles.css')),
  mkdir(path.join(outputDirectory, 'licenses'), {recursive: true}),
]);
await Promise.all([
  copyFile(
      'node_modules/monaco-editor/LICENSE',
      path.join(outputDirectory, 'licenses', 'monaco-editor-LICENSE.txt')),
  copyFile(
      'node_modules/monaco-editor/ThirdPartyNotices.txt',
      path.join(outputDirectory, 'licenses', 'monaco-editor-ThirdPartyNotices.txt')),
  copyFile(
      'node_modules/dompurify/LICENSE',
      path.join(outputDirectory, 'licenses', 'dompurify-LICENSE.txt')),
  copyFile(
      'node_modules/marked/LICENSE.md',
      path.join(outputDirectory, 'licenses', 'marked-LICENSE.md')),
  copyFile(
      'node_modules/esbuild/LICENSE.md',
      path.join(outputDirectory, 'licenses', 'esbuild-LICENSE.md')),
]);
