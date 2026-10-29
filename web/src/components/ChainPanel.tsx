import { useMemo, useState, type ReactNode } from "react";
import type { PanelProps } from "../commands/types";
import { ApiError } from "../lib/api/client";
import { useInstrument } from "../lib/api/marketdata";
import { useExpiries, useOptionChain } from "../lib/api/optionchain";
import type { Instrument, OptionChain } from "../lib/api/types";
import { formatPrice } from "../lib/format/num";
import { decimalUnits, pickExpiry } from "../lib/optionchain";
import { useSubscription } from "../lib/ws/client";
import { useMd } from "../stores/md";
import { MarketTags } from "./MarketPanel";

/** Quote conflation for chain contracts: the table renders from the REST poll, the stream only keeps
 *  the engine subscribed, so a slow cadence keeps the socket quiet. */
const CHAIN_CONFLATE_MS = 1_000;

/** Options and futures open the chain of their underlying. */
function underlyingOf(i: Instrument): number {
  return (i.segment === "OPT" || i.segment === "FUT") && i.underlying_id !== undefined ? i.underlying_id : i.id;
}

/** The underlying, its future and every contract of the expiry (not just the rows shown). */
function chainIds(c: OptionChain): number[] {
  const ids = [c.underlying.instrument_id, ...c.contract_ids];
  if (c.underlying.future_id !== undefined) ids.push(c.underlying.future_id);
  return ids;
}

const FORWARD_LABEL = { future: "fut", synthetic_mid: "syn", synthetic_ltp: "syn·ltp", spot: "spot" } as const;

function Stats({ c }: { c: OptionChain }) {
  const item = (label: string, value: string) => (
    <span>
      <span className="text-muted">{label}</span> {value}
    </span>
  );
  return (
    <div className="flex flex-wrap items-center gap-x-3 px-2 py-0.5 text-xs tabular-nums whitespace-nowrap border-b border-grid" aria-label="Chain summary">
      <span title={`Put OI / call OI over every strike of the expiry with OI (${c.oi_strikes ?? 0} strikes), not just the rows shown`}>
        <span className="text-muted">PCR (all strikes)</span> {c.pcr === undefined ? "—" : c.pcr.toFixed(2)}
      </span>
      {item("Max pain (all strikes)", formatPrice(decimalUnits(c.max_pain)))}
      {item("ATM", formatPrice(decimalUnits(c.atm_strike)))}
      {item(
        `Fwd${c.forward_source ? ` (${FORWARD_LABEL[c.forward_source]})` : ""}`,
        c.forward === undefined ? "—" : formatPrice(Math.round(c.forward * 10_000)),
      )}
      {item("Lot", String(c.lot_size))}
      {c.analytics === "unavailable" && <span className="border border-warn text-warn px-1">NO ANALYTICS</span>}
    </div>
  );
}

interface Props extends PanelProps {
  children: (chain: OptionChain) => ReactNode;
}

/**
 * Frame for underlying-level panels (OC, OI): resolves the underlying, picks the expiry (`expiry=`
 * date | next; nearest by default), polls the chain and subscribes its contracts on the stream so the
 * engine carries them (the gateway only reads latest values). Shows not-found / no-entitlement /
 * closed states like MarketPanel.
 */
export function ChainPanel({ func, security, segment, args, children }: Props) {
  const q = useInstrument(security, segment);
  const inst = q.data ?? undefined;
  const underlyingId = inst ? underlyingOf(inst) : undefined;
  const expiries = useExpiries(underlyingId);
  const [chosen, setChosen] = useState<string | undefined>();
  const expiry = chosen ?? pickExpiry(args["expiry"], expiries.data ?? []);
  const strikes = Math.min(Math.max(Number(args["strikes"]) || 20, 1), 50);
  const market = useMd((s) => s.system?.marketState);
  const chain = useOptionChain(underlyingId, expiry, strikes, market === "open" || market === "pre_open");

  const key = chain.data ? chainIds(chain.data).join(",") : "";
  const ids = useMemo(() => (key ? key.split(",").map(Number) : undefined), [key]);
  useSubscription("quote", ids, CHAIN_CONFLATE_MS);

  const err = chain.error;
  let body: ReactNode;
  if (!security) body = <p className="p-3 text-muted">Type an underlying first, e.g. NIFTY {func}</p>;
  else if (q.isPending) body = <p className="p-3 text-muted">resolving {security}…</p>;
  else if (q.isError) body = <p role="alert" className="p-3 text-down">Instrument lookup failed — {q.error.message}</p>;
  else if (!inst) body = <p role="alert" className="p-3 text-down">No instrument matches {security}{segment ? ` ${segment}` : ""}</p>;
  else if (expiries.isPending) body = <p className="p-3 text-muted">loading expiries…</p>;
  else if (!expiry) body = <p role="alert" className="p-3 text-warn">No listed options for {inst.symbol}</p>;
  else if (err instanceof ApiError && err.code === "MD_NOT_ENTITLED") body = <p role="alert" className="p-3 text-warn">{err.message}</p>;
  else if (err && !chain.data) body = <p role="alert" className="p-3 text-down">Option chain failed — {err.message}</p>;
  else if (!chain.data) body = <p className="p-3 text-muted">loading chain…</p>;
  else body = children(chain.data);

  return (
    <section className="h-full flex flex-col" aria-label={`${func} ${security ?? ""}`.trim()}>
      <header className="h-[22px] shrink-0 flex items-center gap-2 px-2 bg-panel-header border-b border-line text-xs whitespace-nowrap overflow-hidden">
        <span className="text-amber font-semibold">{func}</span>
        <span>{inst ? inst.symbol : (security ?? "")}</span>
        {expiries.data && expiries.data.length > 0 && expiry && (
          <select
            aria-label="Expiry"
            className="bg-panel border border-line px-1 focus-visible:outline focus-visible:outline-accent"
            value={expiry}
            onChange={(e) => setChosen(e.target.value)}
          >
            {expiries.data.map((d) => (
              <option key={d} value={d}>
                {d}
              </option>
            ))}
          </select>
        )}
        {underlyingId !== undefined && <MarketTags id={underlyingId} />}
      </header>
      {chain.data && !err && <Stats c={chain.data} />}
      <div className="flex-1 min-h-0 overflow-auto">{body}</div>
    </section>
  );
}
