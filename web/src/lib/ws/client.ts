import { useEffect } from "react";
import { resetMd, useMd } from "../../stores/md";
import { accessToken, useSession } from "../../stores/session";
import { refreshSession } from "../api/client";
import { histogram, percentile, Ring } from "./latency";
import type { BarLite, Channel, Diff, FromWorker, PrintLite, QuoteLite, ToWorker } from "./protocol";

// Main-thread side of the market-data worker: reference-counted subscriptions, diff application to
// the md store, and listeners for consumers that must not go through React state (grids, charts).

/** UI_SPEC §7: hidden/closed panels give up their subscription after 10 s (a re-open within that is free). */
const RELEASE_AFTER_MS = 10_000;
export const DEFAULT_CONFLATE_MS = 250;

/** Dev/e2e only: localStorage key overriding the WebSocket URL (the perf test points it at a fake feed). */
export const DEV_WS_URL_KEY = "vyuha.dev.wsUrl";

type Listener<T> = (items: T[]) => void;

let worker: Worker | undefined;
const refs = new Map<string, { count: number; timer?: ReturnType<typeof setTimeout> }>();
const quoteListeners = new Set<Listener<QuoteLite>>();
const barListeners = new Set<Listener<BarLite>>();
const printListeners = new Set<Listener<PrintLite>>();
let unsubscribeSession: (() => void) | undefined;

function post(m: ToWorker): void {
  worker?.postMessage(m);
}

function wsUrl(): string {
  if (import.meta.env.DEV) {
    try {
      const override = localStorage.getItem(DEV_WS_URL_KEY);
      if (override) return override;
    } catch {
      // storage unavailable: use the default
    }
  }
  return `${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/ws`;
}

/** Starts the worker for the signed-in user (Terminal mounts it). Idempotent. */
export function startMarketData(): void {
  const token = accessToken();
  if (worker || !token) return;
  worker = new Worker(new URL("./worker.ts", import.meta.url), { type: "module", name: "vyuha-md" });
  worker.onmessage = (e: MessageEvent<FromWorker>) => apply(e.data.diff);
  post({ type: "start", url: wsUrl(), token });
  let last = token;
  unsubscribeSession = useSession.subscribe((s) => {
    if (s.state.status === "authenticated" && s.state.accessToken !== last) {
      last = s.state.accessToken;
      post({ type: "token", token: last });
    }
  });
  // Re-subscribe anything panels asked for before the worker existed.
  for (const [key, r] of refs) if (r.count > 0 && r.timer === undefined) postSub(key);
}

export function stopMarketData(): void {
  post({ type: "stop" });
  worker?.terminate();
  worker = undefined;
  unsubscribeSession?.();
  unsubscribeSession = undefined;
  for (const r of refs.values()) clearTimeout(r.timer);
  refs.clear();
  resetLatency();
  clock = undefined;
  resetMd();
}

function keyOf(channel: Channel, id: number, conflateMs: number): string {
  return `${channel}|${id}|${conflateMs}`;
}

function postSub(key: string): void {
  const [channel, id, conflate] = key.split("|") as [Channel, string, string];
  post({ type: "sub", channel, ids: channel === "system" ? [] : [Number(id)], conflateMs: Number(conflate) });
}

/**
 * Subscribes to ids on a channel; returns the release function. Subscriptions are shared: the
 * worker hears about an id once, and drops it 10 s after the last holder releases it.
 */
export function subscribe(channel: Channel, ids: readonly number[], conflateMs = DEFAULT_CONFLATE_MS): () => void {
  const fresh: number[] = [];
  const keys = ids.map((id) => keyOf(channel, id, conflateMs));
  keys.forEach((key, i) => {
    const r = refs.get(key);
    if (r) {
      r.count++;
      if (r.timer !== undefined) {
        clearTimeout(r.timer);
        delete r.timer;
      }
    } else {
      refs.set(key, { count: 1 });
      fresh.push(ids[i]!);
    }
  });
  if (fresh.length > 0 || channel === "system") post({ type: "sub", channel, ids: fresh, conflateMs });
  let released = false;
  return () => {
    if (released) return;
    released = true;
    for (const key of keys) {
      const r = refs.get(key);
      if (!r || --r.count > 0) continue;
      r.timer = setTimeout(() => {
        refs.delete(key);
        const [, id] = key.split("|");
        post({ type: "unsub", channel, ids: channel === "system" ? [] : [Number(id)] });
      }, RELEASE_AFTER_MS);
    }
  };
}

