import type { PanelProps } from "../../commands/types";
import { MarketPanel } from "../../components/MarketPanel";
import { direction, formatPct, formatPrice, formatQty, formatVolume } from "../../lib/format/num";
import { formatIstTime } from "../../lib/format/time";
import { selectQuote, useMd } from "../../stores/md";

const DIR = { up: "text-up", down: "text-down", flat: "text-fg" } as const;

function Quote({ id }: { id: number }) {
  const q = useMd(selectQuote(id));
  if (!q) return <p className="p-3 text-muted">Waiting for the first quote…</p>;
  const change = q.prevClose > 0 ? q.ltp - q.prevClose : undefined;
  const dir = DIR[direction(change)];
  const rows: [string, string][] = [
    ["Open", formatPrice(q.open)],
    ["High", formatPrice(q.high)],
    ["Low", formatPrice(q.low)],
    ["Prev close", formatPrice(q.prevClose)],
    ["Avg price", formatPrice(q.avgPrice)],
    ["Volume", formatVolume(q.volume)],
    ["OI", q.oi > 0 ? formatQty(q.oi) : "—"],
    ["Last qty", formatQty(q.ltq)],
  ];
  return (
    <div className="p-3 space-y-3 tabular-nums" aria-label="Quote">
      <div className="flex items-baseline gap-3">
        <span className={`text-2xl font-semibold ${dir}`} data-testid="ltp">
          {formatPrice(q.ltp)}
        </span>
        <span className={dir}>{change === undefined ? "—" : formatPrice(change)}</span>
        <span className={dir}>{formatPct(q.changePct)}</span>
        <span className="ml-auto text-muted text-xs">{q.exchangeTs > 0 ? formatIstTime(new Date(q.exchangeTs)) : ""}</span>
      </div>
      <div className="grid grid-cols-2 gap-x-6 gap-y-0.5 max-w-md">
        <span className="text-muted">Bid</span>
        <span className="text-up">
          {formatPrice(q.bid)} <span className="text-muted">× {formatQty(q.bidQty)}</span>
        </span>
        <span className="text-muted">Ask</span>
        <span className="text-down">
          {formatPrice(q.ask)} <span className="text-muted">× {formatQty(q.askQty)}</span>
        </span>
        {rows.map(([k, v]) => (
          <div key={k} className="contents">
            <span className="text-muted">{k}</span>
            <span>{v}</span>
          </div>
        ))}
      </div>
    </div>
  );
}

export default function QuotePanel(props: PanelProps) {
  return (
    <MarketPanel {...props} channel="quote">
      {(inst) => <Quote id={inst.id} />}
    </MarketPanel>
  );
}
