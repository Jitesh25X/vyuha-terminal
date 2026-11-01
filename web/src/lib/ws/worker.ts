/// <reference lib="webworker" />
import { create, type MessageInitShape, toBinary } from "@bufbuild/protobuf";
import { ClientMsgSchema } from "../../gen/vyuha/ws/v1/ws_pb";
import { channelEnum, Merger } from "./decode";
import { bestSample, type ClockSample, clockSample } from "./latency";
import type { Channel, ConnState, Diff, FromWorker, ToWorker } from "./protocol";

// Market-data worker (UI_SPEC §7): owns the WebSocket, decodes protobuf off the main thread, merges
// to the latest value per key and posts one diff per animation frame.

declare const self: DedicatedWorkerGlobalScope;

const BACKOFF_MS = [500, 1000, 2000, 5000, 10_000];
/** Clock-offset probes (Ping/Pong) while connected; the best of the last few is used. */
const PING_EVERY_MS = 5000;
const CLOCK_SAMPLES = 12;

let url = "";
let token = "";
let socket: WebSocket | undefined;
let attempt = 0;
let reconnectTimer: ReturnType<typeof setTimeout> | undefined;
let reqId = 0;
const merger = new Merger();
let conn: ConnState | undefined;
let authExpired = false;
let flushScheduled = false;
let pingTimer: ReturnType<typeof setInterval> | undefined;
let clockSamples: ClockSample[] = [];
let clockChanged = false;

/** Desired subscriptions: channel → id → conflate_ms. Replayed in full after every reconnect. */
const desired = new Map<Channel, Map<number, number>>();
/** The system channel has no keys, so it is tracked separately. */
let wantSystem = false;

function send(body: MessageInitShape<typeof ClientMsgSchema>): void {
  if (socket?.readyState !== WebSocket.OPEN) return;
  socket.send(toBinary(ClientMsgSchema, create(ClientMsgSchema, { reqId: ++reqId, ...body })));
}

function sendSubscribe(channel: Channel, ids: number[], conflateMs: number): void {
  // keys are strings on the wire; system takes none.
  send({ body: { case: "subscribe", value: { channel: channelEnum(channel), keys: ids.map(String), conflateMs } } });
}

function resubscribeAll(): void {
  if (wantSystem) sendSubscribe("system", [], 0);
  for (const [channel, ids] of desired) {
    const byConflate = new Map<number, number[]>();
    for (const [id, c] of ids) byConflate.set(c, [...(byConflate.get(c) ?? []), id]);
    for (const [c, list] of byConflate) sendSubscribe(channel, list, c);
  }
}

function setConn(c: ConnState): void {
  conn = c;
  schedule();
}

function connect(): void {
  clearTimeout(reconnectTimer);
  if (!url || !token) return;
  setConn("connecting");
  const ws = new WebSocket(url);
  ws.binaryType = "arraybuffer";
  socket = ws;
  ws.onopen = () => {
    attempt = 0;
    ws.send(toBinary(ClientMsgSchema, create(ClientMsgSchema, { reqId: ++reqId, body: { case: "hello", value: { accessToken: token, clientVersion: "web" } } })));
    resubscribeAll();
    setConn("open");
    const ping = () => send({ body: { case: "ping", value: { clientTsMs: BigInt(Date.now()) } } });
    clearInterval(pingTimer);
    clockSamples = []; // a new connection may land on another path
    ping();
    pingTimer = setInterval(ping, PING_EVERY_MS);
  };
  ws.onmessage = (e: MessageEvent<ArrayBuffer>) => {
    if (!(e.data instanceof ArrayBuffer) || !merger.push(new Uint8Array(e.data))) return;
    const pongs = merger.takePongs();
    if (pongs.length > 0) {
      clockSamples = [...clockSamples, ...pongs.map((p) => clockSample(p.clientMs, p.serverMs, p.recvMs))].slice(-CLOCK_SAMPLES);
      clockChanged = true;
    }
    schedule();
  };
  ws.onclose = (e) => {
    if (socket !== ws) return; // replaced by a newer connection
    socket = undefined;
    clearInterval(pingTimer);
    setConn("closed");
    if (e.code === 4401) {
      // Token expired or rejected: the main thread refreshes and sends "start" again.
      authExpired = true;
      schedule();
      return;
    }
    const delay = BACKOFF_MS[Math.min(attempt++, BACKOFF_MS.length - 1)]!;
    reconnectTimer = setTimeout(connect, delay * (0.5 + Math.random() / 2));
  };
}

function schedule(): void {
  if (flushScheduled) return;
  flushScheduled = true;
  // Dedicated workers have requestAnimationFrame in Chromium and Firefox; fall back to ~60 Hz.
  if (typeof self.requestAnimationFrame === "function") self.requestAnimationFrame(flush);
  else setTimeout(flush, 16);
}

function flush(): void {
  flushScheduled = false;
  const diff: Diff = merger.take();
  if (conn !== undefined) diff.conn = conn;
  if (authExpired) diff.authExpired = true;
  const clock = clockChanged ? bestSample(clockSamples) : undefined;
  if (clock) diff.clock = clock;
  clockChanged = false;
  conn = undefined;
  authExpired = false;
  const msg: FromWorker = { type: "diff", diff };
  self.postMessage(msg);
}

self.onmessage = (e: MessageEvent<ToWorker>) => {
  const m = e.data;
  switch (m.type) {
    case "start":
      url = m.url;
      token = m.token;
      socket?.close(1000);
      socket = undefined;
      connect();
      break;
    case "token":
      token = m.token;
      send({ body: { case: "tokenRefresh", value: { accessToken: m.token } } });
      break;
    case "sub": {
      if (m.channel === "system") {
        wantSystem = true;
        sendSubscribe("system", [], 0);
        break;
      }
      let ids = desired.get(m.channel);
      if (!ids) desired.set(m.channel, (ids = new Map()));
      for (const id of m.ids) ids.set(id, m.conflateMs);
      sendSubscribe(m.channel, m.ids, m.conflateMs);
      break;
    }
    case "unsub": {
      if (m.channel === "system") {
        wantSystem = false;
        send({ body: { case: "unsubscribe", value: { channel: channelEnum("system"), keys: [] } } });
        break;
      }
      const ids = desired.get(m.channel);
      for (const id of m.ids) ids?.delete(id);
      merger.forget(m.ids);
      send({ body: { case: "unsubscribe", value: { channel: channelEnum(m.channel), keys: m.ids.map(String) } } });
      break;
    }
    case "stop":
      url = token = "";
      clearInterval(pingTimer);
      desired.clear();
      wantSystem = false;
      clearTimeout(reconnectTimer);
      socket?.close(1000);
      socket = undefined;
      break;
  }
};
