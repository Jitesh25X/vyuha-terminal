// Option-chain helpers shared by OC and OI (pure logic; tested in optionchain.test.ts).
import type { OptionChain, OptionSide } from "./api/types";

/**
 * Decimal string from the REST API ("180.5000") → int64 price units (₹1 = 10,000), parsed digit by
 * digit so prices never pass through a binary float. undefined for absent/malformed input.
 */
export function decimalUnits(s: string | undefined): number | undefined {
  if (s === undefined) return undefined;
  const m = /^(-?)(\d+)(?:\.(\d{1,4})\d*)?$/.exec(s);
  if (!m) return undefined;
  const units = Number(m[2]) * 10_000 + Number((m[3] ?? "").padEnd(4, "0"));
  return m[1] ? -units : units;
}

/** Price of the underlying used for moneyness: spot, else the forward/future. */
export function referencePrice(c: OptionChain): number | undefined {
  const spot = decimalUnits(c.underlying.ltp);
  if (spot !== undefined) return spot / 10_000;
  if (c.forward !== undefined) return c.forward;
  const fut = decimalUnits(c.underlying.fut_ltp);
  return fut === undefined ? undefined : fut / 10_000;
}

/** Calls are in the money below the reference price, puts above. */
export function isItm(side: "ce" | "pe", strike: string, ref: number | undefined): boolean {
  const k = decimalUnits(strike);
  if (ref === undefined || k === undefined) return false;
  return side === "ce" ? k / 10_000 < ref : k / 10_000 > ref;
}

export type Buildup = "Long build-up" | "Short build-up" | "Short covering" | "Long unwinding";

/**
 * Standard F&O build-up from today's price change (LTP − previous close) and OI change:
 * price ↑ OI ↑ long build-up · price ↓ OI ↑ short build-up · price ↑ OI ↓ short covering ·
 * price ↓ OI ↓ long unwinding. undefined when either is missing or flat.
 */
export function classifyBuildup(s: OptionSide | undefined): Buildup | undefined {
  const ltp = decimalUnits(s?.ltp);
  const prev = decimalUnits(s?.prev_close);
  const oiChg = s?.oi_chg;
  if (ltp === undefined || prev === undefined || oiChg === undefined || ltp === prev || oiChg === 0) return undefined;
  const up = ltp > prev;
  if (oiChg > 0) return up ? "Long build-up" : "Short build-up";
  return up ? "Short covering" : "Long unwinding";
}

/** Resolves `expiry=` arg against the listed expiries: a date, "next" (second), or nearest by default. */
export function pickExpiry(arg: string | undefined, expiries: readonly string[]): string | undefined {
  if (expiries.length === 0) return undefined;
  if (arg === "next") return expiries[1] ?? expiries[0];
  if (arg && expiries.includes(arg)) return arg;
  return expiries[0];
}
