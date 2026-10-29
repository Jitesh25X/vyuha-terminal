import tailwindcss from "@tailwindcss/vite";
import { defineConfig } from "vitest/config";

// The dev server proxies /api to the gateway so the refresh cookie (Path=/api/v1/auth,
// SameSite=Strict) stays same-origin. scripts/e2e.sh points it at a throwaway gateway.
const gateway = process.env["VYUHA_GATEWAY_URL"] ?? "http://127.0.0.1:8080";

export default defineConfig({
  plugins: [tailwindcss()],
  server: {
    host: "127.0.0.1",
    port: 5173,
    strictPort: true,
    proxy: { "/api": { target: gateway }, "/ws": { target: gateway, ws: true } },
  },
  build: {
    rolldownOptions: {
      // cmdk's Radix deps carry "use client" (an RSC hint); meaningless in this SPA bundle.
      onwarn(warning, warn) {
        if (warning.code !== "MODULE_LEVEL_DIRECTIVE") warn(warning);
      },
    },
  },
  test: {
    environment: "node",
    include: ["src/**/*.test.ts"],
  },
});
