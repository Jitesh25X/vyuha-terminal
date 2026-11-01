// Index movers and heatmap logic for MOV and HM (pure; tested in movers.test.ts). Constituents are
// reference data from GET /indices/{id}/constituents; prices come from the quote stream.
import type { Constituent } from "./api/types";
import type { QuoteLite } from "./ws/protocol";

export interface MoverRow {
  id: number;
  sym: string;
  sector: string;
  /** index weight, percent (undefined when the source has none) */
  weight: number | undefined;
  ltp: number | undefined; // price units
  changePct: number | undefined;
  volume: number | undefined;
  /** today's volume ÷ 20-day average daily volume */
  volRatio: number | undefined;
  /** near-month future OI change since the previous close, percent */
  oiChgPct: number | undefined;
}

export const UNKNOWN_SECTOR = "Unclassified";

export function buildRows(constituents: readonly Constituent[], quotes: Readonly<Record<number, QuoteLite | undefined>>): MoverRow[] {
  return constituents.map((c) => {
    const q = quotes[c.instrument_id];
    const fut = c.future_id === undefined ? undefined : quotes[c.future_id];
    const prevOi = c.future_prev_oi;
    return {
      id: c.instrument_id,
      sym: c.tradingsymbol,
      sector: c.sector ?? UNKNOWN_SECTOR,
      weight: c.weight,
      ltp: q?.ltp,
      changePct: q && q.prevClose > 0 ? q.changePct : undefined,
      volume: q?.volume,
      volRatio: q && c.avg_volume_20d ? q.volume / c.avg_volume_20d : undefined,
      oiChgPct: fut && fut.oi > 0 && prevOi ? ((fut.oi - prevOi) / prevOi) * 100 : undefined,
    };
  });
}

function top(rows: readonly MoverRow[], key: (r: MoverRow) => number | undefined, n: number, keep: (v: number) => boolean): MoverRow[] {
  return rows
    .filter((r) => {
      const v = key(r);
      return v !== undefined && Number.isFinite(v) && keep(v);
    })
    .sort((a, b) => key(b)! - key(a)! || a.sym.localeCompare(b.sym))
    .slice(0, n);
}

/** Biggest % gainers (change > 0), best first. */
export const gainers = (rows: readonly MoverRow[], n: number) => top(rows, (r) => r.changePct, n, (v) => v > 0);
/** Biggest % losers (change < 0), worst first. */
export const losers = (rows: readonly MoverRow[], n: number) => top(rows, (r) => (r.changePct === undefined ? undefined : -r.changePct), n, (v) => v > 0);
/** Highest volume vs the 20-day average (needs the baseline). */
export const volumeShockers = (rows: readonly MoverRow[], n: number) => top(rows, (r) => r.volRatio, n, () => true);
/** Biggest rise in near-month future OI (needs the previous close OI). */
export const oiGainers = (rows: readonly MoverRow[], n: number) => top(rows, (r) => r.oiChgPct, n, (v) => v > 0);

export interface SectorSummary {
  sector: string;
  count: number;
  /** summed index weight, percent (undefined when no member has one) */
  weight: number | undefined;
  /** weight-averaged % change of members with a price (equal weights when weights are missing) */
  changePct: number | undefined;
}

/** Per-sector roll-up, heaviest sector first. */
export function sectorSummary(rows: readonly MoverRow[]): SectorSummary[] {
  const by = new Map<string, MoverRow[]>();
  for (const r of rows) by.set(r.sector, [...(by.get(r.sector) ?? []), r]);
  const out: SectorSummary[] = [];
  for (const [sector, members] of by) {
    const weighted = members.every((m) => m.weight !== undefined);
    let w = 0;
    let sum = 0;
    for (const m of members) {
      if (m.changePct === undefined) continue;
      const mw = weighted ? m.weight! : 1;
      w += mw;
      sum += mw * m.changePct;
    }
    out.push({
      sector,
      count: members.length,
      weight: weighted ? members.reduce((a, m) => a + m.weight!, 0) : undefined,
      changePct: w > 0 ? sum / w : undefined,
    });
  }
  return out.sort((a, b) => (b.weight ?? b.count) - (a.weight ?? a.count) || a.sector.localeCompare(b.sector));
}

/**
 * Diverging colour for a % change: `down` at −limit, `neutral` at 0, `up` at +limit (linear in RGB,
 * clamped). Undefined change → neutral. Colours are #rrggbb.
 */
export function heatColor(changePct: number | undefined, up: string, down: string, neutral: string, limit = 3): string {
  if (changePct === undefined || !Number.isFinite(changePct) || changePct === 0) return neutral;
  const t = Math.min(Math.abs(changePct) / limit, 1);
  const to = changePct > 0 ? up : down;
  const rgb = (hex: string) => [1, 3, 5].map((i) => parseInt(hex.slice(i, i + 2), 16));
  const [a, b] = [rgb(neutral), rgb(to)];
  return `#${a.map((v, i) => Math.round(v + (b[i]! - v) * t).toString(16).padStart(2, "0")).join("")}`;
}

/** Black or white label ink for a #rrggbb fill (WCAG relative luminance, threshold 0.18). */
export function inkOn(fill: string): "#000000" | "#ffffff" {
  const lin = [1, 3, 5].map((i) => {
    const c = parseInt(fill.slice(i, i + 2), 16) / 255;
    return c <= 0.03928 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
  });
  const l = 0.2126 * lin[0]! + 0.7152 * lin[1]! + 0.0722 * lin[2]!;
  return l > 0.18 ? "#000000" : "#ffffff";
}
