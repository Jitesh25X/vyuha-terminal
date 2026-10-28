// Node globals used by the Vite/Playwright/e2e config files. @types/node is not a dependency
// (lean footprint), so declare only what they read.
declare const process: { env: Record<string, string | undefined> };
