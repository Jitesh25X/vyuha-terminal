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

// ── option chain (openapi.yaml: OptionChain, OptionChainRow, OptionSide) ─────
/** One contract. Market fields absent until a tick is seen; analytics absent where undefined. */
export interface OptionSide {
  instrument_id: number;
  tradingsymbol: string;
  ltp?: string;
  prev_close?: string;
  bid?: string;
  ask?: string;
  volume?: number;
  oi?: number;
  oi_chg?: number;
  /** decimal: 0.142 = 14.2 % */
  iv?: number;
  /** price the IV was solved from: tight mid, else an LTP that traded within the last minute */
  iv_source?: "mid" | "ltp";
  delta?: number;
  gamma?: number;
  /** ₹ per calendar day */
  theta?: number;
  /** ₹ per vol point */
  vega?: number;
}

export interface OptionChainRow {
  strike: string;
  ce?: OptionSide;
  pe?: OptionSide;
}

export interface OptionChain {
  underlying: { instrument_id: number; ltp?: string; future_id?: number; fut_ltp?: string };
  expiry: string;
  lot_size: number;
  analytics: "ok" | "unavailable";
  model?: "black76" | "bsm";
  forward?: number;
  forward_source?: "future" | "synthetic_mid" | "synthetic_ltp" | "spot";
  rate?: number;
  /** date the configured risk-free rate took effect */
  rate_as_of?: string;
  t_years?: number;
  atm_strike?: string;
  /** over every strike of the expiry with OI (`oi_strikes`), not just the rows */
  pcr?: number;
  max_pain?: string;
  oi_strikes?: number;
  as_of: string;
  rows: OptionChainRow[];
  /** every contract of the expiry: subscribed so PCR and max pain see the whole chain */
  contract_ids: number[];
}

// ── indices (openapi.yaml: IndexConstituents, Constituent) ───────────────────
export interface Constituent {
  instrument_id: number;
  tradingsymbol: string;
  name?: string;
  sector?: string;
  /** index weight, percent */
  weight?: number;
  avg_volume_20d?: number;
  /** near-month stock future, for OI gainers */
  future_id?: number;
  future_prev_oi?: number;
}

export interface IndexConstituents {
  index_id: number;
  as_of: string;
  constituents: Constituent[];
}
