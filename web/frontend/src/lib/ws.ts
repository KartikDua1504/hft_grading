import { writable, type Writable } from 'svelte/store';
import { browser } from '$app/environment';

export type WSStatus = 'connecting' | 'connected' | 'disconnected';

export interface WSEvent {
  type: string;
  [key: string]: any;
}

interface WSState {
  status: WSStatus;
  lastEvent: WSEvent | null;
}

export const wsState: Writable<WSState> = writable({
  status: 'disconnected',
  lastEvent: null,
});

// Event subscribers
type EventHandler = (event: WSEvent) => void;
const listeners: Map<string, Set<EventHandler>> = new Map();

let ws: WebSocket | null = null;
let reconnectTimer: ReturnType<typeof setTimeout> | null = null;
let reconnectDelay = 1000;

export function connectWS() {
  if (!browser) return;
  if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) return;

  const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  const url = `${protocol}//${window.location.host}/ws/live`;

  wsState.update(s => ({ ...s, status: 'connecting' }));

  try {
    ws = new WebSocket(url);

    ws.onopen = () => {
      reconnectDelay = 1000;
      wsState.update(s => ({ ...s, status: 'connected' }));
    };

    ws.onmessage = (event) => {
      try {
        const data: WSEvent = JSON.parse(event.data);
        wsState.update(s => ({ ...s, lastEvent: data }));

        // Notify type-specific listeners
        const typeHandlers = listeners.get(data.type);
        if (typeHandlers) {
          typeHandlers.forEach(handler => handler(data));
        }

        // Notify wildcard listeners
        const allHandlers = listeners.get('*');
        if (allHandlers) {
          allHandlers.forEach(handler => handler(data));
        }
      } catch {
        // Ignore parse errors (e.g. ping frames)
      }
    };

    ws.onclose = () => {
      wsState.update(s => ({ ...s, status: 'disconnected' }));
      scheduleReconnect();
    };

    ws.onerror = () => {
      ws?.close();
    };
  } catch {
    wsState.update(s => ({ ...s, status: 'disconnected' }));
    scheduleReconnect();
  }
}

function scheduleReconnect() {
  if (reconnectTimer) clearTimeout(reconnectTimer);
  reconnectTimer = setTimeout(() => {
    reconnectDelay = Math.min(reconnectDelay * 1.5, 15000);
    connectWS();
  }, reconnectDelay);
}

export function disconnectWS() {
  if (reconnectTimer) clearTimeout(reconnectTimer);
  reconnectTimer = null;
  if (ws) {
    ws.onclose = null; // prevent auto-reconnect
    ws.close();
    ws = null;
  }
  wsState.update(s => ({ ...s, status: 'disconnected' }));
}

export function onWSEvent(type: string, handler: EventHandler): () => void {
  if (!listeners.has(type)) {
    listeners.set(type, new Set());
  }
  listeners.get(type)!.add(handler);

  return () => {
    listeners.get(type)?.delete(handler);
  };
}
