// Number formatting (UI_SPEC §6): Indian digit grouping, tabular figures via CSS. Prices arrive as
// int64 units (₹1 = 10,000) and are converted only here, at render.

export const UNITS_PER_RUPEE = 10_000;

const price2 = new Intl.NumberFormat("en-IN", { minimumFractionDigits: 2, maximumFractionDigits: 2 });
const price4 = new Intl.NumberFormat("en-IN", { minimumFractionDigits: 2, maximumFractionDigits: 4 });
const int = new Intl.NumberFormat("en-IN", { maximumFractionDigits: 0 });
const pct = new Intl.NumberFormat("en-IN", { minimumFractionDigits: 2, maximumFractionDigits: 2, signDisplay: "exceptZero" });

/** "2,450.50" — two decimals, or up to four when the price needs them (e.g. 0.0025 tick contracts). */
export function formatPrice(units: number | undefined): string {
  if (units === undefined || !Number.isFinite(units)) return "—";
  return (units % 100 === 0 ? price2 : price4).format(units / UNITS_PER_RUPEE);
}

/** "₹12,34,567.50" */
export function formatRupees(units: number | undefined): string {
  const s = formatPrice(units);
  if (s === "—") return s;
  return s.startsWith("-") ? `-₹${s.slice(1)}` : `₹${s}`;
}

/** "+1.25%" / "-0.40%" / "0.00%" */
export function formatPct(v: number | undefined): string {
  return v === undefined || !Number.isFinite(v) ? "—" : `${pct.format(v)}%`;
}

/** "12,34,567" */
export function formatQty(v: number | undefined): string {
  return v === undefined || !Number.isFinite(v) ? "—" : int.format(v);
}

/** Compact volume in Indian units: "9.87 Cr", "4.50 L", "12.3 K", "950". */
export function formatVolume(v: number | undefined): string {
  if (v === undefined || !Number.isFinite(v)) return "—";
  const a = Math.abs(v);
  if (a >= 1e7) return `${(v / 1e7).toFixed(2)} Cr`;
  if (a >= 1e5) return `${(v / 1e5).toFixed(2)} L`;
  if (a >= 1e4) return `${(v / 1e3).toFixed(1)} K`;
  return int.format(v);
}

/** Sign class for coloured numbers: up / down / flat. */
export function direction(v: number | undefined): "up" | "down" | "flat" {
  if (v === undefined || v === 0 || !Number.isFinite(v)) return "flat";
  return v > 0 ? "up" : "down";
}

/** Units → rupees as a float, for charts only (display; never for money arithmetic). */
export function toChartPrice(units: number): number {
  return units / UNITS_PER_RUPEE;
}
