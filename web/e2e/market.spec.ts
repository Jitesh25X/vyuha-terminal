import { expect, test } from "@playwright/test";
import { enrollAndSignIn, run } from "./session";

// M1.9: Q, CH, DOM, TS and WL open from the keyboard and resolve securities against the gateway.
// The e2e user is mapped to no broker account, so every market panel must show the entitlement
// message (PRD F-MD-2 AC5) — through the real gateway's WebSocket hub.

const user = process.env["VYUHA_E2E_MD_USER"] ?? "";
const password = process.env["VYUHA_E2E_PASSWORD"] ?? "";
const NOT_ENTITLED = "No market-data entitlement — ask admin to map a data-enabled account";

test("market panels open by keyboard and show the entitlement state", async ({ page }) => {
  expect(user, "run through make e2e").not.toBe("");
  await enrollAndSignIn(page, user, password);

  // The stream connects to /ws and the status bar's system channel answers (no engine in e2e).
  const status = page.getByLabel("Status");
  await expect(status).toContainText("stream OPEN");
  await expect(status).toContainText("engine DOWN");

  for (const [cmd, label, header] of [
    ["RELIANCE Q", "Q RELIANCE", "RELIANCE NSE"],
    ["RELIANCE DOM", "DOM RELIANCE", "RELIANCE NSE"],
    ["RELIANCE TS", "TS RELIANCE", "RELIANCE NSE"],
    ["INFY CH interval=5m", "CH INFY", "5m"],
  ] as const) {
    await run(page, cmd); // replaces the focused panel; the default layout has its own DOM etc.
    const panel = page.getByRole("region", { name: label, exact: true }).last();
    await expect(panel).toBeVisible();
    await expect(panel.getByText(header, { exact: true })).toBeVisible();
    await expect(panel.getByRole("alert")).toHaveText(NOT_ENTITLED);
  }

  // An unknown symbol says so.
  await run(page, "NOSUCHSYM Q");
  await expect(page.getByRole("region", { name: "Q NOSUCHSYM" }).getByRole("alert")).toHaveText("No instrument matches NOSUCHSYM");

  // WL: add by typing, remove with Delete — keyboard only.
  await run(page, "WL name=e2e");
  const wl = page.getByRole("region", { name: "WL e2e" });
  await expect(wl).toBeVisible();
  await page.keyboard.press("Tab"); // the new panel has focus; its first control is the add box
  await expect(wl.getByLabel("Add to watchlist")).toBeFocused();
  for (const s of ["RELIANCE", "INFY"]) {
    await page.keyboard.type(s);
    await page.keyboard.press("Enter");
    await expect(wl.getByRole("gridcell", { name: s, exact: true })).toBeVisible();
  }
  await expect(wl.getByText("2 instruments")).toBeVisible();
  await expect(wl.getByRole("alert")).toContainText(NOT_ENTITLED);
  await page.keyboard.press("Tab"); // into the grid header
  await page.keyboard.press("ArrowDown"); // first row: RELIANCE
  await expect(wl.getByRole("gridcell", { name: "RELIANCE", exact: true })).toBeFocused();
  await page.keyboard.press("Delete");
  await expect(wl.getByText("1 instrument", { exact: true })).toBeVisible();
  await expect(wl.getByRole("gridcell", { name: "RELIANCE", exact: true })).toHaveCount(0);

  // Daily broker login (M1.8 API, M1.12 UI): the status bar offers it for the mapped Kite account. The
  // broker's page is intercepted — nothing reaches Kite.
  let kiteUrl: URL | undefined;
  await page.route("https://kite.zerodha.com/**", async (route) => {
    kiteUrl = new URL(route.request().url());
    await route.fulfill({ status: 200, contentType: "text/html", body: "<title>Kite stub</title>stub" });
  });
  const login = status.getByRole("button", { name: "LOGIN KITE E2E001" });
  await expect(login).toBeVisible();
  await login.focus();
  await page.keyboard.press("Enter");
  await expect(page).toHaveTitle("Kite stub");
  expect(kiteUrl?.pathname).toBe("/connect/login");
  expect(kiteUrl?.searchParams.get("v")).toBe("3");
  expect(kiteUrl?.searchParams.get("api_key")).toBe("e2e-kite-key");
  expect(new URLSearchParams(kiteUrl?.searchParams.get("redirect_params") ?? "").get("state")).toMatch(/^[\w-]+\.[\w-]+$/);

  // The broker sends the user back; a cancelled login is reported and the URL cleaned.
  await page.goto("/terminal?broker_login=error&code=BROKER_LOGIN_DENIED");
  await expect(page.getByRole("status").getByText("Broker login failed: the login was cancelled at the broker")).toBeVisible();
  await expect(page).toHaveURL(/\/$/);
});
