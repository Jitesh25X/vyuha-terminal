import { useMemo, type ReactNode } from "react";
import type { PanelProps } from "../commands/types";
import { ApiError } from "../lib/api/client";
import { useIndexConstituents, useInstrument } from "../lib/api/marketdata";
import type { IndexConstituents } from "../lib/api/types";
import { buildRows, type MoverRow } from "../lib/movers";
import { DEFAULT_CONFLATE_MS, useSubscription } from "../lib/ws/client";
import { useQuoteSnapshot } from "../lib/ws/snapshot";
import { selectDenied, useMd } from "../stores/md";
import { MarketTags } from "./MarketPanel";

interface Props extends PanelProps {
  children: (rows: MoverRow[], index: IndexConstituents) => ReactNode;
}

/**
 * Frame for index panels (MOV, HM): resolves the index, loads its constituents (reference data),
 * subscribes their quotes (and near-month futures, for OI) and passes rows sampled once a second.
 */
export function IndexPanel({ func, security, segment, children }: Props) {
  const q = useInstrument(security, segment);
  const inst = q.data ?? undefined;
  const isIndex = inst?.segment === "IDX";
  const cons = useIndexConstituents(isIndex ? inst.id : undefined);

  const key = cons.data
    ? cons.data.constituents.flatMap((c) => (c.future_id === undefined ? [c.instrument_id] : [c.instrument_id, c.future_id])).join(",")
    : "";
  const ids = useMemo(() => (key ? key.split(",").map(Number) : undefined), [key]);
  useSubscription("quote", ids, DEFAULT_CONFLATE_MS);
  const quotes = useQuoteSnapshot(ids);
  const rows = useMemo(() => (cons.data ? buildRows(cons.data.constituents, quotes) : []), [cons.data, quotes]);
  const denied = useMd(selectDenied(ids?.[0]));

  const err = cons.error;
  let body: ReactNode;
  if (!security) body = <p className="p-3 text-muted">Type an index first, e.g. NIFTY {func}</p>;
  else if (q.isPending) body = <p className="p-3 text-muted">resolving {security}…</p>;
  else if (q.isError) body = <p role="alert" className="p-3 text-down">Instrument lookup failed — {q.error.message}</p>;
  else if (!inst) body = <p role="alert" className="p-3 text-down">No instrument matches {security}{segment ? ` ${segment}` : ""}</p>;
  else if (!isIndex) body = <p role="alert" className="p-3 text-warn">{func} needs an index, e.g. NIFTY {func}</p>;
  else if (err instanceof ApiError && err.code === "NO_CONSTITUENTS")
    body = <p role="alert" className="p-3 text-warn">No constituent data for {inst.symbol} yet (index members and weights arrive with the graph seed)</p>;
  else if (err) body = <p role="alert" className="p-3 text-down">Constituents failed — {err.message}</p>;
  else if (!cons.data) body = <p className="p-3 text-muted">loading constituents…</p>;
  else if (denied) body = <p role="alert" className="p-3 text-warn">{denied}</p>;
  else body = children(rows, cons.data);

  return (
    <section className="h-full flex flex-col" aria-label={`${func} ${security ?? ""}`.trim()}>
      <header className="h-[22px] shrink-0 flex items-center gap-2 px-2 bg-panel-header border-b border-line text-xs whitespace-nowrap overflow-hidden">
        <span className="text-amber font-semibold">{func}</span>
        <span>{inst ? inst.symbol : (security ?? "")}</span>
        {cons.data && (
          <span className="text-muted">
            {cons.data.constituents.length} members · as of {cons.data.as_of}
          </span>
        )}
        {inst && <MarketTags id={inst.id} />}
      </header>
      <div className="flex-1 min-h-0 overflow-auto">{body}</div>
    </section>
  );
}
