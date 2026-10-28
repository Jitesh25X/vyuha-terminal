import { defineConfig, devices } from "@playwright/test";

// Run through `make e2e`: scripts/e2e.sh starts a throwaway gateway + database and sets
// VYUHA_GATEWAY_URL / VYUHA_E2E_USER / VYUHA_E2E_PASSWORD.
const port = Number(process.env["VYUHA_WEB_PORT"] ?? 5173);

export default defineConfig({
  testDir: "e2e",
  fullyParallel: false,
  workers: 1,
  forbidOnly: !!process.env["CI"],
  retries: 0,
  reporter: [["list"]],
  use: {
    baseURL: `http://127.0.0.1:${port}`,
    trace: "retain-on-failure",
  },
  projects: [{ name: "chromium", use: { ...devices["Desktop Chrome"] } }],
  webServer: {
    command: `pnpm exec vite --port ${port} --strictPort`,
    url: `http://127.0.0.1:${port}`,
    reuseExistingServer: false,
    timeout: 60_000,
  },
});
