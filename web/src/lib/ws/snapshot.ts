import { useEffect, useState } from "react";
import { useMd } from "../../stores/md";
import type { QuoteLite } from "./protocol";

/**
 * Latest quotes for ids, sampled every `everyMs` (default 1 s) instead of per tick: panels that rank
 * or colour many instruments (MOV, HM) re-render once a second at most, and only when something
 * changed. Quote objects are replaced, never mutated, so holding them until the next sample is safe.
 */
export function useQuoteSnapshot(ids: readonly number[] | undefined, everyMs = 1_000): Readonly<Record<number, QuoteLite>> {
  const [snap, setSnap] = useState<{ version: number; quotes: Record<number, QuoteLite> }>({ version: -1, quotes: {} });
  useEffect(() => {
    if (!ids || ids.length === 0) return;
    let last = -1;
    const sample = () => {
      const s = useMd.getState();
      if (s.version === last) return;
      last = s.version;
      const quotes: Record<number, QuoteLite> = {};
      for (const id of ids) {
        const q = s.quotes[id];
        if (q) quotes[id] = q;
      }
      setSnap({ version: last, quotes });
    };
    sample();
    const t = setInterval(sample, everyMs);
    return () => clearInterval(t);
  }, [ids, everyMs]);
  return snap.quotes;
}
