import { useMemo } from "react";
import type { PanelProps } from "../../commands/types";
import { IndexPanel } from "../../components/IndexPanel";
import { baseOption, token, useEChart, type ChartOption } from "../../lib/echarts";
import { direction, formatPct, formatPrice } from "../../lib/format/num";
import { heatColor, inkOn, sectorSummary, type MoverRow } from "../../lib/movers";

const DIR = { up: "text-up", down: "text-down", flat: "text-fg" } as const;
/** ±3 % saturates the scale; beyond it tiles keep the end colour. */
const LIMIT = 3;

interface Leaf {
  name: string;
  value: number;
  pct: number | undefined;
  sector: string;
  weight: number | undefined;
  ltp: number | undefined;
  itemStyle: { color: string };
  label: { color: string };
}

function Treemap({ rows }: { rows: MoverRow[] }) {
  const option = useMemo<ChartOption>(() => {
    const [up, down, neutral] = [token("--up"), token("--down"), token("--border")];
    const weighted = rows.every((r) => r.weight !== undefined);
    const sectors = new Map<string, Leaf[]>();
    for (const r of rows) {
      const color = heatColor(r.changePct, up, down, neutral, LIMIT);
      const leaf: Leaf = {
        name: r.sym, value: weighted ? r.weight! : 1, pct: r.changePct, sector: r.sector, weight: r.weight, ltp: r.ltp,
        itemStyle: { color }, label: { color: inkOn(color) },
      }; // prettier-ignore
      sectors.set(r.sector, [...(sectors.get(r.sector) ?? []), leaf]);
    }
    const base = baseOption();
    return {
      textStyle: base["textStyle"] ?? {},
      animation: false,
      tooltip: {
        ...(base["tooltip"] as object),
        trigger: "item",
        formatter: (p: { data: Partial<Leaf> & { name: string } }) =>
          p.data.sector === undefined
            ? p.data.name
            : `${p.data.name} · ${p.data.sector}<br/>LTP ${formatPrice(p.data.ltp)} · ${formatPct(p.data.pct)}` +
              (p.data.weight === undefined ? "" : `<br/>Weight ${p.data.weight.toFixed(2)}%`),
      },
      series: [
        {
          type: "treemap",
          data: [...sectors].map(([name, children]) => ({ name, children })),
          roam: false,
          nodeClick: false,
          breadcrumb: { show: false },
          width: "100%",
          height: "100%",
          top: 0,
          left: 0,
          upperLabel: { show: true, height: 16, color: token("--text-secondary"), fontSize: 10 },
          label: { show: true, fontSize: 10, formatter: (p: { data: Leaf }) => `${p.data.name}\n${formatPct(p.data.pct)}` },
          // levels[0] is the (unnamed) root: no label; [1] sectors; [2] constituents.
          levels: [
            { upperLabel: { show: false }, itemStyle: { borderWidth: 0, gapWidth: 2 } },
            { itemStyle: { borderColor: token("--panel"), borderWidth: 2, gapWidth: 1 } },
            { itemStyle: { borderColor: token("--panel"), borderWidth: 1 } },
          ],
        },
      ],
    };
  }, [rows]);
  const ref = useEChart<HTMLDivElement>(option);
  return <div ref={ref} className="h-[260px] w-full" role="img" aria-label="Constituent heatmap by % change, sized by index weight" />;
}

function SectorTable({ rows }: { rows: MoverRow[] }) {
  const sectors = sectorSummary(rows);
  return (
    <table className="text-xs tabular-nums whitespace-nowrap" aria-label="Sectors">
      <thead className="text-muted">
        <tr>
          <th className="px-2 text-left font-normal">Sector</th>
          <th className="px-2 text-right font-normal">Members</th>
          <th className="px-2 text-right font-normal">Weight</th>
          <th className="px-2 text-right font-normal">% (wtd)</th>
        </tr>
      </thead>
      <tbody>
        {sectors.map((s) => (
          <tr key={s.sector} className="border-b border-grid">
            <td className="px-2">{s.sector}</td>
            <td className="px-2 text-right">{s.count}</td>
            <td className="px-2 text-right">{s.weight === undefined ? "—" : `${s.weight.toFixed(2)}%`}</td>
            <td className={`px-2 text-right ${DIR[direction(s.changePct)]}`}>{formatPct(s.changePct)}</td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}

/** HM <index> — constituent heatmap by % change, sized by index weight, grouped by sector (F-MD-3). */
export default function HeatmapPanel(props: PanelProps) {
  return (
    <IndexPanel {...props}>
      {(rows) => (
        <div className="flex flex-col gap-1 p-1">
          <Treemap rows={rows} />
          <p className="px-1 text-xs text-muted">
            Colour: <span className="text-down">−{LIMIT}%</span> … 0 … <span className="text-up">+{LIMIT}%</span> (clamped)
            {rows.some((r) => r.weight === undefined) && " · equal tiles: no index weights in the data"}
          </p>
          <SectorTable rows={rows} />
        </div>
      )}
    </IndexPanel>
  );
}
