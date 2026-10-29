import {
  CandlestickSeries,
  createChart,
  HistogramSeries,
  type IChartApi,
  type ISeriesApi,
  type Time,
  type UTCTimestamp,
} from "lightweight-charts";
import { useEffect, useRef } from "react";
import type { PanelProps } from "../../commands/types";
import { MarketPanel } from "../../components/MarketPanel";
import { useCandles } from "../../lib/api/marketdata";
import type { CandleInterval, CandleRow, Instrument } from "../../lib/api/types";
import { toChartPrice } from "../../lib/format/num";
import { onBars } from "../../lib/ws/client";
import type { BarLite } from "../../lib/ws/protocol";

const INTERVALS: readonly CandleInterval[] = ["1m", "5m", "15m", "1h", "1d"];
const MINUTE = 60_000;
const IST_OFFSET_MS = 330 * MINUTE;
const WIDTH_MS: Record<CandleInterval, number> = { "1m": MINUTE, "5m": 5 * MINUTE, "15m": 15 * MINUTE, "1h": 60 * MINUTE, "1d": 1440 * MINUTE };

/** Bucket start for a 1-minute bar: UTC-aligned intraday buckets, IST calendar days (as the gateway). */
export function bucketStart(tsMs: number, interval: CandleInterval): number {
  const w = WIDTH_MS[interval];
  if (interval === "1d") return Math.floor((tsMs + IST_OFFSET_MS) / w) * w - IST_OFFSET_MS;
  return Math.floor(tsMs / w) * w;
}

const css = (name: string) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

const ist = (opts: Intl.DateTimeFormatOptions) => new Intl.DateTimeFormat("en-IN", { timeZone: "Asia/Kolkata", hourCycle: "h23", ...opts });
const istFull = ist({ day: "2-digit", month: "short", hour: "2-digit", minute: "2-digit" });
const istTime = ist({ hour: "2-digit", minute: "2-digit" });
const istDay = ist({ day: "2-digit", month: "short" });
const toDate = (t: Time) => new Date((t as number) * 1000);

interface Live {
  bucket: number;
  open: number;
  high: number;
  low: number;
  close: number;
  /** per-minute volume inside the bucket (forming minutes are rewritten) */
  minutes: Map<number, number>;
}

function Chart({ inst, interval }: { inst: Instrument; interval: CandleInterval }) {
  const host = useRef<HTMLDivElement>(null);
  const chartRef = useRef<{ chart: IChartApi; candles: ISeriesApi<"Candlestick">; volume: ISeriesApi<"Histogram"> } | null>(null);
  const history = useCandles(inst.id, interval);

  useEffect(() => {
    if (!host.current) return;
    const chart = createChart(host.current, {
      autoSize: true,
      layout: { background: { color: css("--panel") }, textColor: css("--text-secondary"), fontFamily: css("--font-mono"), fontSize: 11 },
      grid: { vertLines: { color: css("--grid") }, horzLines: { color: css("--grid") } },
      rightPriceScale: { borderColor: css("--border") },
      timeScale: {
        borderColor: css("--border"),
        timeVisible: interval !== "1d",
        tickMarkFormatter: (t: Time) => (interval === "1d" ? istDay : istTime).format(toDate(t)),
      },
      localization: { timeFormatter: (t: Time) => (interval === "1d" ? istDay : istFull).format(toDate(t)), locale: "en-IN" },
    });
    const candles = chart.addSeries(CandlestickSeries, {
      upColor: css("--up"),
      downColor: css("--down"),
      borderVisible: false,
      wickUpColor: css("--up"),
      wickDownColor: css("--down"),
    });
    const volume = chart.addSeries(HistogramSeries, { priceScaleId: "vol", priceFormat: { type: "volume" }, color: css("--border") });
    chart.priceScale("vol").applyOptions({ scaleMargins: { top: 0.8, bottom: 0 } });
    chartRef.current = { chart, candles, volume };
    return () => {
      chartRef.current = null;
      chart.remove();
    };
  }, [interval]);

  // History, then live bars appended/updated with update() (UI_SPEC §7: last bar only).
  useEffect(() => {
    const c = chartRef.current;
    if (!c || !history.data) return;
    const rows: CandleRow[] = history.data.bars;
    const toUnits = (s: string) => Math.round(Number(s) * 10_000);
    c.candles.setData(
      rows.map(([ts, o, h, l, cl]) => ({
        time: (Date.parse(ts) / 1000) as UTCTimestamp,
        open: Number(o),
        high: Number(h),
        low: Number(l),
        close: Number(cl),
      })),
    );
    c.volume.setData(rows.map(([ts, , , , , v]) => ({ time: (Date.parse(ts) / 1000) as UTCTimestamp, value: v })));
    const lastRow = rows[rows.length - 1];
    let live: Live | undefined = lastRow
      ? {
          bucket: Date.parse(lastRow[0]),
          open: toUnits(lastRow[1]),
          high: toUnits(lastRow[2]),
          low: toUnits(lastRow[3]),
          close: toUnits(lastRow[4]),
          minutes: new Map([[-1, lastRow[5]]]), // history volume for the bucket so far
        }
      : undefined;
    return onBars((bars: BarLite[]) => {
      for (const b of bars) {
        if (b.id !== inst.id) continue;
        const bucket = bucketStart(b.ts, interval);
        if (live && bucket < live.bucket) continue; // older than what is drawn
        if (!live || bucket > live.bucket) {
          live = { bucket, open: b.open, high: b.high, low: b.low, close: b.close, minutes: new Map() };
        } else {
          live.high = Math.max(live.high, b.high);
          live.low = Math.min(live.low, b.low);
          live.close = b.close;
        }
        // History's last bucket may include this minute already (it was forming when fetched):
        // on 1m the live bar replaces it; on wider intervals one forming minute can count twice
        // until the next fetch.
        if (interval === "1m") live.minutes.clear();
        live.minutes.set(b.ts, b.volume);
        const time = (bucket / 1000) as UTCTimestamp;
        c.candles.update({ time, open: toChartPrice(live.open), high: toChartPrice(live.high), low: toChartPrice(live.low), close: toChartPrice(live.close) });
        c.volume.update({ time, value: [...live.minutes.values()].reduce((a, v) => a + v, 0) });
      }
    });
  }, [history.data, inst.id, interval]);

  return (
    <div className="h-full flex flex-col">
      {history.isError && (
        <p role="alert" className="px-2 text-warn text-xs">
          History unavailable — {history.error.message}. Live bars only.
        </p>
      )}
      <div ref={host} className="flex-1 min-h-0" aria-label={`Chart ${inst.tradingsymbol} ${interval}`} />
    </div>
  );
}

export default function ChartPanel(props: PanelProps) {
  const requested = props.args["interval"] as CandleInterval | undefined;
  const interval: CandleInterval = requested && INTERVALS.includes(requested) ? requested : "1m";
  return (
    <MarketPanel {...props} channel="bar.1m" extra={<span className="text-muted">{interval}</span>}>
      {(inst) => <Chart inst={inst} interval={interval} />}
    </MarketPanel>
  );
}
