import { expect, test, type Page } from "@playwright/test";
import { enrollAndSignIn, run } from "./session";

// M1.10: OC and OI open from the keyboard against the real gateway (expiries from the seeded master;
// the chain itself is refused because the e2e user has no data-entitled account), then render a
// fixture chain (no engine/md_last in e2e) to check the table, summary, keyboard navigation and charts.

const user = process.env["VYUHA_E2E_OC_USER"] ?? "";
const password = process.env["VYUHA_E2E_PASSWORD"] ?? "";
const NOT_ENTITLED = "No market-data entitlement — ask admin to map a data-enabled account";

function side(id: number, ltp: string, iv: number | undefined, delta: number, oi: number, oiChg: number | undefined, prev: string, ivSource = "mid") {
  return { instrument_id: id, tradingsymbol: `NIFTYE2E${id}`, ltp, prev_close: prev, bid: ltp, ask: ltp, volume: 120_000,
    oi, oi_chg: oiChg, iv, iv_source: iv === undefined ? undefined : ivSource, delta, gamma: 0.0004, theta: -12.5, vega: 9.8 }; // prettier-ignore
}

const FIXTURE = {
  underlying: { instrument_id: 1100, ltp: "24290.0000", future_id: 1210, fut_ltp: "24320.0000" },
  expiry: "2026-11-05",
  lot_size: 75,
  analytics: "ok",
  model: "black76",
  forward: 24320,
  forward_source: "future",
  rate: 0.055,
  rate_as_of: "2026-10-07",
  t_years: 0.02,
  atm_strike: "24300.0000",
  pcr: 0.82,
  max_pain: "24200.0000",
  oi_strikes: 41,
  contract_ids: [1201, 1202, 1203, 1204, 1205, 1206],
  as_of: new Date().toISOString(),
  rows: [
    { strike: "24200.0000", ce: side(1201, "190.0000", 0.131, 0.62, 1_500_000, 250_000, "150.0000"), pe: side(1202, "80.0000", undefined, -0.38, 2_100_000, -50_000, "95.0000") }, // wide spread, no recent trade: no IV
    { strike: "24300.0000", ce: side(1203, "120.5000", 0.1234, 0.51, 1_800_000, 400_000, "140.0000", "ltp"), pe: side(1204, "110.0000", 0.135, -0.49, 1_600_000, 300_000, "90.0000") },
    { strike: "24400.0000", ce: side(1205, "70.0000", 0.128, 0.38, 2_400_000, -100_000, "60.0000"), pe: side(1206, "170.0000", 0.15, -0.62, 900_000, undefined, "190.0000") }, // no OI baseline
  ],
};

async function stubChain(page: Page) {
  await page.route("**/api/v1/optionchain/**", (route) =>
    route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ ...FIXTURE, as_of: new Date().toISOString() }) }),
  );
}

test("OC and OI: keyboard, entitlement state, then a fixture chain with summary, ATM, Greeks and charts", async ({ page }) => {
  expect(user, "run through make e2e").not.toBe("");
  await enrollAndSignIn(page, user, password);
  for (const func of ["OC", "OI"]) {
    await run(page, `NIFTY ${func}`);
    const panel = page.getByRole("region", { name: `${func} NIFTY`, exact: true }).last();
    await expect(panel).toBeVisible();
    await expect(panel.getByLabel("Expiry")).toHaveCount(1); // the seeded expiry, from the master
    await expect(panel.getByRole("alert")).toHaveText(NOT_ENTITLED);
  }
  await run(page, "NOSUCHSYM OC");
  await expect(page.getByRole("region", { name: "OC NOSUCHSYM" }).getByRole("alert")).toHaveText("No instrument matches NOSUCHSYM");
  await run(page, "RELIANCE OC");
  await expect(page.getByRole("region", { name: "OC RELIANCE" }).getByRole("alert")).toHaveText("No listed options for RELIANCE");

  // From here the chain endpoint answers with a fixture (entitled data needs an engine).
  await stubChain(page);
  await run(page, "NIFTY OC");
  const oc = page.getByRole("region", { name: "OC NIFTY", exact: true }).last();
  const summary = oc.getByLabel("Chain summary");
  await expect(summary).toContainText("PCR (all strikes) 0.82");
  await expect(summary).toContainText("Max pain (all strikes) 24,200.00");
  const footer = oc.getByLabel("Chain footer");
  await expect(footer).toContainText("r 5.50% (as of 2026-10-07)");
  await expect(footer).toContainText("PCR (all strikes, 41 with OI)");
  await expect(summary).toContainText("ATM 24,300.00");
  await expect(summary).toContainText("Fwd (fut) 24,320.00");
  const table = oc.getByRole("table", { name: "Option chain" });
  await expect(table.getByRole("row", { name: /^Strike/ })).toHaveCount(3);
  const atm = table.getByRole("row", { name: "Strike 24,300.00 (ATM)" });
  await expect(atm).toContainText("120.50"); // CE LTP
  await expect(atm).toContainText("12.34"); // CE IV %
  await expect(atm.getByLabel("from last traded price")).toHaveCount(1); // CE IV came from a recent LTP
  await expect(table.getByRole("row", { name: "Strike 24,200.00" })).toContainText("—"); // PE: no usable price, no IV
  const top = table.getByRole("row", { name: "Strike 24,400.00" });
  await expect(top).toContainText("—"); // PE ΔOI without a baseline: never 0
  await expect(top).not.toContainText("+0");
  await expect(table.getByRole("columnheader", { name: "Γ" })).toHaveCount(0);

  // Keyboard: expiry → Greeks toggle (Enter) → the ATM row → ↓ to the next strike.
  await page.keyboard.press("Tab");
  await expect(oc.getByLabel("Expiry")).toBeFocused();
  await page.keyboard.press("Tab");
  await expect(oc.getByRole("button", { name: "Show Γ Θ ν" })).toBeFocused();
  await page.keyboard.press("Enter");
  await expect(table.getByRole("columnheader", { name: "Γ" })).toHaveCount(2);
  await page.keyboard.press("Tab");
  await expect(atm).toBeFocused();
  await page.keyboard.press("ArrowDown");
  await expect(table.getByRole("row", { name: "Strike 24,400.00" })).toBeFocused();

  await run(page, "NIFTY OI");
  const oi = page.getByRole("region", { name: "OI NIFTY", exact: true }).last();
  await expect(oi.getByRole("img", { name: "Open interest by strike" })).toBeVisible();
  await expect(oi.getByRole("img", { name: "OI change by strike" })).toBeVisible();
  await expect(oi.getByRole("img", { name: "PCR history" })).toBeVisible();
  const buildup = oi.getByRole("table", { name: "Build-up by strike" });
  await expect(buildup.getByRole("row", { name: /24,200\.00 Long build-up Long unwinding/ })).toBeVisible();
  await expect(buildup.getByRole("row", { name: /24,300\.00 Short build-up Long build-up/ })).toBeVisible();
  await expect(buildup.getByRole("row", { name: /24,400\.00 Short covering —/ })).toBeVisible();
});
