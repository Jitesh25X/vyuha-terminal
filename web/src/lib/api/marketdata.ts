import { useQuery } from "@tanstack/react-query";
import { request } from "./client";
import type { CandleInterval, Candles, IndexConstituents, Instrument } from "./types";

const SEGMENT = /^(EQ|FUT|OPT|IDX|NSE|BSE|NFO|BFO)$/;

export function searchInstruments(q: string, segment?: string, limit = 20): Promise<Instrument[]> {
  const p = new URLSearchParams({ q, limit: String(limit) });
  if (segment && SEGMENT.test(segment)) p.set("segment", segment);
  return request<Instrument[]>(`/instruments/search?${p}`, { auth: true });
}

/**
 * Resolves the command-bar security ("RELIANCE", "#123", with an optional segment) to one
 * instrument: "#id" directly, otherwise the exact trading symbol, else the best search match.
 */
export async function resolveSecurity(security: string, segment?: string): Promise<Instrument | null> {
  if (security.startsWith("#")) {
    try {
      return await request<Instrument>(`/instruments/${Number(security.slice(1))}`, { auth: true });
    } catch {
      return null;
    }
  }
  const hits = await searchInstruments(security, segment, 10);
  return hits.find((i) => i.tradingsymbol.toUpperCase() === security.toUpperCase()) ?? hits[0] ?? null;
}

export function useInstrument(security: string | undefined, segment: string | undefined) {
  return useQuery({
    queryKey: ["instrument", security?.toUpperCase(), segment],
    queryFn: () => resolveSecurity(security!, segment),
    enabled: !!security,
    staleTime: 10 * 60_000,
  });
}

/** History window per interval: enough to fill a chart without hitting the 50,000-bar cap. */
export const HISTORY_DAYS: Record<CandleInterval, number> = { "1m": 5, "5m": 20, "15m": 45, "1h": 120, "1d": 365 };

export function useCandles(instrumentId: number | undefined, interval: CandleInterval) {
  return useQuery({
    queryKey: ["candles", instrumentId, interval],
    queryFn: () => {
      const to = new Date();
      const from = new Date(to.getTime() - HISTORY_DAYS[interval] * 86_400_000);
      const p = new URLSearchParams({ instrument_id: String(instrumentId), interval, from: from.toISOString(), to: to.toISOString() });
      return request<Candles>(`/marketdata/candles?${p}`, { auth: true });
    },
    enabled: instrumentId !== undefined,
    staleTime: 60_000,
    retry: false,
  });
}

/** Index members (reference data); 404 NO_CONSTITUENTS until a source exists (M6.1). */
export function useIndexConstituents(indexId: number | undefined) {
  return useQuery({
    queryKey: ["constituents", indexId],
    queryFn: () => request<IndexConstituents>(`/indices/${indexId}/constituents`, { auth: true }),
    enabled: indexId !== undefined,
    staleTime: 60 * 60_000,
    retry: false,
  });
}
