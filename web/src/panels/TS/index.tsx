import { useEffect, useRef, useState } from "react";
import type { PanelProps } from "../../commands/types";
import { MarketPanel } from "../../components/MarketPanel";
import { formatPrice, formatQty } from "../../lib/format/num";
import { formatIstTime } from "../../lib/format/time";
import { onPrints } from "../../lib/ws/client";
import type { PrintLite } from "../../lib/ws/protocol";

const MAX_PRINTS = 200;
const SIDE = { up: "text-up", down: "text-down", flat: "text-fg" } as const;

/**
 * Time & sales from the quote stream. The gateway conflates (≥ 50 ms), so this lists the trades
 * seen at each conflation tick, not every exchange print.
 */
function Prints({ id }: { id: number }) {
  const buf = useRef<PrintLite[]>([]);
  const [prints, setPrints] = useState<PrintLite[]>([]);
  useEffect(() => {
    let frame = 0;
    const off = onPrints((ps) => {
      const mine = ps.filter((p) => p.id === id);
      if (mine.length === 0) return;
      buf.current = [...mine.reverse(), ...buf.current].slice(0, MAX_PRINTS);
      // One React update per frame however many prints arrived.
      if (!frame) frame = requestAnimationFrame(() => ((frame = 0), setPrints(buf.current)));
    });
    return () => {
      off();
      cancelAnimationFrame(frame);
    };
  }, [id]);
  if (prints.length === 0) return <p className="p-3 text-muted">No trades yet.</p>;
  return (
    <table className="w-full tabular-nums text-right" aria-label="Time and sales">
      <thead className="text-muted sticky top-0 bg-panel">
        <tr>
          <th className="px-2 font-normal text-left">Time</th>
          <th className="px-2 font-normal">Price</th>
          <th className="px-2 font-normal">Qty</th>
        </tr>
      </thead>
      <tbody>
        {prints.map((p) => (
          <tr key={`${p.ts}-${p.price}-${p.qty}`} className="h-[18px]">
            <td className="px-2 text-left text-muted">{formatIstTime(new Date(p.ts))}</td>
            <td className={`px-2 ${SIDE[p.side]}`}>{formatPrice(p.price)}</td>
            <td className="px-2">{formatQty(p.qty)}</td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}

export default function TimeAndSalesPanel(props: PanelProps) {
  return (
    <MarketPanel {...props} channel="quote" conflateMs={50}>
      {(inst) => <Prints id={inst.id} />}
    </MarketPanel>
  );
}
