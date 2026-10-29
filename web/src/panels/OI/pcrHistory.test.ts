import { beforeEach, describe, expect, it } from "vitest";
import { recordPcr, resetPcrHistory } from "./pcrHistory";

describe("PCR history", () => {
  beforeEach(resetPcrHistory);

  it("appends one point per new chain time, per key", () => {
    recordPcr("1|2026-11-03", "2026-10-29T05:30:00Z", 0.8);
    recordPcr("1|2026-11-03", "2026-10-29T05:30:00Z", 0.9); // same poll time: ignored
    recordPcr("1|2026-11-03", "2026-10-29T05:30:01Z", undefined); // no PCR: ignored
    const s = recordPcr("1|2026-11-03", "2026-10-29T05:30:02Z", 0.85);
    expect(s).toEqual([
      [Date.parse("2026-10-29T05:30:00Z"), 0.8],
      [Date.parse("2026-10-29T05:30:02Z"), 0.85],
    ]);
    expect(recordPcr("1|2026-11-10", "2026-10-29T05:30:02Z", 1.1)).toHaveLength(1);
  });

  it("caps the series", () => {
    let s: readonly [number, number][] = [];
    for (let i = 0; i < 2_100; i++) s = recordPcr("k", new Date(Date.UTC(2026, 9, 29, 4, 0, i)).toISOString(), 1);
    expect(s).toHaveLength(2_000);
    expect(s[0]![0]).toBe(Date.UTC(2026, 9, 29, 4, 0, 100));
  });
});
