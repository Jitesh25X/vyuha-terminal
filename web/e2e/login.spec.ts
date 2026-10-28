import { expect, test, type Page } from "@playwright/test";
import { currentStep, totp } from "./totp";

// M0.9 done check: login → workspace → HELP, keyboard only. Run via `make e2e` (scripts/e2e.sh
// provides a throwaway gateway with one un-enrolled trader).

const username = process.env["VYUHA_E2E_USER"] ?? "";
const password = process.env["VYUHA_E2E_PASSWORD"] ?? "";

// Each TOTP step is accepted once (replay protection), so never reuse one across tests.
let secret = "";
let lastStep = 0;
async function nextCode(): Promise<string> {
  lastStep = Math.max(currentStep(), lastStep + 1);
  return totp(secret, lastStep);
}

async function typePassword(page: Page) {
  await expect(page.getByLabel("Username")).toBeFocused();
  await page.keyboard.type(username);
  await page.keyboard.press("Tab");
  await page.keyboard.type(password);
  await page.keyboard.press("Enter");
}

test.describe.serial("auth + shell", () => {
  test.beforeAll(() => {
    expect(username, "run through make e2e").not.toBe("");
  });

  test("first login enrolls TOTP, lands in the workspace, HELP opens from the keyboard", async ({ page }) => {
    await page.goto("/");
    // The enrollment response carries the seed; read it off the wire like an authenticator would.
    const enroll = page.waitForResponse((r) => r.url().endsWith("/api/v1/auth/mfa/enroll") && r.ok());
    await typePassword(page);
    const { otpauth_url } = (await (await enroll).json()) as { otpauth_url: string };
    secret = new URL(otpauth_url).searchParams.get("secret") ?? "";
    expect(secret).not.toBe("");

    await expect(page.getByAltText("Authenticator QR code")).toBeVisible();
    await expect(page.getByLabel("Authenticator code")).toBeFocused();
    await page.keyboard.type(await nextCode());
    await page.keyboard.press("Enter");

    // Workspace: trader default layout (UI_SPEC §8), top bar and PAPER status bar.
    await expect(page.locator('[data-func="CH"]')).toBeVisible();
    for (const f of ["DOM", "OC", "BLT", "AI"]) await expect(page.locator(`[data-func="${f}"]`)).toBeVisible();
    await expect(page.getByText("E2E Trader")).toBeVisible();
    await expect(page.getByLabel("Status").getByText("PAPER")).toBeVisible();
    await expect(page.getByLabel("Status").getByText("OK", { exact: true })).toBeVisible();

    // ` focuses the command bar; HELP + Enter opens the help panel in the focused slot.
    await page.keyboard.press("Backquote");
    const bar = page.getByPlaceholder("RELIANCE EQ CH  ·  HELP");
    await expect(bar).toBeFocused();
    await page.keyboard.type("HELP");
    await page.keyboard.press("Enter");
    const help = page.getByRole("region", { name: "Help" });
    await expect(help).toBeVisible();
    await expect(help.getByRole("heading", { name: "HELP" })).toBeVisible();
    await expect(bar).toHaveValue("");
    await expect(bar).not.toBeFocused(); // focus moved into the new panel

    // Ctrl+K, then HELP <FUNC> for one function's page.
    await page.keyboard.press("Control+k");
    await expect(bar).toBeFocused();
    await page.keyboard.type("help ch");
    await page.keyboard.press("Enter");
    await expect(page.getByLabel("Help for CH")).toBeVisible();

    // A function that needs a security explains itself instead of opening.
    await page.keyboard.press("Control+k");
    await page.keyboard.type("CH");
    await page.keyboard.press("Enter");
    await expect(page.getByRole("alert").filter({ hasText: "CH needs a security" })).toBeVisible();
    await page.keyboard.press("Escape");
    await expect(bar).toHaveValue("");
    await page.keyboard.press("Escape");
    await expect(bar).not.toBeFocused();

    // Reload: the access token is in memory only, the refresh cookie restores the session.
    await page.reload();
    await expect(page.locator('[data-func="CH"]')).toBeVisible();
  });

  test("enrolled user signs in with a TOTP code, then signs out", async ({ page }) => {
    await page.goto("/");
    await typePassword(page);
    await expect(page.getByLabel("Authenticator code")).toBeFocused();
    await expect(page.getByAltText("Authenticator QR code")).toHaveCount(0); // no re-enrollment

    await page.keyboard.type("000000");
    await page.keyboard.press("Enter");
    await expect(page.getByRole("alert")).toContainText("Invalid or already used code");

    await page.getByLabel("Authenticator code").fill("");
    await page.keyboard.type(await nextCode());
    await page.keyboard.press("Enter");
    await expect(page.locator('[data-func="CH"]')).toBeVisible();

    await page.keyboard.press("F1");
    await expect(page.getByRole("region", { name: "Help" })).toBeVisible();

    await page.getByRole("button", { name: "Sign out" }).focus();
    await page.keyboard.press("Enter");
    await expect(page.getByLabel("Username")).toBeVisible();
    await page.reload(); // the refresh cookie was revoked and cleared
    await expect(page.getByLabel("Username")).toBeVisible();
  });
});
