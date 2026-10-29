import { useState } from "react";
import type { PanelProps } from "../../commands/types";
import { ChainPanel } from "../../components/ChainPanel";
import { formatIstTime } from "../../lib/format/time";
import { ChainTable } from "./ChainTable";

/** OC <underlying> [expiry=YYYY-MM-DD|next] [strikes=N] [greeks=all] — F-MD-3. */
export default function OptionChainPanel(props: PanelProps) {
  const [greeks, setGreeks] = useState(props.args["greeks"] === "all");
  return (
    <ChainPanel {...props}>
      {(chain) => (
        <div className="flex flex-col">
          <div className="flex items-center gap-3 px-2 py-0.5 text-xs text-muted whitespace-nowrap">
            <button
              type="button"
              aria-pressed={greeks}
              onClick={() => setGreeks((g) => !g)}
              className="border border-line px-1 hover:text-fg focus-visible:outline focus-visible:outline-accent"
            >
              {greeks ? "Hide Γ Θ ν" : "Show Γ Θ ν"}
            </button>
          </div>
          <ChainTable chain={chain} greeks={greeks} />
          <footer
            aria-label="Chain footer"
            className="sticky bottom-0 flex flex-wrap gap-x-3 px-2 py-0.5 text-xs text-muted bg-panel border-t border-line whitespace-nowrap"
          >
            <span>{chain.model === "black76" ? "Black-76" : chain.model === "bsm" ? "BSM" : "no model"}</span>
            <span title="Risk-free rate (RISK_FREE_RATE) and the date it took effect: check it is current">
              r {chain.rate === undefined ? "—" : `${(chain.rate * 100).toFixed(2)}%`}
              {chain.rate_as_of && ` (as of ${chain.rate_as_of})`}
            </span>
            <span>PCR (all strikes{chain.oi_strikes !== undefined ? `, ${chain.oi_strikes} with OI` : ""})</span>
            <span title="IV is solved from the mid when the spread is tight, else from an LTP that traded within the last minute (L), else not shown (—)">
              IV: mid · <span className="text-warn">L</span> LTP · — none
            </span>
            <span className="ml-auto">as of {formatIstTime(new Date(chain.as_of))}</span>
          </footer>
        </div>
      )}
    </ChainPanel>
  );
}
