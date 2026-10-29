import { expect, type Page } from "@playwright/test";
import { currentStep, totp } from "./totp";

/**
 * First login for a fresh e2e user, keyboard only: password, TOTP enrollment (the seed is read off
 * the enrollment response, as an authenticator would scan it), then the workspace.
 */
export async function enrollAndSignIn(page: Page, username: string, password: string): Promise<string> {
  await page.goto("/");
  const enroll = page.waitForResponse((r) => r.url().endsWith("/api/v1/auth/mfa/enroll") && r.ok());
  await expect(page.getByLabel("Username")).toBeFocused();
  await page.keyboard.type(username);
  await page.keyboard.press("Tab");
  await page.keyboard.type(password);
  await page.keyboard.press("Enter");
  const { otpauth_url } = (await (await enroll).json()) as { otpauth_url: string };
  const secret = new URL(otpauth_url).searchParams.get("secret") ?? "";
  await expect(page.getByLabel("Authenticator code")).toBeFocused();
  const confirmed = page.waitForResponse((r) => r.url().endsWith("/api/v1/auth/mfa/enroll/confirm") && r.ok());
  await page.keyboard.type(await totp(secret, currentStep()));
  await page.keyboard.press("Enter");
  const { user } = (await (await confirmed).json()) as { user: { id: string } };
  await expect(page.getByLabel("Status")).toBeVisible();
  return user.id;
}

/** Types a command into the command bar (` to focus) and runs it. */
export async function run(page: Page, command: string): Promise<void> {
  await page.keyboard.press("Backquote");
  await expect(page.getByPlaceholder("RELIANCE EQ CH  ·  HELP")).toBeFocused();
  await page.keyboard.type(command);
  await page.keyboard.press("Enter");
}
