import { expect, test, type Page } from "@playwright/test";
import { enrollAndSignIn, run } from "./session";

// M1.9 done check — PRD F-MD-2 AC4 / UI_SPEC §7 perf budget: a watchlist of 1,000 instruments, every
// one updating 4 times a second for 60 s, holds ≥ 55 fps with no long tasks (> 50 ms) in steady
// state. The data comes over a real WebSocket in the real protocol, so the whole path
// is measured: socket → worker decode/merge → per-frame diff → store → AG Grid transactions.
// The feed is gateway/cmd/fakefeed, started by scripts/e2e.sh at 4 Hz.

const user = process.env["VYUHA_E2E_PERF_USER"] ?? "";
const feedAddr = process.env["VYUHA_E2E_FAKEFEED"] ?? "";
const password = process.env["VYUHA_E2E_PASSWORD"] ?? "";
const ROWS = 1000;
const HZ = 4;
const SECONDS = Number(process.env["VYUHA_PERF_SECONDS"] ?? 60);
// Before measuring, the page must have finished loading and had this long with no long task, so startup work
// (grid mount, first batches, JIT) isn't measured as steady state. A page that never goes quiet is measured as
// it is after SETTLE_CAP_MS, so it still fails honestly.
const QUIET_MS = 1000;
const SETTLE_CAP_MS = 20_000;

async function settle(page: Page): Promise<void> {
  for (let attempt = 0; attempt < 3; attempt++) {
    await page.waitForLoadState("load");
    try {
      await page.evaluate(
        ([quietMs, capMs]) =>
          new Promise<void>((resolve) => {
            const started = performance.now();
            let quietSince = started;
            const po = new PerformanceObserver(() => {
              quietSince = performance.now();
            });
            po.observe({ type: "longtask" });
            const check = () => {
              const now = performance.now();
              if (now - quietSince >= quietMs || now - started >= capMs) {
                po.disconnect();
                resolve();
                return;
              }
              setTimeout(check, 100);
            };
            check();
          }),
        [QUIET_MS, SETTLE_CAP_MS] as const,
      );
      return;
    } catch (e) {
      // A navigation destroyed the page's context: wait for the new page to load, then go again.
      if (!String(e).includes("Execution context was destroyed")) throw e;
    }
  }
}

test("1,000 rows × 4 Hz holds ≥ 55 fps with no long tasks", async ({ page, request }) => {
  test.setTimeout((SECONDS + 90) * 1000);
  expect(user && feedAddr, "run through make e2e").toBeTruthy();
  const url = `ws://${feedAddr}/ws`;
  const stats = async () => (await (await request.get(`http://${feedAddr}/stats`)).json()) as { batches: number; subscribed: number };
  const userId = await enrollAndSignIn(page, user, password);
  const items = Array.from({ length: ROWS }, (_, i) => ({ id: 100_000 + i, sym: `PERF${String(i).padStart(4, "0")}` }));
  await page.evaluate(
    ([key, wsUrl, wlKey, list]) => {
      localStorage.setItem(key, wsUrl);
      localStorage.setItem(wlKey, list);
    },
    ["vyuha.dev.wsUrl", url, `vyuha.wl.${userId}.perf`, JSON.stringify(items)] as const,
  );
  await page.reload(); // session restores from the refresh cookie; the worker now dials the fake feed
  await expect(page.getByLabel("Status")).toContainText("stream OPEN");
  await expect(page.getByLabel("Status")).toContainText("UPSTOX PERF01");

  await run(page, "WL name=perf");
  const wl = page.getByRole("region", { name: "WL perf" });
  await expect(wl.getByText(`${ROWS} instruments`)).toBeVisible();
  await expect.poll(async () => (await stats()).subscribed).toBe(ROWS);
  const before = (await stats()).batches;
  // Rows are live: the first row's LTP changes between two reads.
  const ltp = wl.locator('[row-index="0"] [col-id="ltp"]');
  await expect(ltp).not.toHaveText("—");
  const first = await ltp.textContent();
  await expect(ltp).not.toHaveText(first ?? "", { timeout: 2000 });

  await settle(page);
  const result = await page.evaluate(
    ([durationMs]) =>
      new Promise<{ fps: number; frames: number; longTasks: number[]; seconds: number }>((resolve) => {
        const longTasks: number[] = [];
        const po = new PerformanceObserver((l) => {
          for (const e of l.getEntries()) longTasks.push(Math.round(e.duration));
        });
        po.observe({ type: "longtask" });
        let frames = 0;
        const t0 = performance.now();
        const tick = () => {
          frames++;
          const elapsed = performance.now() - t0;
          if (elapsed < durationMs) requestAnimationFrame(tick);
          else {
            po.disconnect();
            resolve({ fps: (frames * 1000) / elapsed, frames, longTasks, seconds: elapsed / 1000 });
          }
        };
        requestAnimationFrame(tick);
      }),
    [SECONDS * 1000] as const,
  );
  const batches = (await stats()).batches - before;
  console.log(`perf: ${result.fps.toFixed(1)} fps over ${result.seconds.toFixed(0)} s (${result.frames} frames), long tasks ${JSON.stringify(result.longTasks)}, ${batches} batches × ${ROWS} rows sent`);
  expect(batches).toBeGreaterThanOrEqual(HZ * SECONDS);
  expect(result.fps).toBeGreaterThanOrEqual(55);
  expect(result.longTasks).toEqual([]);

  // M1.12 measuring tool: SYS reports tick → screen percentiles from the same session (the fake
  // feed stamps recvTs at send time, so this is network + worker + paint, without conflation).
  await run(page, "SYS");
  const sys = page.getByRole("region", { name: "Tick to screen latency" });
  await expect(sys.getByTestId("p95")).toHaveText(/^\d+ ms$/);
  const p95 = Number((await sys.getByTestId("p95").textContent())!.replace(" ms", ""));
  console.log(`perf: SYS tick → screen p95 ${p95} ms`);
  expect(p95).toBeLessThan(400);
});
