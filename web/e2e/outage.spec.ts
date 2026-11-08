import { expect, test } from "@playwright/test";
import { enrollAndSignIn } from "./session";

// D-19 (ADR-027), the browser's end of the path: real frames over a real WebSocket raise the toast and the
// sticky status bar banner, and recovery clears the banner. The stream comes from gateway/cmd/fakefeed, so
// nothing behind it is Postgres. The engine → NATS and NATS → gateway hops have their own no-database tests.

const user = process.env["VYUHA_E2E_OUTAGE_USER"] ?? "";
const password = process.env["VYUHA_E2E_PASSWORD"] ?? "";
const feedAddr = process.env["VYUHA_E2E_FAKEFEED"] ?? "";
const TEXT = "ORDER LOG DOWN: new orders blocked";

test("order log outage: toast and sticky banner, cleared on recovery", async ({ page, request }) => {
  expect(user && feedAddr, "run through make e2e").toBeTruthy();
  await enrollAndSignIn(page, user, password);
  await page.evaluate(([k, url]) => localStorage.setItem(k, url), ["vyuha.dev.wsUrl", `ws://${feedAddr}/ws`] as const);
  await page.reload();
  const status = page.getByLabel("Status");
  await expect(status).toContainText("stream OPEN");
  await expect(status.getByRole("alert")).toHaveCount(0);

  await request.post(`http://${feedAddr}/outage?on=1`);
  try {
    await expect(status.getByRole("alert")).toHaveText(TEXT);
    await expect(page.locator('[aria-live="polite"]').getByText(TEXT)).toBeVisible(); // the toast
  } finally {
    await request.post(`http://${feedAddr}/outage?on=0`);
  }
  await expect(status.getByRole("alert")).toHaveCount(0);
});
