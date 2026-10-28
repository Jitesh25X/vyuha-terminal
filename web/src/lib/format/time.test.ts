import { describe, expect, it } from "vitest";
import { formatIstTime } from "./time";

describe("formatIstTime", () => {
  it.each([
    ["2026-10-28T09:02:07Z", "14:32:07"],
    ["2026-10-28T18:30:00Z", "00:00:00"], // crosses midnight into the next IST day
    ["2026-03-29T01:30:00Z", "07:00:00"], // no DST shift in IST
  ])("%s → %s", (iso, want) => {
    expect(formatIstTime(new Date(iso))).toBe(want);
  });
});
