import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { useSession } from "../../stores/session";
import { request, refreshSession, resetRefreshForTests } from "./client";

const user = { id: "u1", username: "meera", display_name: "Meera", role: "trader" as const };

function json(status: number, body: unknown, headers: Record<string, string> = {}): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": status >= 400 ? "application/problem+json" : "application/json", ...headers },
  });
}

describe("api client", () => {
  let refreshCalls = 0;
  let token = "fresh";

  beforeEach(() => {
    refreshCalls = 0;
    token = "fresh";
    resetRefreshForTests();
    useSession.getState().signedIn({ access_token: "stale", expires_in: 900, user });
    vi.stubGlobal(
      "fetch",
      vi.fn(async (url: string, init: RequestInit) => {
        const auth = (init.headers as Record<string, string>)["Authorization"];
        if (url === "/api/v1/auth/refresh") {
          refreshCalls++;
          await new Promise((r) => setTimeout(r, 10));
          return json(200, { access_token: token, expires_in: 900, user });
        }
        if (auth !== `Bearer ${token}`) return json(401, { title: "Unauthorized", status: 401, code: "AUTH_UNAUTHENTICATED" });
        return json(200, { ok: true });
      }),
    );
  });

  afterEach(() => vi.unstubAllGlobals());

  it("serializes refresh: concurrent 401s share one refresh call", async () => {
    const results = await Promise.all([1, 2, 3, 4, 5].map(() => request<{ ok: boolean }>("/x", { auth: true })));
    expect(results.every((r) => r.ok)).toBe(true);
    expect(refreshCalls).toBe(1);
    const s = useSession.getState().state;
    expect(s.status === "authenticated" && s.accessToken).toBe("fresh");
  });

  it("starts a new refresh once the previous one settled", async () => {
    await refreshSession();
    await refreshSession();
    expect(refreshCalls).toBe(2);
  });

  it("signs out with a notice when the refresh token was reused", async () => {
    vi.stubGlobal("fetch", vi.fn(async () => json(401, { title: "x", status: 401, code: "AUTH_REFRESH_REUSED" })));
    expect(await refreshSession()).toBeNull();
    const s = useSession.getState().state;
    expect(s.status).toBe("anonymous");
    expect(s.status === "anonymous" && s.notice).toContain("reuse");
  });

  it("surfaces problem details with Retry-After", async () => {
    vi.stubGlobal(
      "fetch",
      vi.fn(async () => json(429, { title: "Too many", status: 429, code: "RATE_LIMITED" }, { "Retry-After": "17" })),
    );
    await expect(request("/auth/login", { body: {} })).rejects.toMatchObject({ status: 429, code: "RATE_LIMITED", retryAfter: 17 });
  });

  it("leaves a restoring session anonymous when the gateway is down", async () => {
    useSession.setState({ state: { status: "restoring" } });
    vi.stubGlobal("fetch", vi.fn(async () => Promise.reject(new TypeError("fetch failed"))));
    expect(await refreshSession()).toBeNull();
    expect(useSession.getState().state.status).toBe("anonymous");
  });
});
