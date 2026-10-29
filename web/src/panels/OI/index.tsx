import type { PanelProps } from "../../commands/types";
import { ChainPanel } from "../../components/ChainPanel";
import type { OptionChain } from "../../lib/api/types";
import { formatPrice } from "../../lib/format/num";
import { classifyBuildup, decimalUnits, type Buildup } from "../../lib/optionchain";
import { OiBars, PcrLine } from "./charts";
import { recordPcr } from "./pcrHistory";

const TONE: Record<Buildup, string> = {
  "Long build-up": "text-up",
  "Short covering": "text-up",
  "Short build-up": "text-down",
  "Long unwinding": "text-down",
};

function BuildupTable({ chain }: { chain: OptionChain }) {
  const rows = chain.rows
    .map((r) => ({ strike: r.strike, ce: classifyBuildup(r.ce), pe: classifyBuildup(r.pe) }))
    .filter((r) => r.ce || r.pe);
  if (rows.length === 0) return <p className="px-2 py-1 text-xs text-muted">No build-up yet (needs today's price and OI change).</p>;
  const cell = (b: Buildup | undefined) => <td className={`px-2 ${b ? TONE[b] : "text-muted"}`}>{b ?? "—"}</td>;
  return (
    <table className="text-xs tabular-nums" aria-label="Build-up by strike">
      <thead className="text-muted">
        <tr>
          <th className="px-2 text-right font-normal">Strike</th>
          <th className="px-2 text-left font-normal text-series-ce">Calls</th>
          <th className="px-2 text-left font-normal text-series-pe">Puts</th>
        </tr>
      </thead>
      <tbody>
        {rows.map((r) => (
          <tr key={r.strike} className="border-b border-grid">
            <td className="px-2 text-right">{formatPrice(decimalUnits(r.strike))}</td>
            {cell(r.ce)}
            {cell(r.pe)}
          </tr>
        ))}
      </tbody>
    </table>
  );
}

function OiBody({ chain }: { chain: OptionChain }) {
  const pcr = recordPcr(`${chain.underlying.instrument_id}|${chain.expiry}`, chain.as_of, chain.pcr);
  return (
    <div className="flex flex-col gap-1 p-1">
      <h3 className="px-1 text-xs text-muted">Open interest by strike</h3>
      <OiBars chain={chain} metric="oi" />
      <h3 className="px-1 text-xs text-muted">OI change since the previous close</h3>
      <OiBars chain={chain} metric="oi_chg" />
      <h3 className="px-1 text-xs text-muted">PCR — this session ({pcr.length} points)</h3>
      <PcrLine points={pcr} />
      <h3 className="px-1 text-xs text-muted">Build-up</h3>
      <BuildupTable chain={chain} />
    </div>
  );
}

/** OI <underlying> [expiry=…] [strikes=N] — OI/ΔOI by strike, build-up, PCR history (F-MD-3). */
export default function OiPanel(props: PanelProps) {
  return <ChainPanel {...props}>{(chain) => <OiBody chain={chain} />}</ChainPanel>;
}
