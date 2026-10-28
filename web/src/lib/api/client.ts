import { accessToken, useSession } from "../../stores/session";
import type { Problem, SessionResponse } from "./types";

const BASE = "/api/v1";

export class ApiError extends Error {
  readonly status: number;
  readonly code: string;
  readonly retryAfter: number | undefined;
  readonly lockedUntil: string | undefined;

  constructor(status: number, problem: Partial<Problem>, retryAfter?: number) {
    super(problem.title ?? `HTTP ${status}`);
    this.name = "ApiError";
    this.status = status;
    this.code = problem.code ?? `HTTP_${status}`;
    this.retryAfter = retryAfter;
    this.lockedUntil = problem.locked_until;
  }
}

interface RequestOptions {
  method?: "GET" | "POST";
  body?: unknown;
  /** Send the bearer token and, on 401, refresh once and retry. */
  auth?: boolean;
}

async function send(path: string, opts: RequestOptions, token: string | undefined): Promise<Response> {
  const headers: Record<string, string> = { Accept: "application/json, application/problem+json" };
  if (opts.body !== undefined) headers["Content-Type"] = "application/json";
  if (token) headers["Authorization"] = `Bearer ${token}`;
  return fetch(BASE + path, {
    method: opts.method ?? (opts.body === undefined ? "GET" : "POST"),
    headers,
    credentials: "same-origin",
    ...(opts.body === undefined ? {} : { body: JSON.stringify(opts.body) }),
  });
}

async function toError(res: Response): Promise<ApiError> {
  let problem: Partial<Problem> = {};
  try {
    problem = (await res.json()) as Partial<Problem>;
  } catch {
    // non-JSON error body (e.g. proxy error page): keep the status only
  }
  const ra = Number(res.headers.get("Retry-After"));
  return new ApiError(res.status, problem, Number.isFinite(ra) && ra > 0 ? ra : undefined);
}

async function parse<T>(res: Response): Promise<T> {
  if (!res.ok) throw await toError(res);
  if (res.status === 204) return undefined as T;
  return (await res.json()) as T;
}

export async function request<T>(path: string, opts: RequestOptions = {}): Promise<T> {
  if (!opts.auth) return parse<T>(await send(path, opts, undefined));
  const res = await send(path, opts, accessToken());
  if (res.status !== 401) return parse<T>(res);
  // Access token expired or missing: one serialized refresh, then a single retry.
  if (!(await refreshSession())) throw await toError(res);
  return parse<T>(await send(path, opts, accessToken()));
}

// ── refresh: one call in flight per browser ─────────────────────────────────
// The gateway treats a second use of a rotated refresh token as theft and revokes the session
// (API.md §1), so concurrent refreshes must never race: one shared promise per tab, and a Web
// Lock across tabs (they share the cookie, so each waits for the previous rotation to land).

let inFlight: Promise<SessionResponse | null> | null = null;

export function refreshSession(): Promise<SessionResponse | null> {
  inFlight ??= withBrowserLock(doRefresh).finally(() => {
    inFlight = null;
  });
  return inFlight;
}

async function doRefresh(): Promise<SessionResponse | null> {
  const store = useSession.getState();
  let res: Response;
  try {
    res = await send("/auth/refresh", { method: "POST" }, undefined);
  } catch {
    res = new Response(null, { status: 503 });
  }
  if (res.ok) {
    const s = (await res.json()) as SessionResponse;
    store.signedIn(s);
    return s;
  }
  const status = store.state.status;
  if (res.status === 401) {
    const err = await toError(res);
    store.signedOut(
      err.code === "AUTH_REFRESH_REUSED"
        ? "Session revoked: refresh token reuse detected. Sign in again."
        : status === "authenticated"
          ? "Session expired. Sign in again."
          : undefined,
    );
  } else if (status === "restoring") {
    store.signedOut("Gateway unavailable. Try again shortly.");
  }
  // Otherwise (429, transient error while signed in) keep the session; the caller sees its error.
  return null;
}

function withBrowserLock<T>(fn: () => Promise<T>): Promise<T> {
  const locks = typeof navigator === "undefined" ? undefined : navigator.locks;
  return locks ? locks.request("vyuha-auth-refresh", fn) : fn();
}

/** Test hook: drop any in-flight refresh between tests. */
export function resetRefreshForTests(): void {
  inFlight = null;
}
