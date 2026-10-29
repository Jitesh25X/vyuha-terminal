import { useRef, useState, type KeyboardEvent } from "react";
import type { OptionChain, OptionSide } from "../../lib/api/types";
import { direction, formatGreek, formatIv, formatPrice, formatVolume } from "../../lib/format/num";
import { decimalUnits, isItm, referencePrice } from "../../lib/optionchain";

type Col = {
  label: string;
  title: string;
  cell: (s: OptionSide) => string;
  dir?: (s: OptionSide) => number | undefined;
  /** small marker after the value, with its explanation */
  mark?: (s: OptionSide) => { text: string; title: string } | undefined;
};

const LTP_IV = { text: "L", title: "IV from the last traded price (traded within the last minute): the bid-ask spread is too wide for the mid" };

const signedVolume = (v: number | undefined) => (v === undefined ? "—" : `${v > 0 ? "+" : ""}${formatVolume(v)}`);

const OI: Col = { label: "OI", title: "Open interest", cell: (s) => formatVolume(s.oi) };
const DOI: Col = { label: "ΔOI", title: "OI change since the previous session's close", cell: (s) => signedVolume(s.oi_chg), dir: (s) => s.oi_chg };
const VOL: Col = { label: "Vol", title: "Volume", cell: (s) => formatVolume(s.volume) };
const IV: Col = {
  label: "IV",
  title: "Implied volatility, %: from the mid when the spread is tight; L = from a recent LTP; — when neither is usable",
  cell: (s) => formatIv(s.iv),
  mark: (s) => (s.iv !== undefined && s.iv_source === "ltp" ? LTP_IV : undefined),
};
const DELTA: Col = { label: "Δ", title: "Delta", cell: (s) => formatGreek(s.delta, 2) };
const GAMMA: Col = { label: "Γ", title: "Gamma (per ₹ of underlying)", cell: (s) => formatGreek(s.gamma, 4) };
const THETA: Col = { label: "Θ", title: "Theta, ₹ per day", cell: (s) => formatGreek(s.theta, 2) };
const VEGA: Col = { label: "ν", title: "Vega, ₹ per vol point", cell: (s) => formatGreek(s.vega, 2) };
const LTP: Col = {
  label: "LTP",
  title: "Last traded price",
  cell: (s) => formatPrice(decimalUnits(s.ltp)),
  dir: (s) => {
    const l = decimalUnits(s.ltp);
    const p = decimalUnits(s.prev_close);
    return l === undefined || p === undefined ? undefined : l - p;
  },
};

const DIR = { up: "text-up", down: "text-down", flat: "" } as const;

function columns(greeks: boolean): Col[] {
  return greeks ? [OI, DOI, VOL, IV, DELTA, GAMMA, THETA, VEGA, LTP] : [OI, DOI, VOL, IV, DELTA, LTP];
}

function Cells({ s, cols, itm }: { s: OptionSide | undefined; cols: Col[]; itm: boolean }) {
  return cols.map((c) => {
    const mark = s && c.mark?.(s);
    return (
      <td key={c.label} className={`px-1.5 text-right ${itm ? "bg-white/5" : ""} ${s && c.dir ? DIR[direction(c.dir(s))] : ""}`}>
        {s ? c.cell(s) : ""}
        {mark && (
          <sup className="ml-px text-[9px] text-warn" title={mark.title} aria-label="from last traded price">
            {mark.text}
          </sup>
        )}
      </td>
    );
  });
}

/**
 * Calls | strike | puts, strikes ascending. ITM cells are shaded, the ATM strike is outlined. Rows
 * are reachable with Tab (roving tabindex) and ↑/↓/Home/End.
 */
export function ChainTable({ chain, greeks }: { chain: OptionChain; greeks: boolean }) {
  const cols = columns(greeks);
  const ref = referencePrice(chain);
  const atmIndex = Math.max(chain.rows.findIndex((r) => r.strike === chain.atm_strike), 0);
  const [active, setActive] = useState<number | undefined>();
  const current = active !== undefined && active < chain.rows.length ? active : atmIndex;
  const body = useRef<HTMLTableSectionElement>(null);

  const onKey = (e: KeyboardEvent<HTMLTableSectionElement>) => {
    const last = chain.rows.length - 1;
    const next = { ArrowDown: current + 1, ArrowUp: current - 1, Home: 0, End: last }[e.key];
    if (next === undefined) return;
    e.preventDefault();
    const i = Math.min(Math.max(next, 0), last);
    setActive(i);
    body.current?.querySelectorAll<HTMLTableRowElement>("tr")[i]?.focus();
  };

  return (
    <table className="min-w-full text-xs tabular-nums border-collapse whitespace-nowrap" aria-label="Option chain">
      <thead className="sticky top-0 bg-panel-header text-muted">
        <tr>
          <th colSpan={cols.length} className="text-series-ce font-semibold py-0.5">CALLS</th>
          <th className="px-2" />
          <th colSpan={cols.length} className="text-series-pe font-semibold">PUTS</th>
        </tr>
        <tr className="border-b border-line">
          {cols.map((c) => (
            <th key={c.label} title={c.title} className="px-1.5 text-right font-normal">{c.label}</th>
          ))}
          <th className="px-2 text-center font-normal text-fg">Strike</th>
          {[...cols].reverse().map((c) => (
            <th key={c.label} title={c.title} className="px-1.5 text-right font-normal">{c.label}</th>
          ))}
        </tr>
      </thead>
      <tbody ref={body} onKeyDown={onKey}>
        {chain.rows.map((r, i) => {
          const atm = r.strike === chain.atm_strike;
          return (
            <tr
              key={r.strike}
              tabIndex={i === current ? 0 : -1}
              onFocus={() => setActive(i)}
              aria-label={`Strike ${formatPrice(decimalUnits(r.strike))}${atm ? " (ATM)" : ""}`}
              className={`h-[var(--row-h)] border-b border-grid focus-visible:outline focus-visible:outline-accent ${atm ? "outline outline-1 outline-amber" : ""}`}
            >
              <Cells s={r.ce} cols={cols} itm={isItm("ce", r.strike, ref)} />
              <td className={`px-2 text-center font-semibold ${atm ? "text-amber" : "text-fg"}`}>{formatPrice(decimalUnits(r.strike))}</td>
              <Cells s={r.pe} cols={[...cols].reverse()} itm={isItm("pe", r.strike, ref)} />
            </tr>
          );
        })}
      </tbody>
    </table>
  );
}
