import { useEffect, useRef } from "react";
import { useHealth } from "../lib/api/auth";
import { BROKER_NAME, startBrokerLogin, useBrokerAccounts } from "../lib/api/brokers";
import type { FeedLite } from "../lib/ws/protocol";
import { useMd } from "../stores/md";
import { pushToast } from "../stores/toasts";

// Live trading is off until the order path exists, so the desk is always PAPER here.
// UI_SPEC §5: paper must be visually unmistakable.

const BROKER = { zerodha: "Kite", upstox: "Upstox", sim: "Sim", unknown: "Feed" } as const;
const DOT: Record<FeedLite["state"], string> = {
  connected: "text-up",
  connecting: "text-warn",
  reconnecting: "text-warn",
  idle: "text-muted",
  down: "text-down",
};

export const feedName = (f: FeedLite) => `${BROKER[f.broker]} ${f.label || f.accountId.slice(0, 8)}`;

/**
 * PRD F-UI-3: a feed that drops shows red within 2 s (status arrives every second) and raises
 * "Upstox <acct> market data disconnected — reconnecting".
 */
function useDisconnectToasts(feeds: FeedLite[] | undefined) {
  const prev = useRef(new Map<string, FeedLite["state"]>());
  useEffect(() => {
    if (!feeds) return;
    const next = new Map<string, FeedLite["state"]>();
    for (const f of feeds) {
      next.set(f.accountId, f.state);
      const was = prev.current.get(f.accountId);
      if (was === "connected" && f.state !== "connected") {
        pushToast("warn", `${feedName(f)} market data disconnected — reconnecting`);
      }
    }
    prev.current = next;
  }, [feeds]);
}

/** Accounts the user may log in whose session isn't connected: one button each (daily broker login). */
function LoginPrompts() {
  const accounts = useBrokerAccounts();
  const pending = (accounts.data ?? []).filter((a) => a.can_login && a.status !== "connected" && a.status !== "disabled");
  if (pending.length === 0) return null;
  return (
    <span className="flex items-center gap-2" aria-label="Broker logins">
      {pending.map((a) => (
        <button
          key={a.id}
          type="button"
          title={`${a.holder_name} · ${a.status.replace("_", " ")}`}
          className="border border-warn text-warn px-1 focus-visible:outline focus-visible:outline-amber"
          onClick={() =>
            void startBrokerLogin(a.id).catch((e: unknown) =>
              pushToast("error", `${BROKER_NAME[a.broker]} ${a.client_code}: login could not start — ${e instanceof Error ? e.message : String(e)}`),
            )
          }
        >
          LOGIN {BROKER_NAME[a.broker].toUpperCase()} {a.client_code}
        </button>
      ))}
    </span>
  );
}

export function StatusBar() {
  const health = useHealth();
  const system = useMd((s) => s.system);
  const conn = useMd((s) => s.conn);
  useDisconnectToasts(system?.feeds);
  const gw = health.isError ? "DOWN" : health.data ? health.data.status.toUpperCase() : "…";
  const gwClass = health.isError ? "text-down" : health.data?.status === "ok" ? "text-up" : "text-warn";
  const market = system?.marketState.replace("_", "-").toUpperCase();
  return (
    <footer className="h-[22px] flex items-center gap-4 px-2 bg-panel-header border-t border-line whitespace-nowrap overflow-hidden" aria-label="Status">
      <span className="text-paper border border-paper px-1 font-semibold">PAPER</span>
      {market && <span className={market === "OPEN" ? "text-up" : "text-muted"}>{market}</span>}
      <span className="flex items-center gap-3" aria-label="Broker feeds">
        {system && system.feeds.length === 0 && <span className="text-muted">no data feeds</span>}
        {system?.feeds.map((f) => (
          <span key={f.accountId} data-feed-state={f.state} title={`${f.state} · ${f.subscribed} instruments`}>
            <span className={DOT[f.state]}>●</span> {feedName(f).toUpperCase()}
          </span>
        ))}
      </span>
      <LoginPrompts />
      {system && <span className="text-muted">data {system.dataLatencyMs}ms</span>}
      <span>
        engine <span className={system ? (system.engineOk ? "text-up" : "text-down") : "text-muted"}>{system ? (system.engineOk ? "OK" : "DOWN") : "…"}</span>
      </span>
      <span>
        stream <span className={conn === "open" ? "text-up" : conn === "connecting" ? "text-warn" : "text-down"}>{conn.toUpperCase()}</span>
      </span>
      <span>
        gateway <span className={gwClass}>{gw}</span>
      </span>
      {health.data && <span className="text-muted ml-auto">{health.data.env} · v{health.data.version}</span>}
    </footer>
  );
}
