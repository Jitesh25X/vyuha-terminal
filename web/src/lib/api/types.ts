// Mirrors gateway/api/openapi.yaml (components.schemas). Hand-written until a later task adds
// openapi-typescript; keep in sync with the spec when either side changes.

export type Role = "admin" | "risk_manager" | "trader" | "analyst";

export interface User {
  id: string;
  username: string;
  display_name: string;
  role: Role;
}

export interface LoginResponse {
  mfa_token: string;
  mfa_enrolled: boolean;
}

export interface MfaEnrollResponse {
  otpauth_url: string;
  qr_png_base64: string;
}

export interface SessionResponse {
  access_token: string;
  expires_in: number; // seconds
  user: User;
}

export interface Health {
  status: "ok" | "degraded";
  version: string;
  env: "dev" | "staging" | "prod";
  checks: Record<string, { status: "ok" | "fail"; latency_ms?: number; error?: string }>;
}

/** RFC 9457 problem details as returned by the gateway. */
export interface Problem {
  type: string;
  title: string;
  status: number;
  detail?: string;
  code?: string;
  locked_until?: string;
}

// ── market data (openapi.yaml: Instrument, Candles) ─────────────────────────
export interface Instrument {
  id: number;
  exchange: "NSE" | "BSE" | "NFO" | "BFO";
  segment: "EQ" | "FUT" | "OPT" | "IDX";
  tradingsymbol: string;
  symbol: string;
  name?: string;
  expiry?: string;
  strike?: string;
  option_type?: "CE" | "PE";
  lot_size: number;
  tick_size: string;
  freeze_qty?: number;
  fo_ban: boolean;
  underlying_id?: number;
  valid_to?: string;
}

export type CandleInterval = "1m" | "5m" | "15m" | "1h" | "1d";

/** [ts RFC 3339, open, high, low, close (decimal strings), volume, oi] */
export type CandleRow = [string, string, string, string, string, number, number];

export interface Candles {
  bars: CandleRow[];
}
