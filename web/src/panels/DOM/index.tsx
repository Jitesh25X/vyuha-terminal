import type { PanelProps } from "../../commands/types";
import { MarketPanel } from "../../components/MarketPanel";
import { formatPrice, formatQty } from "../../lib/format/num";
import type { LevelLite } from "../../lib/ws/protocol";
import { selectDepth, useMd } from "../../stores/md";

/** Market depth: 5 levels (30 where Upstox full_d30 arrives), best bid/ask at the top. */
function Book({ id }: { id: number }) {
  const d = useMd(selectDepth(id));
  if (!d) return <p className="p-3 text-muted">Waiting for depth…</p>;
  const rows = Math.max(d.bids.length, d.asks.length, 5);
  const sum = (ls: LevelLite[]) => ls.reduce((a, l) => a + l.qty, 0);
  const cell = (l: LevelLite | undefined, k: keyof LevelLite) => (l ? (k === "price" ? formatPrice(l.price) : formatQty(l[k])) : "");
  return (
    <table className="w-full tabular-nums text-right" aria-label="Depth">
      <thead className="text-muted">
        <tr>
          <th className="px-2 font-normal">Orders</th>
          <th className="px-2 font-normal">Qty</th>
          <th className="px-2 font-normal">Bid</th>
          <th className="px-2 font-normal">Ask</th>
          <th className="px-2 font-normal">Qty</th>
          <th className="px-2 font-normal">Orders</th>
        </tr>
      </thead>
      <tbody>
        {Array.from({ length: rows }, (_, i) => {
          const b = d.bids[i];
          const a = d.asks[i];
          return (
            <tr key={i} className="h-[18px]">
              <td className="px-2 text-muted">{cell(b, "orders")}</td>
              <td className="px-2">{cell(b, "qty")}</td>
              <td className="px-2 text-up">{cell(b, "price")}</td>
              <td className="px-2 text-down">{cell(a, "price")}</td>
              <td className="px-2">{cell(a, "qty")}</td>
              <td className="px-2 text-muted">{cell(a, "orders")}</td>
            </tr>
          );
        })}
      </tbody>
      <tfoot className="border-t border-line">
        <tr>
          <td className="px-2 text-muted">Total</td>
          <td className="px-2">{formatQty(sum(d.bids))}</td>
          <td />
          <td />
          <td className="px-2">{formatQty(sum(d.asks))}</td>
          <td />
        </tr>
      </tfoot>
    </table>
  );
}

export default function DepthPanel(props: PanelProps) {
  return (
    <MarketPanel {...props} channel="depth">
      {(inst) => <Book id={inst.id} />}
    </MarketPanel>
  );
}
