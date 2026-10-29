// PCR history for the OI panel: one point per chain poll, kept in memory for this browser session
// (per underlying + expiry) so it survives closing and reopening the panel. Nothing is persisted yet:
// the history moves to ClickHouse when the quant risk worker lands (M5.2), and this becomes a query.

const MAX_POINTS = 2_000; // ~33 minutes at 1 Hz; older points are dropped first

const series = new Map<string, [number, number][]>();

/** Appends the chain's PCR (if any) at its as_of time and returns the series (oldest first). */
export function recordPcr(key: string, asOf: string, pcr: number | undefined): readonly [number, number][] {
  let s = series.get(key);
  if (!s) {
    s = [];
    series.set(key, s);
  }
  const t = Date.parse(asOf);
  const last = s[s.length - 1];
  if (pcr !== undefined && Number.isFinite(t) && (!last || t > last[0])) {
    s.push([t, pcr]);
    if (s.length > MAX_POINTS) s.splice(0, s.length - MAX_POINTS);
  }
  return s;
}

export function resetPcrHistory(): void {
  series.clear();
}
