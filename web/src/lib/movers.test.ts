import { describe, expect, it } from "vitest";
import type { Constituent } from "./api/types";
import { buildRows, gainers, heatColor, inkOn, losers, oiGainers, sectorSummary, volumeShockers } from "./movers";
import type { QuoteLite } from "./ws/protocol";

function quote(id: number, changePct: number, volume = 0, oi = 0): QuoteLite {
  return { id, ltp: 1_000_000, ltq: 0, changePct, open: 0, high: 0, low: 0, prevClose: 1_000_000, avgPrice: 0, volume, oi,
    bid: 0, ask: 0, bidQty: 0, askQty: 0, exchangeTs: 0, recvTs: 0 }; // prettier-ignore
}

const C: Constituent[] = [
  { instrument_id: 1, tradingsymbol: "AAA", sector: "Banks", weight: 10, avg_volume_20d: 1000, future_id: 11, future_prev_oi: 2000 },
  { instrument_id: 2, tradingsymbol: "BBB", sector: "Banks", weight: 30, avg_volume_20d: 1000, future_id: 12, future_prev_oi: 2000 },
  { instrument_id: 3, tradingsymbol: "CCC", sector: "IT", weight: 20 },
  { instrument_id: 4, tradingsymbol: "DDD", weight: 5 }, // no sector, no baselines
  { instrument_id: 5, tradingsymbol: "EEE", sector: "IT", weight: 35 }, // no quote yet
];
const Q: Record<number, QuoteLite> = {
  1: quote(1, 2.5, 5000),
  2: quote(2, -1, 500),
  3: quote(3, 0.5),
  4: quote(4, -3),
  11: quote(11, 0, 0, 3000),
  12: quote(12, 0, 0, 1000),
};

describe("index movers", () => {
  const rows = buildRows(C, Q);

  it("joins constituents with quotes and baselines", () => {
    expect(rows[0]).toMatchObject({ sym: "AAA", sector: "Banks", changePct: 2.5, volRatio: 5, oiChgPct: 50 });
    expect(rows[3]).toMatchObject({ sym: "DDD", sector: "Unclassified", volRatio: undefined, oiChgPct: undefined });
    expect(rows[4]).toMatchObject({ sym: "EEE", ltp: undefined, changePct: undefined });
  });

  it("ranks gainers, losers, volume shockers and OI gainers", () => {
    expect(gainers(rows, 5).map((r) => r.sym)).toEqual(["AAA", "CCC"]);
    expect(losers(rows, 5).map((r) => r.sym)).toEqual(["DDD", "BBB"]);
    expect(losers(rows, 1).map((r) => r.sym)).toEqual(["DDD"]);
    expect(volumeShockers(rows, 5).map((r) => r.sym)).toEqual(["AAA", "BBB"]); // only with a baseline
    expect(oiGainers(rows, 5).map((r) => r.sym)).toEqual(["AAA"]); // BBB's future OI fell
  });

  it("rolls sectors up by weight, priced members only", () => {
    const s = sectorSummary(rows);
    expect(s.map((x) => x.sector)).toEqual(["IT", "Banks", "Unclassified"]);
    const banks = s.find((x) => x.sector === "Banks")!;
    expect(banks.weight).toBe(40);
    expect(banks.changePct).toBeCloseTo((10 * 2.5 + 30 * -1) / 40); // weight-averaged
    expect(s.find((x) => x.sector === "IT")!.changePct).toBeCloseTo(0.5); // EEE unpriced: excluded
  });

  it("falls back to equal weights when the source has none", () => {
    const s = sectorSummary(buildRows([{ instrument_id: 1, tradingsymbol: "A", sector: "X" }, { instrument_id: 3, tradingsymbol: "C", sector: "X" }], Q));
    expect(s[0]!.weight).toBeUndefined();
    expect(s[0]!.changePct).toBeCloseTo(1.5);
  });

  it("maps % change onto a clamped diverging scale", () => {
    expect(heatColor(0, "#00ff00", "#ff0000", "#202020")).toBe("#202020");
    expect(heatColor(undefined, "#00ff00", "#ff0000", "#202020")).toBe("#202020");
    expect(heatColor(3, "#00ff00", "#ff0000", "#202020")).toBe("#00ff00");
    expect(heatColor(-9, "#00ff00", "#ff0000", "#202020")).toBe("#ff0000"); // clamped
    expect(heatColor(1.5, "#00ff00", "#ff0000", "#202020")).toBe("#109010");
  });

  it("picks legible label ink for a tile", () => {
    expect(inkOn("#00d26a")).toBe("#000000"); // bright green (--up)
    expect(inkOn("#2a2a2a")).toBe("#ffffff"); // neutral dark
    expect(inkOn("#ff3b3b")).toBe("#000000"); // --down
    expect(inkOn("#7a1d1d")).toBe("#ffffff");
  });
});
