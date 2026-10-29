import { describe, expect, it } from "vitest";
import { direction, formatPct, formatPrice, formatQty, formatRupees, formatVolume } from "./num";

describe("number formatting (en-IN)", () => {
  it("prices from int64 units", () => {
    expect(formatPrice(24_505_000)).toBe("2,450.50");
    expect(formatPrice(123_456_789_000)).toBe("1,23,45,678.90");
    expect(formatPrice(25)).toBe("0.0025"); // sub-paisa tick
    expect(formatPrice(-5_000)).toBe("-0.50");
    expect(formatPrice(undefined)).toBe("—");
    expect(formatRupees(123_456_750_000)).toBe("₹1,23,45,675.00");
    expect(formatRupees(-32_400_000)).toBe("-₹3,240.00");
  });
  it("percent, quantity, volume", () => {
    expect(formatPct(1.234)).toBe("+1.23%");
    expect(formatPct(-0.4)).toBe("-0.40%");
    expect(formatPct(0)).toBe("0.00%");
    expect(formatQty(1234567)).toBe("12,34,567");
    expect(formatVolume(98_700_000)).toBe("9.87 Cr");
    expect(formatVolume(450_000)).toBe("4.50 L");
    expect(formatVolume(12_345)).toBe("12.3 K");
    expect(formatVolume(950)).toBe("950");
  });
  it("direction", () => {
    expect([direction(2), direction(-1), direction(0), direction(undefined)]).toEqual(["up", "down", "flat", "flat"]);
  });
});
