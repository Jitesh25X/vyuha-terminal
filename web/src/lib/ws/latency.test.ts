import { describe, expect, it } from "vitest";
import { bestSample, clockSample, histogram, percentile, Ring } from "./latency";

describe("latency measurement", () => {
  it("estimates the clock offset from a round trip", () => {
    // Browser sends at 1000, server (running 5,000 ms ahead) answers at 6020, reply lands at 1040.
    expect(clockSample(1000, 6020, 1040)).toEqual({ offsetMs: 5000, rttMs: 40 });
    const best = bestSample([clockSample(0, 5100, 200), clockSample(1000, 6005, 1010), clockSample(2000, 7050, 2100)]);
    expect(best).toEqual({ offsetMs: 5000, rttMs: 10 });
    expect(bestSample([])).toBeUndefined();
  });

  it("percentiles (nearest rank)", () => {
    const v = Array.from({ length: 100 }, (_, i) => i + 1); // 1..100
    expect(percentile(v, 50)).toBe(50);
    expect(percentile(v, 95)).toBe(95);
    expect(percentile(v, 99)).toBe(99);
    expect(percentile([7], 95)).toBe(7);
    expect(percentile([], 95)).toBeUndefined();
  });

  it("histogram buckets and the ring", () => {
    expect(histogram([10, 50, 51, 399, 400, 401, 5000])).toEqual([2, 1, 0, 0, 2, 1, 0, 1]);
    const r = new Ring(3);
    for (const x of [1, 2, 3, 4]) r.push(x);
    expect([...r.values()].sort()).toEqual([2, 3, 4]);
  });
});