/** React helper: subscribe while mounted. ids must be stable (memoized) to avoid churn. */
export function useSubscription(channel: Channel, ids: readonly number[] | undefined, conflateMs = DEFAULT_CONFLATE_MS): void {
  useEffect(() => (ids && ids.length > 0) || channel === "system" ? subscribe(channel, ids ?? [], conflateMs) : undefined, [channel, ids, conflateMs]);
}

export function onQuotes(fn: Listener<QuoteLite>): () => void {
  quoteListeners.add(fn);
  return () => quoteListeners.delete(fn);
}
export function onBars(fn: Listener<BarLite>): () => void {
  barListeners.add(fn);
  return () => barListeners.delete(fn);
}
export function onPrints(fn: Listener<PrintLite>): () => void {
  printListeners.add(fn);
  return () => printListeners.delete(fn);
}

// ── tick → screen latency (SYS panel; M1.12 done check) ─────────────────────
const LATENCY_SAMPLES = 20_000;
const tickToScreen = new Ring(LATENCY_SAMPLES);
const exchangeToScreen = new Ring(LATENCY_SAMPLES);
let clock: { offsetMs: number; rttMs: number } | undefined;

export interface LatencyStats {
  samples: number;
  p50?: number;
  p95?: number;
  p99?: number;
  histogram: number[];
  exchangeP50?: number;
  exchangeP95?: number;
  clockOffsetMs?: number;
  clockRttMs?: number;
}

/** Latency of the last ≤ 20,000 quotes painted, in ms (engine receive → paint; exchange → paint). */
export function latencyStats(): LatencyStats {
  const v = tickToScreen.values();
  const x = exchangeToScreen.values();
  const s: LatencyStats = { samples: v.length, histogram: histogram(v) };
  const set = <K extends keyof LatencyStats>(k: K, val: LatencyStats[K] | undefined) => {
    if (val !== undefined) s[k] = val;
  };
  set("p50", percentile(v, 50));
  set("p95", percentile(v, 95));
  set("p99", percentile(v, 99));
  set("exchangeP50", percentile(x, 50));
  set("exchangeP95", percentile(x, 95));
  set("clockOffsetMs", clock?.offsetMs);
  set("clockRttMs", clock?.rttMs);
  return s;
}

export function resetLatency(): void {
  tickToScreen.clear();
  exchangeToScreen.clear();
}

/** Samples the quotes of a diff in the next animation frame, i.e. when they are painted. */
function sampleLatency(quotes: QuoteLite[]): void {
  if (!clock) return; // no clock estimate yet: server and browser times aren't comparable
  const offset = clock.offsetMs;
  requestAnimationFrame(() => {
    const paint = Date.now() + offset; // on the server's clock
    for (const q of quotes) {
      if (q.recvTs > 0) tickToScreen.push(paint - q.recvTs);
      if (q.exchangeTs > 0) exchangeToScreen.push(paint - q.exchangeTs);
    }
  });
}

function apply(d: Diff): void {
  if (d.authExpired) void reauthenticate();
  if (d.clock) clock = d.clock;
  const patch: Partial<ReturnType<typeof useMd.getState>> = {};
  const s = useMd.getState();
  // quotes/depth/lastQuoteAt are updated in place (see stores/md); version triggers subscribers.
  if (d.quotes.length > 0) {
    const now = Date.now();
    for (const q of d.quotes) {
      s.quotes[q.id] = q;
      s.lastQuoteAt[q.id] = now;
    }
    patch.version = s.version + 1;
  }
  if (d.depth.length > 0) {
    for (const x of d.depth) s.depth[x.id] = x;
    patch.version = s.version + 1;
  }
  const denied = d.errors.filter((e) => e.code === "MD_NOT_ENTITLED");
  if (denied.length > 0) {
    const map = { ...s.denied };
    for (const e of denied) for (const id of e.ids) map[id] = e.message.replace(/\s*\[[\d,]+\]\s*$/, "");
    patch.denied = map;
  }
  for (const e of d.errors) if (e.code !== "MD_NOT_ENTITLED") console.warn(`market data: ${e.code}: ${e.message}`);
  if (d.system) patch.system = d.system;
  if (d.conn) patch.conn = d.conn;
  if (Object.keys(patch).length > 0) useMd.setState(patch);
  if (d.quotes.length > 0) {
    for (const fn of quoteListeners) fn(d.quotes);
    sampleLatency(d.quotes);
  }
  if (d.bars.length > 0) for (const fn of barListeners) fn(d.bars);
  if (d.prints.length > 0) for (const fn of printListeners) fn(d.prints);
}

async function reauthenticate(): Promise<void> {
  const s = await refreshSession();
  const token = accessToken();
  if (s && token && worker) post({ type: "start", url: wsUrl(), token });
}
