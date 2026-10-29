import { useEffect, useMemo, useState, type ReactNode } from "react";
import type { PanelProps } from "../commands/types";
import { useInstrument } from "../lib/api/marketdata";
import type { Instrument } from "../lib/api/types";
import { type Channel } from "../lib/ws/protocol";
import { DEFAULT_CONFLATE_MS, useSubscription } from "../lib/ws/client";
import { selectDenied, useMd } from "../stores/md";

/** UI_SPEC §6: a quote older than this while the market is open is STALE. */
const STALE_MS = 3_000;

export function instrumentLabel(i: Instrument): string {
  return `${i.tradingsymbol} ${i.exchange}${i.segment === "EQ" ? "" : ` ${i.segment}`}`;
}

/** Re-renders every second while mounted (for age-based tags). */
function useSecondTick(): number {
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    const t = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(t);
  }, []);
  return now;
}

function Tags({ id }: { id: number }) {
  const market = useMd((s) => s.system?.marketState);
  const last = useMd((s) => s.lastQuoteAt[id]);
  const now = useSecondTick();
  const closed = market !== undefined && market !== "open";
  const stale = !closed && last !== undefined && now - last > STALE_MS;
  return (
    <>
      {closed && <span className="border border-muted text-muted px-1">MARKET CLOSED</span>}
      {stale && <span className="border border-warn text-warn px-1">STALE</span>}
    </>
  );
}

interface Props extends PanelProps {
  channel: Channel;
  conflateMs?: number;
  /** Optional header extras (interval selector etc.). */
  extra?: ReactNode;
  children: (inst: Instrument) => ReactNode;
}

/**
 * Frame for single-security market panels (Q, DOM, TS, CH): resolves the security, subscribes the
 * channel while mounted and shows the not-found / no-entitlement / closed / stale states.
 */
export function MarketPanel({ func, security, segment, channel, conflateMs = DEFAULT_CONFLATE_MS, extra, children }: Props) {
  const q = useInstrument(security, segment);
  const inst = q.data ?? undefined;
  const ids = useMemo(() => (inst ? [inst.id] : undefined), [inst]);
  useSubscription(channel, ids, conflateMs);
  const denied = useMd(selectDenied(inst?.id));

  let body: ReactNode;
  if (!security) body = <p className="p-3 text-muted">Type a security first, e.g. RELIANCE {func}</p>;
  else if (q.isPending) body = <p className="p-3 text-muted">resolving {security}…</p>;
  else if (q.isError) body = <p role="alert" className="p-3 text-down">Instrument lookup failed — {q.error.message}</p>;
  else if (!inst) body = <p role="alert" className="p-3 text-down">No instrument matches {security}{segment ? ` ${segment}` : ""}</p>;
  else if (denied) body = <p role="alert" className="p-3 text-warn">{denied}</p>;
  else body = children(inst);

  return (
    <section className="h-full flex flex-col" aria-label={`${func} ${security ?? ""}`.trim()}>
      <header className="h-[22px] shrink-0 flex items-center gap-2 px-2 bg-panel-header border-b border-line text-xs">
        <span className="text-amber font-semibold">{func}</span>
        <span>{inst ? instrumentLabel(inst) : (security ?? "")}</span>
        {inst?.fo_ban && <span className="border border-down text-down px-1">F&O BAN</span>}
        {inst && <Tags id={inst.id} />}
        <span className="ml-auto flex items-center gap-2">{extra}</span>
      </header>
      <div className="flex-1 min-h-0 overflow-auto">{body}</div>
    </section>
  );
}
