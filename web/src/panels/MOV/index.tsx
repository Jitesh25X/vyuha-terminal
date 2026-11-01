import type { PanelProps } from "../../commands/types";
import { IndexPanel } from "../../components/IndexPanel";
import { direction, formatPct, formatPrice, formatVolume } from "../../lib/format/num";
import { gainers, losers, oiGainers, volumeShockers, type MoverRow } from "../../lib/movers";

const DIR = { up: "text-up", down: "text-down", flat: "text-fg" } as const;

interface ListProps {
  title: string;
  rows: MoverRow[];
  metric: { label: string; value: (r: MoverRow) => string; tone?: (r: MoverRow) => number | undefined };
  empty: string;
}

function MoverList({ title, rows, metric, empty }: ListProps) {
  return (
    <section aria-label={title} className="min-w-0">
      <h3 className="px-2 py-0.5 text-xs text-muted border-b border-grid">{title}</h3>
      {rows.length === 0 ? (
        <p className="px-2 py-1 text-xs text-muted">{empty}</p>
      ) : (
        <table className="w-full text-xs tabular-nums whitespace-nowrap">
          <thead className="text-muted">
            <tr>
              <th className="px-2 text-left font-normal">Symbol</th>
              <th className="px-2 text-right font-normal">LTP</th>
              <th className="px-2 text-right font-normal">%</th>
              {metric.label !== "%" && <th className="px-2 text-right font-normal">{metric.label}</th>}
            </tr>
          </thead>
          <tbody>
            {rows.map((r) => (
              <tr key={r.id} className="border-b border-grid">
                <td className="px-2">{r.sym}</td>
                <td className="px-2 text-right">{formatPrice(r.ltp)}</td>
                <td className={`px-2 text-right ${DIR[direction(r.changePct)]}`}>{formatPct(r.changePct)}</td>
                {metric.label !== "%" && (
                  <td className={`px-2 text-right ${metric.tone ? DIR[direction(metric.tone(r))] : ""}`}>{metric.value(r)}</td>
                )}
              </tr>
            ))}
          </tbody>
        </table>
      )}
    </section>
  );
}

const pct = { label: "%", value: (r: MoverRow) => formatPct(r.changePct) };

function Movers({ rows, n }: { rows: MoverRow[]; n: number }) {
  const priced = rows.some((r) => r.changePct !== undefined);
  const waiting = priced ? "None right now" : "Waiting for quotes…";
  return (
    <div className="grid grid-cols-[repeat(auto-fit,minmax(220px,1fr))] gap-x-2 gap-y-1 p-1">
      <MoverList title="Gainers" rows={gainers(rows, n)} metric={pct} empty={waiting} />
      <MoverList title="Losers" rows={losers(rows, n)} metric={pct} empty={waiting} />
      <MoverList
        title="Volume shockers"
        rows={volumeShockers(rows, n)}
        metric={{ label: "Vol × avg", value: (r) => (r.volRatio === undefined ? "—" : `${r.volRatio.toFixed(1)}×`) }}
        empty={rows.some((r) => r.volume !== undefined) ? "No 20-day volume baseline in the constituent data" : waiting}
      />
      <MoverList
        title="OI gainers (near-month futures)"
        rows={oiGainers(rows, n)}
        metric={{ label: "OI Δ%", value: (r) => formatPct(r.oiChgPct), tone: (r) => r.oiChgPct }}
        empty="No futures OI change yet (needs each future's previous-close OI)"
      />
      <p className="col-span-full px-2 text-xs text-muted">
        Volume today: {formatVolume(rows.reduce((a, r) => a + (r.volume ?? 0), 0))} across {rows.length} members
      </p>
    </div>
  );
}

/** MOV <index> [n=10] — gainers, losers, volume shockers, OI gainers within an index (F-MD-3). */
export default function MoversPanel(props: PanelProps) {
  const n = Math.min(Math.max(Number(props.args["n"]) || 10, 1), 50);
  return <IndexPanel {...props}>{(rows) => <Movers rows={rows} n={n} />}</IndexPanel>;
}
