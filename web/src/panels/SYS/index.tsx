import { useEffect, useState } from "react";
import type { PanelProps } from "../../commands/types";
import { formatQty } from "../../lib/format/num";
import { latencyStats, type LatencyStats, resetLatency } from "../../lib/ws/client";
import { HISTOGRAM_EDGES_MS } from "../../lib/ws/latency";
import { useMd } from "../../stores/md";
import { feedName } from "../../shell/StatusBar";

/** PRD NFR: tick → browser paint p95 < 400 ms (with 250 ms conflation). */
const BUDGET_P95_MS = 400;

const ms = (v: number | undefined) => (v === undefined ? "—" : `${Math.round(v)} ms`);
const BUCKETS = [...HISTOGRAM_EDGES_MS.map((e, i) => `${i === 0 ? 0 : HISTOGRAM_EDGES_MS[i - 1]}–${e}`), `>${HISTOGRAM_EDGES_MS.at(-1)}`];

function Latency({ s }: { s: LatencyStats }) {
  const max = Math.max(1, ...s.histogram);
  const ok = s.p95 !== undefined && s.p95 < BUDGET_P95_MS;
  return (
    <section aria-label="Tick to screen latency" className="space-y-2">
      <h3 className="text-amber">Tick → screen</h3>
      {s.samples === 0 ? (
        <p className="text-muted">
          {s.clockOffsetMs === undefined ? "Waiting for a clock estimate (Ping/Pong)…" : "No quotes painted yet — open a panel with live instruments."}
        </p>
      ) : (
        <>
          <div className="flex gap-6 tabular-nums">
            <span>
              p50 <span data-testid="p50">{ms(s.p50)}</span>
            </span>
            <span>
              p95{" "}
              <span data-testid="p95" className={ok ? "text-up" : "text-down"}>
                {ms(s.p95)}
              </span>{" "}
              <span className="text-muted">(budget &lt; {BUDGET_P95_MS} ms)</span>
            </span>
            <span>p99 {ms(s.p99)}</span>
            <span className="text-muted">{formatQty(s.samples)} quotes</span>
          </div>
          <div className="grid grid-cols-[6rem_1fr_4rem] gap-x-2 tabular-nums text-xs" aria-label="Latency histogram">
            {s.histogram.map((n, i) => (
              <div key={BUCKETS[i]} className="contents">
                <span className="text-muted text-right">{BUCKETS[i]} ms</span>
                <span className="bg-panel-header h-3 self-center">
                  <span className="block h-full bg-accent" style={{ width: `${(n / max) * 100}%` }} />
                </span>
                <span className="text-right">{formatQty(n)}</span>
              </div>
            ))}
          </div>
          <p className="text-muted text-xs">
            Exchange → screen p50 {ms(s.exchangeP50)} · p95 {ms(s.exchangeP95)} (includes the broker&apos;s own delay)
          </p>
        </>
      )}
      <p className="text-muted text-xs">
        Engine receive → paint in this browser, on the server clock: offset {ms(s.clockOffsetMs)}, Ping RTT {ms(s.clockRttMs)} (error ≤ RTT/2).
      </p>
      <button type="button" className="border border-line px-2 focus-visible:outline focus-visible:outline-amber" onClick={resetLatency}>
        Reset samples
      </button>
    </section>
  );
}

/** System health: feeds, engine, stream and tick → screen latency (UI_SPEC §3 SYS). */
export default function SystemPanel(_: PanelProps) {
  const system = useMd((s) => s.system);
  const conn = useMd((s) => s.conn);
  const [stats, setStats] = useState(latencyStats);
  useEffect(() => {
    const t = setInterval(() => setStats(latencyStats()), 1000);
    return () => clearInterval(t);
  }, []);
  return (
    <section className="h-full overflow-auto p-3 space-y-4" aria-label="SYS">
      <section aria-label="Services" className="flex gap-6">
        <span>
          stream <span className={conn === "open" ? "text-up" : "text-down"}>{conn.toUpperCase()}</span>
        </span>
        <span>
          engine <span className={system?.engineOk ? "text-up" : "text-down"}>{system ? (system.engineOk ? "OK" : "DOWN") : "…"}</span>
        </span>
        <span>market {system?.marketState ?? "…"}</span>
        <span>engine → gateway {ms(system?.dataLatencyMs)}</span>
      </section>
      <Latency s={stats} />
      <section aria-label="Feeds">
        <h3 className="text-amber">Feeds</h3>
        {!system || system.feeds.length === 0 ? (
          <p className="text-muted">No data feeds you can see.</p>
        ) : (
          <table className="tabular-nums">
            <thead className="text-muted">
              <tr>
                <th className="pr-4 text-left font-normal">Account</th>
                <th className="pr-4 text-left font-normal">State</th>
                <th className="pr-4 text-right font-normal">Instruments</th>
              </tr>
            </thead>
            <tbody>
              {system.feeds.map((f) => (
                <tr key={f.accountId}>
                  <td className="pr-4">{feedName(f)}</td>
                  <td className={`pr-4 ${f.state === "connected" ? "text-up" : "text-down"}`}>{f.state}</td>
                  <td className="pr-4 text-right">{formatQty(f.subscribed)}</td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </section>
    </section>
  );
}
