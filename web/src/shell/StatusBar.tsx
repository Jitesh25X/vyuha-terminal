import { useHealth } from "../lib/api/auth";

// Live trading is off in M0 (no broker adapters yet), so the desk is always PAPER here.
// UI_SPEC §5: paper must be visually unmistakable.

export function StatusBar() {
  const health = useHealth();
  const gw = health.isError ? "DOWN" : health.data ? health.data.status.toUpperCase() : "…";
  const gwClass = health.isError ? "text-down" : health.data?.status === "ok" ? "text-up" : "text-warn";
  return (
    <footer className="h-[22px] flex items-center gap-4 px-2 bg-panel-header border-t border-line" aria-label="Status">
      <span className="text-paper border border-paper px-1 font-semibold">PAPER</span>
      <span>
        gateway <span className={gwClass}>{gw}</span>
      </span>
      {health.data && <span className="text-muted">{health.data.env} · v{health.data.version}</span>}
      <span className="text-muted ml-auto">brokers: none connected</span>
    </footer>
  );
}
