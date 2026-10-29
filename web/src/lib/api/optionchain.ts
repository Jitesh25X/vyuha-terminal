import { keepPreviousData, useQuery } from "@tanstack/react-query";
import { request } from "./client";
import type { OptionChain } from "./types";

export function useExpiries(underlyingId: number | undefined) {
  return useQuery({
    queryKey: ["expiries", underlyingId],
    queryFn: () => request<string[]>(`/instruments/${underlyingId}/expiries`, { auth: true }),
    enabled: underlyingId !== undefined,
    staleTime: 10 * 60_000,
  });
}

/**
 * Polls the chain (IV/Greeks are computed server-side per poll): every second while the market is
 * open, every 15 s otherwise. Keeps showing the previous chain while the next one loads.
 */
export function useOptionChain(underlyingId: number | undefined, expiry: string | undefined, strikes: number, live: boolean) {
  return useQuery({
    queryKey: ["optionchain", underlyingId, expiry, strikes],
    queryFn: () => {
      const p = new URLSearchParams({ strikes: String(strikes) });
      if (expiry) p.set("expiry", expiry);
      return request<OptionChain>(`/optionchain/${underlyingId}?${p}`, { auth: true });
    },
    enabled: underlyingId !== undefined && expiry !== undefined,
    refetchInterval: live ? 1_000 : 15_000,
    placeholderData: keepPreviousData,
    retry: false,
  });
}
