import { create } from "zustand";
import type { ConnState, DepthLite, QuoteLite, SystemLite } from "../lib/ws/protocol";

// Market data for React (UI_SPEC §7): keyed by instrument id, applied once per animation frame by
// lib/ws/client. Components subscribe with per-id selectors; high-rate consumers (watchlist grid,
// chart, time & sales) use client.ts listeners instead of React state.

interface MdStore {
  /**
   * Mutated in place by lib/ws/client (copying a 1,000-key record every frame was the main source
   * of garbage at 4 Hz × 1,000 rows); `version` changes on every apply so subscribers re-run.
   * Values are replaced, never mutated, so per-id selectors still see a new object on change.
   */
  quotes: Record<number, QuoteLite>;
  depth: Record<number, DepthLite>;
  /** Instruments the gateway refused (MD_NOT_ENTITLED), with its message. */
  denied: Record<number, string>;
  system: SystemLite | undefined;
  conn: ConnState;
  /** epoch ms of the last diff carrying any quote, for STALE tags (mutated in place like quotes) */
  lastQuoteAt: Record<number, number>;
  version: number;
}

export const useMd = create<MdStore>(() => ({
  quotes: {},
  depth: {},
  denied: {},
  system: undefined,
  conn: "closed",
  lastQuoteAt: {},
  version: 0,
}));

export function resetMd(): void {
  useMd.setState({ quotes: {}, depth: {}, denied: {}, system: undefined, conn: "closed", lastQuoteAt: {}, version: 0 });
}

export const selectQuote = (id: number | undefined) => (s: MdStore) => (id === undefined ? undefined : s.quotes[id]);
export const selectDepth = (id: number | undefined) => (s: MdStore) => (id === undefined ? undefined : s.depth[id]);
export const selectDenied = (id: number | undefined) => (s: MdStore) => (id === undefined ? undefined : s.denied[id]);
