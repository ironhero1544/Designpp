// Copyright 2026 The Design++ Authors

export const protocolVersion = 1;

const nativeMessageTypes = new Set([
  'initialize',
  'open_document',
  'close_document',
  'save_result',
  'external_change',
  'set_diagnostics',
  'reveal_location',
  'set_read_only',
  'request_save_all',
  'shutdown',
]);

export function parseNativeMessage(value, expectedSessionId) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return null;
  if (value.protocol !== protocolVersion ||
      !nativeMessageTypes.has(value.type) ||
      typeof value.session_id !== 'string') {
    return null;
  }
  if (expectedSessionId && value.session_id !== expectedSessionId) return null;
  return value;
}

export function postToNative(type, sessionId, payload = {}) {
  window.chrome.webview.postMessage({
    protocol: protocolVersion,
    type,
    session_id: sessionId,
    ...payload,
  });
}
