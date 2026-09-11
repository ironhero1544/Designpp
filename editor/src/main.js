// Copyright 2026 The Design++ Authors

import * as monaco from 'monaco-editor';
import {registerHdlLanguages} from './hdl_language.js';
import {parseNativeMessage, postToNative} from './protocol.js';

self.MonacoEnvironment = {
  getWorker() {
    return new Worker(new URL('./editor.worker.js', import.meta.url), {
      type: 'module',
    });
  },
};

registerHdlLanguages(monaco);

const documents = new Map();
const tabs = document.querySelector('#tabs');
const editorElement = document.querySelector('#editor');
const emptyElement = document.querySelector('#empty');
let sessionId = '';
let activeDocumentId = '';
let shuttingDown = false;
let initialized = false;

const editor = monaco.editor.create(editorElement, {
  automaticLayout: true,
  theme: 'vs-dark',
  minimap: {enabled: true},
  scrollBeyondLastLine: false,
  fixedOverflowWidgets: true,
});

function renderTabs() {
  tabs.replaceChildren();
  for (const state of documents.values()) {
    const tab = document.createElement('button');
    tab.className = `tab${state.id === activeDocumentId ? ' active' : ''}`;
    tab.type = 'button';
    tab.dataset.documentId = state.id;
    tab.setAttribute('role', 'tab');
    tab.setAttribute('aria-selected', String(state.id === activeDocumentId));
    const title = document.createElement('span');
    title.className = 'tab-title';
    title.textContent = `${state.dirty ? '● ' : ''}${state.name}`;
    const close = document.createElement('button');
    close.className = 'tab-close';
    close.type = 'button';
    close.textContent = '×';
    close.title = `Close ${state.name}`;
    close.addEventListener('click', (event) => {
      event.stopPropagation();
      postToNative('close_document_requested', sessionId, {
        document_id: state.id,
        dirty: state.dirty,
      });
    });
    tab.addEventListener('click', () => activateDocument(state.id));
    tab.append(title, close);
    tabs.append(tab);
  }
}

function activateDocument(documentId) {
  const state = documents.get(documentId);
  if (!state) return;
  if (activeDocumentId && activeDocumentId !== documentId) {
    const previous = documents.get(activeDocumentId);
    if (previous) previous.viewState = editor.saveViewState();
  }
  activeDocumentId = documentId;
  editor.setModel(state.model);
  editor.updateOptions({readOnly: state.readOnly});
  if (state.viewState) editor.restoreViewState(state.viewState);
  editor.focus();
  emptyElement.hidden = true;
  editorElement.hidden = false;
  renderTabs();
  postToNative('active_document_changed', sessionId, {
    document_id: state.id,
  });
}

function openDocument(message) {
  if (documents.has(message.document_id)) {
    activateDocument(message.document_id);
    return;
  }
  const model = monaco.editor.createModel(
      message.text, message.language,
      monaco.Uri.parse(message.model_uri));
  const state = {
    id: message.document_id,
    name: message.display_name,
    model,
    readOnly: Boolean(message.read_only),
    dirty: false,
    version: Number(message.version) || 1,
    viewState: null,
    changeSubscription: null,
  };
  state.changeSubscription = model.onDidChangeContent(() => {
    state.dirty = true;
    state.version = model.getVersionId();
    renderTabs();
    postToNative('document_changed', sessionId, {
      document_id: state.id,
      version: state.version,
    });
  });
  documents.set(state.id, state);
  activateDocument(state.id);
}

function closeDocument(documentId) {
  const state = documents.get(documentId);
  if (!state) return;
  state.changeSubscription.dispose();
  state.model.dispose();
  documents.delete(documentId);
  if (activeDocumentId === documentId) {
    activeDocumentId = documents.keys().next().value ?? '';
    if (activeDocumentId) {
      activateDocument(activeDocumentId);
    } else {
      editor.setModel(null);
      editorElement.hidden = true;
      emptyElement.hidden = false;
      postToNative('active_document_changed', sessionId, {document_id: ''});
    }
  }
  renderTabs();
}

