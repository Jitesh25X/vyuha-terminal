import { useMemo } from "react";
import type { OptionChain } from "../../lib/api/types";
import { baseOption, token, useEChart, type ChartOption } from "../../lib/echarts";
import { formatPrice, formatVolume } from "../../lib/format/num";
import { formatIstTime } from "../../lib/format/time";
import { decimalUnits } from "../../lib/optionchain";

const strikeLabel = (s: string | undefined) => formatPrice(decimalUnits(s)).replace(/\.00$/, "");

/** CE vs PE bars per strike for OI or ΔOI (one measure per chart: never two y-scales). */
export function OiBars({ chain, metric }: { chain: OptionChain; metric: "oi" | "oi_chg" }) {
  const option = useMemo<ChartOption>(() => {
    const base = baseOption();
    const strikes = chain.rows.map((r) => strikeLabel(r.strike));
    const pick = (s: { oi?: number; oi_chg?: number } | undefined) => (s?.[metric] ?? null);
    const marks =
      metric === "oi"
        ? [
            chain.max_pain && { xAxis: strikeLabel(chain.max_pain), label: { formatter: "Max pain" } },
            chain.atm_strike && { xAxis: strikeLabel(chain.atm_strike), label: { formatter: "ATM" } },
          ].filter(Boolean)
        : [];
    const bar = (name: string, color: string, data: (number | null)[], withMarks: boolean) => ({
      name,
      type: "bar",
      data,
      barMaxWidth: 14,
      barGap: "15%",
      itemStyle: { color, borderRadius: 2, borderColor: token("--panel"), borderWidth: 1 },
      ...(withMarks && marks.length > 0
        ? {
            markLine: {
              symbol: "none",
              silent: true,
              lineStyle: { color: token("--text-muted"), type: "dashed", width: 1 },
              label: { color: token("--text-secondary"), position: "insideEndTop", fontSize: 10 },
              data: marks,
            },
          }
        : {}),
    });
    return {
      ...base,
      xAxis: { ...(base["xAxis"] as object), type: "category", data: strikes },
      yAxis: { ...(base["yAxis"] as object), type: "value", axisLabel: { color: token("--text-muted"), formatter: (v: number) => formatVolume(v) } },
      tooltip: { ...(base["tooltip"] as object), valueFormatter: (v: unknown) => (typeof v === "number" ? formatVolume(v) : "—") },
      series: [
        bar(metric === "oi" ? "CE OI" : "CE ΔOI", token("--series-ce"), chain.rows.map((r) => pick(r.ce)), true),
        bar(metric === "oi" ? "PE OI" : "PE ΔOI", token("--series-pe"), chain.rows.map((r) => pick(r.pe)), false),
      ],
    };
  }, [chain, metric]);
  const ref = useEChart<HTMLDivElement>(option);
  return <div ref={ref} className="h-[200px] w-full" role="img" aria-label={metric === "oi" ? "Open interest by strike" : "OI change by strike"} />;
}

/** PCR over this session's polls (single series: the heading names it, no legend). */
export function PcrLine({ points }: { points: readonly [number, number][] }) {
  const option = useMemo<ChartOption>(() => {
    const base = baseOption();
    return {
      ...base,
      legend: { show: false },
      grid: { left: 40, right: 12, top: 8, bottom: 20 },
      tooltip: { ...(base["tooltip"] as object), axisPointer: { type: "line" }, valueFormatter: (v: unknown) => (typeof v === "number" ? v.toFixed(2) : "—") },
      xAxis: { ...(base["xAxis"] as object), type: "time", axisLabel: { color: token("--text-muted"), formatter: (t: number) => formatIstTime(new Date(t)).slice(0, 5) } },
      yAxis: { ...(base["yAxis"] as object), type: "value", scale: true },
      series: [{ name: "PCR", type: "line", data: points.slice(), showSymbol: false, lineStyle: { width: 2, color: token("--accent") }, itemStyle: { color: token("--accent") } }],
    };
  }, [points]);
  const ref = useEChart<HTMLDivElement>(option);
  return <div ref={ref} className="h-[90px] w-full" role="img" aria-label="PCR history" />;
}
