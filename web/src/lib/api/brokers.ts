import { useQuery } from "@tanstack/react-query";
import { request } from "./client";
import type { BrokerAccount } from "./types";

/** The user's broker accounts with session status (re-read every minute: tokens expire overnight). */
export function useBrokerAccounts() {
  return useQuery({
    queryKey: ["brokers", "accounts"],
    queryFn: () => request<BrokerAccount[]>("/brokers/accounts", { auth: true }),
    refetchInterval: 60_000,
  });
}

/**
 * Starts the daily broker login: the gateway returns the broker's login URL with a signed, single-use
 * state; the browser goes there (the broker's own 2FA) and comes back to /terminal?broker_login=…
 */
export async function startBrokerLogin(accountId: string): Promise<void> {
  const { url } = await request<{ url: string }>(`/brokers/${encodeURIComponent(accountId)}/login-url`, { auth: true });
  window.location.assign(url);
}

export const BROKER_NAME = { zerodha: "Kite", upstox: "Upstox", sim: "Sim" } as const;

/** Messages for the callback's ?broker_login=error&code=… (codes from API.md §7). */
export const BROKER_LOGIN_ERRORS: Record<string, string> = {
  BROKER_LOGIN_STATE_INVALID: "the login link expired or was already used — start again",
  BROKER_LOGIN_DENIED: "the login was cancelled at the broker",
  BROKER_LOGIN_FORBIDDEN: "you may not log in this account",
  BROKER_ACCOUNT_MISMATCH: "you signed in to a different client at the broker",
  BROKER_TOKEN_EXCHANGE_FAILED: "the broker refused the token exchange",
  BROKER_SECRETS_UNREADABLE: "the account's API credentials can't be read — ask admin",
  STATIC_IP_NOT_ON_HOST: "the account's static IP is not configured on the server — ask admin",
  ENGINE_UNAVAILABLE: "the engine is not running — ask admin",
};