function requestSave(documentId) {
  const state = documents.get(documentId);
  if (!state || state.readOnly) return;
  postToNative('save_document', sessionId, {
    document_id: state.id,
    version: state.version,
    text: state.model.getValue(),
  });
}

editor.addCommand(monaco.KeyMod.CtrlCmd | monaco.KeyCode.KeyS, () => {
  if (activeDocumentId) requestSave(activeDocumentId);
});
editor.addCommand(
    monaco.KeyMod.CtrlCmd | monaco.KeyMod.Shift | monaco.KeyCode.KeyS,
    () => postToNative('editor_command', sessionId, {command: 'save_all'}));

window.chrome.webview.addEventListener('message', (event) => {
  if (shuttingDown) return;
  const message = parseNativeMessage(event.data, sessionId);
  if (!message) return;
  if (message.type === 'initialize') {
    if (initialized) return;
    sessionId = message.session_id;
    initialized = true;
    monaco.editor.setTheme(message.theme === 'light' ? 'vs' : 'vs-dark');
    editor.updateOptions({
      fontFamily: message.font_family ?? 'Cascadia Mono, Consolas, monospace',
      fontSize: Number(message.font_size) || 14,
    });
    postToNative('ready', sessionId);
  } else if (message.type === 'open_document') {
    openDocument(message);
  } else if (message.type === 'close_document') {
    closeDocument(message.document_id);
  } else if (message.type === 'save_result') {
    const state = documents.get(message.document_id);
    if (state && message.succeeded) {
      state.dirty = false;
      renderTabs();
    }
  } else if (message.type === 'external_change') {
    const state = documents.get(message.document_id);
    if (state && !state.dirty && typeof message.text === 'string') {
      state.model.setValue(message.text);
      state.dirty = false;
      renderTabs();
    }
  } else if (message.type === 'set_diagnostics') {
    for (const state of documents.values()) {
      const markers = (message.diagnostics ?? [])
          .filter((diagnostic) => diagnostic.document_id === state.id)
          .map((diagnostic) => ({
            severity: diagnostic.severity === 'error' ?
                monaco.MarkerSeverity.Error : monaco.MarkerSeverity.Warning,
            code: diagnostic.code,
            message: diagnostic.message,
            startLineNumber: Math.max(1, diagnostic.line),
            startColumn: Math.max(1, diagnostic.column),
            endLineNumber: Math.max(1, diagnostic.line),
            endColumn: Math.max(2, diagnostic.column + 1),
          }));
      monaco.editor.setModelMarkers(state.model, 'verilator', markers);
    }
  } else if (message.type === 'reveal_location') {
    activateDocument(message.document_id);
    editor.setPosition({lineNumber: message.line, column: message.column});
    editor.revealPositionInCenter({
      lineNumber: message.line,
      column: message.column,
    });
  } else if (message.type === 'set_read_only') {
    const state = documents.get(message.document_id);
    if (state) state.readOnly = Boolean(message.read_only);
    if (message.document_id === activeDocumentId) {
      editor.updateOptions({readOnly: Boolean(message.read_only)});
    }
  } else if (message.type === 'replace_document_text') {
    const state = documents.get(message.document_id);
    if (state && !state.readOnly && typeof message.text === 'string') {
      state.model.setValue(message.text);
      state.dirty = true;
      state.version = state.model.getVersionId();
      renderTabs();
    }
  } else if (message.type === 'request_document_text') {
    const state = documents.get(message.document_id);
    if (state) {
      postToNative('document_text', sessionId, {
        document_id: state.id,
        version: state.version,
        text: state.model.getValue(),
        dirty: state.dirty,
      });
    }
  } else if (message.type === 'request_save_all') {
    for (const state of documents.values()) {
      if (state.dirty) requestSave(state.id);
    }
  } else if (message.type === 'shutdown') {
    shuttingDown = true;
    for (const state of documents.values()) {
      state.changeSubscription.dispose();
      state.model.dispose();
    }
    documents.clear();
    editor.dispose();
  }
});

editorElement.hidden = true;
postToNative('ready_for_initialize', '', {});
