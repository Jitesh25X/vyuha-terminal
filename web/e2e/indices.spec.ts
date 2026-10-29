import { expect, test } from "@playwright/test";
import { enrollAndSignIn, run } from "./session";

// M1.11: MOV and HM open from the keyboard. Against the real gateway there is no constituent source
// yet (M6.1), so both say so. Then a fixture constituent list (stubbed REST) renders with live quotes
// from the fake feed (dev WebSocket override, as in perf.spec.ts): movers rank, the heatmap draws.

const user = process.env["VYUHA_E2E_MOV_USER"] ?? "";
const password = process.env["VYUHA_E2E_PASSWORD"] ?? "";
const feedAddr = process.env["VYUHA_E2E_FAKEFEED"] ?? "";

const SECTORS = ["Banks", "IT", "Energy", "Autos"];
const FIXTURE = {
  index_id: 1100,
  as_of: "2026-10-01",
  constituents: Array.from({ length: 12 }, (_, i) => ({
    instrument_id: 300_000 + i,
    tradingsymbol: `STK${String(i).padStart(2, "0")}`,
    sector: SECTORS[i % SECTORS.length],
    weight: 12 - i * 0.5,
    avg_volume_20d: 50_000, // the fake feed's day volume grows past it within seconds
  })),
};

test("MOV and HM: keyboard, no-source state, then fixture constituents with live quotes", async ({ page }) => {
  expect(user && feedAddr, "run through make e2e").toBeTruthy();
  await enrollAndSignIn(page, user, password);

  for (const func of ["MOV", "HM"]) {
    await run(page, `NIFTY ${func}`);
    await expect(page.getByRole("region", { name: `${func} NIFTY`, exact: true }).last().getByRole("alert")).toHaveText(
      "No constituent data for NIFTY yet (index members and weights arrive with the graph seed)",
    );
  }
  await run(page, "RELIANCE MOV");
  await expect(page.getByRole("region", { name: "MOV RELIANCE" }).getByRole("alert")).toHaveText("MOV needs an index, e.g. NIFTY MOV");

  // Fixture constituents + the fake feed's quotes.
  await page.route("**/api/v1/indices/*/constituents", (route) =>
    route.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify(FIXTURE) }),
  );
  await page.evaluate((url) => localStorage.setItem("vyuha.dev.wsUrl", url), `ws://${feedAddr}/ws`);
  await page.reload(); // session restores from the refresh cookie; the worker now dials the fake feed
  await expect(page.getByLabel("Status")).toContainText("stream OPEN");

  await run(page, "NIFTY MOV n=5");
  const mov = page.getByRole("region", { name: "MOV NIFTY", exact: true }).last();
  await expect(mov.getByText("12 members · as of 2026-10-01")).toBeVisible();
  // Every constituent is priced by the feed: gainers + losers together list rows, at most 5 each.
  const ranked = mov.getByRole("region", { name: /^(Gainers|Losers)$/ }).getByRole("row").filter({ hasText: /^STK/ });
  await expect.poll(() => ranked.count()).toBeGreaterThan(0);
  expect(await ranked.count()).toBeLessThanOrEqual(10);
  const shockers = mov.getByRole("region", { name: "Volume shockers" });
  await expect(shockers.getByRole("row").filter({ hasText: /^STK.*×$/ })).toHaveCount(5); // n=5, baseline present
  await expect(mov.getByRole("region", { name: "OI gainers (near-month futures)" })).toContainText("No futures OI change yet");

  await run(page, "NIFTY HM");
  const hm = page.getByRole("region", { name: "HM NIFTY", exact: true }).last();
  await expect(hm.getByRole("img", { name: "Constituent heatmap by % change, sized by index weight" })).toBeVisible();
  const sectors = hm.getByRole("table", { name: "Sectors" });
  for (const s of SECTORS) await expect(sectors.getByRole("row", { name: new RegExp(`^${s} 3 `) })).toBeVisible();
  await expect(sectors).not.toContainText("—"); // weights present, every sector priced
});
