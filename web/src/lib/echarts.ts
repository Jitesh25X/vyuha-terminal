// Apache ECharts (TECH_STACK: analytics charts, canvas renderer), registered once with only the parts
// the terminal uses so the bundle stays small.
import { BarChart, LineChart } from "echarts/charts";
import { GridComponent, LegendComponent, MarkLineComponent, TooltipComponent } from "echarts/components";
import * as echarts from "echarts/core";
import { CanvasRenderer } from "echarts/renderers";
import { useEffect, useRef } from "react";

echarts.use([BarChart, LineChart, GridComponent, LegendComponent, MarkLineComponent, TooltipComponent, CanvasRenderer]);

export type ChartOption = echarts.EChartsCoreOption;

/** Reads a design token (tokens.css) so charts follow the theme. */
export function token(name: string): string {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}

/** Text, axis and tooltip styling shared by every analytics chart (recessive grid, text in ink tokens). */
export function baseOption(): ChartOption {
  const muted = token("--text-muted");
  const grid = token("--grid");
  return {
    animation: false,
    textStyle: { color: token("--text-secondary"), fontFamily: token("--font-mono"), fontSize: 11 },
    grid: { left: 56, right: 12, top: 28, bottom: 24 },
    tooltip: {
      trigger: "axis",
      axisPointer: { type: "shadow" },
      backgroundColor: token("--panel-header"),
      borderColor: token("--border"),
      textStyle: { color: token("--text-secondary"), fontSize: 11 },
    },
    legend: { top: 4, right: 8, itemWidth: 10, itemHeight: 10, textStyle: { color: token("--text-secondary") } },
    xAxis: { axisLine: { lineStyle: { color: grid } }, axisTick: { show: false }, axisLabel: { color: muted } },
    yAxis: { splitLine: { lineStyle: { color: grid } }, axisLabel: { color: muted } },
  };
}

/**
 * Mounts a chart on the returned ref and applies `option` whenever it changes (in place: no
 * re-creation per poll). Resizes with its container (Dockview panels resize freely).
 */
export function useEChart<T extends HTMLElement>(option: ChartOption | undefined) {
  const el = useRef<T>(null);
  const chart = useRef<echarts.ECharts | null>(null);
  useEffect(() => {
    if (!el.current) return;
    const c = echarts.init(el.current, undefined, { renderer: "canvas" });
    chart.current = c;
    const ro = new ResizeObserver(() => c.resize());
    ro.observe(el.current);
    return () => {
      ro.disconnect();
      c.dispose();
      chart.current = null;
    };
  }, []);
  useEffect(() => {
    if (option) chart.current?.setOption(option, { notMerge: true, lazyUpdate: true });
  }, [option]);
  return el;
}
