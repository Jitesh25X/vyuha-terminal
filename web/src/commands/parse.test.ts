import { describe, expect, it } from "vitest";
import { completeFunction, parseCommand } from "./parse";
import { FUNCTIONS } from "./registry";

describe("parseCommand", () => {
  it.each([
    ["help", { fn: "HELP", args: {} }],
    ["HELP ch", { fn: "HELP", args: { topic: "CH" } }],
    ["reliance ch", { fn: "CH", security: "RELIANCE", args: {} }],
    ["RELIANCE EQ CH interval=5m", { fn: "CH", security: "RELIANCE", segment: "EQ", args: { interval: "5m" } }],
    ["NIFTY OC expiry=next", { fn: "OC", security: "NIFTY", args: { expiry: "next" } }],
    ["#256265 Q", { fn: "Q", security: "#256265", args: {} }],
    ["M&M Q", { fn: "Q", security: "M&M", args: {} }],
    ["  pos  ", { fn: "POS", args: {} }],
    ["N", { fn: "N", args: {} }],
    ["INFY N", { fn: "N", security: "INFY", args: {} }],
  ])("%s", (input, want) => {
    const r = parseCommand(input);
    expect(r.ok).toBe(true);
    if (!r.ok) return;
    const { fn, ...rest } = want;
    expect(r.cmd.fn.mnemonic).toBe(fn);
    expect({ security: r.cmd.security, segment: r.cmd.segment, args: r.cmd.args }).toEqual({
      security: undefined,
      segment: undefined,
      ...rest,
    });
  });

  it.each([
    ["", "Type a function"],
    ["CH", "CH needs a security"],
    ["OC", "OC needs an underlying"],
    ["RELIANCE POS", "POS does not take a security"],
    ["RELIANCE XYZ", "Unknown function XYZ"],
    ["HELP XYZ", "Unknown function XYZ"],
    ["interval=5m CH", "Arguments go after the function"],
    ["RELIANCE CH interval=5m EQ", 'Unexpected "EQ" after arguments'],
    ["A B CH", 'Unexpected "B"'],
    ["A B EQ CH", "Too many words"],
    ["RELI;ANCE CH", "Invalid security"],
  ])("%j fails: %s", (input, want) => {
    const r = parseCommand(input);
    expect(r.ok).toBe(false);
    if (!r.ok) expect(r.error).toContain(want);
  });
});

describe("registry", () => {
  it("has unique, upper-case mnemonics", () => {
    const m = FUNCTIONS.map((f) => f.mnemonic);
    expect(new Set(m).size).toBe(m.length);
    for (const x of m) expect(x).toMatch(/^[A-Z]+$/);
  });
});

describe("completeFunction", () => {
  it("matches by mnemonic or title prefix of the last word", () => {
    expect(completeFunction("RELIANCE c", FUNCTIONS).map((f) => f.mnemonic)).toEqual(["CH", "CTX", "CAL", "AI", "COMP"]);
    expect(completeFunction("hel", FUNCTIONS).map((f) => f.mnemonic)).toEqual(["HELP"]);
    expect(completeFunction("CH interval=", FUNCTIONS)).toEqual([]);
  });
});
