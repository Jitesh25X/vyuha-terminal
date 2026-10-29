import { describe, expect, it } from "vitest";
import type { OptionChain } from "./api/types";
import { formatGreek, formatIv } from "./format/num";
import { classifyBuildup, decimalUnits, isItm, pickExpiry, referencePrice } from "./optionchain";

describe("option chain helpers", () => {
  it("parses decimal strings to units without floats", () => {
    expect(decimalUnits("180.5000")).toBe(1_805_000);
    expect(decimalUnits("24300")).toBe(243_000_000);
    expect(decimalUnits("0.05")).toBe(500);
    expect(decimalUnits("-1.2345")).toBe(-12_345);
    expect(decimalUnits("0.123456")).toBe(1_234); // beyond 4 dp is cut, never rounded via float
    expect(decimalUnits(undefined)).toBeUndefined();
    expect(decimalUnits("1e5")).toBeUndefined();
  });

  it("formats IV in percent and Greeks with fixed decimals", () => {
    expect(formatIv(0.14321)).toBe("14.32");
    expect(formatIv(undefined)).toBe("—");
    expect(formatGreek(-0.35661, 2)).toBe("-0.36");
    expect(formatGreek(Number.NaN, 2)).toBe("—");
  });

  it("moneyness from spot, else forward, else future", () => {
    const c = { underlying: { instrument_id: 1, ltp: "24290.0000", fut_ltp: "24320.0000" }, forward: 24310 } as OptionChain;
    expect(referencePrice(c)).toBe(24290);
    expect(referencePrice({ ...c, underlying: { instrument_id: 1, fut_ltp: "24320.0000" } })).toBe(24310);
    expect(referencePrice({ underlying: { instrument_id: 1, fut_ltp: "24320.0000" } } as OptionChain)).toBe(24320);
    expect(isItm("ce", "24200.0000", 24290)).toBe(true);
    expect(isItm("ce", "24300.0000", 24290)).toBe(false);
    expect(isItm("pe", "24300.0000", 24290)).toBe(true);
    expect(isItm("pe", "24300.0000", undefined)).toBe(false);
  });

  it("classifies build-up from price and OI change", () => {
    const s = (ltp: string, oi_chg: number) => ({ instrument_id: 1, tradingsymbol: "X", ltp, prev_close: "100.0000", oi_chg });
    expect(classifyBuildup(s("110.0000", 500))).toBe("Long build-up");
    expect(classifyBuildup(s("90.0000", 500))).toBe("Short build-up");
    expect(classifyBuildup(s("110.0000", -500))).toBe("Short covering");
    expect(classifyBuildup(s("90.0000", -500))).toBe("Long unwinding");
    expect(classifyBuildup(s("100.0000", 500))).toBeUndefined();
    expect(classifyBuildup({ instrument_id: 1, tradingsymbol: "X", ltp: "110.0000" })).toBeUndefined();
    expect(classifyBuildup(undefined)).toBeUndefined();
  });

  it("resolves the expiry argument", () => {
    const e = ["2026-11-03", "2026-11-10", "2026-11-24"];
    expect(pickExpiry(undefined, e)).toBe("2026-11-03");
    expect(pickExpiry("next", e)).toBe("2026-11-10");
    expect(pickExpiry("2026-11-24", e)).toBe("2026-11-24");
    expect(pickExpiry("2027-01-01", e)).toBe("2026-11-03"); // unknown → nearest
    expect(pickExpiry("next", ["2026-11-03"])).toBe("2026-11-03");
    expect(pickExpiry(undefined, [])).toBeUndefined();
  });
});
