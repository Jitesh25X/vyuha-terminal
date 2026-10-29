import {
  CellStyleModule,
  ClientSideRowModelApiModule,
  ClientSideRowModelModule,
  type ColDef,
  type GridApi,
  type GridReadyEvent,
  HighlightChangesModule,
  ModuleRegistry,
  RowApiModule,
  RowSelectionModule,
  themeQuartz,
  type ValueFormatterParams,
} from "ag-grid-community";
import { AgGridReact } from "ag-grid-react";
import { useCallback, useEffect, useMemo, useRef, useState, type KeyboardEvent } from "react";
import type { PanelProps } from "../../commands/types";
import { resolveSecurity } from "../../lib/api/marketdata";
import { direction, formatPct, formatPrice, formatQty, formatVolume } from "../../lib/format/num";
import { onQuotes, useSubscription } from "../../lib/ws/client";
import type { QuoteLite } from "../../lib/ws/protocol";
import { useMd } from "../../stores/md";
import { useSession } from "../../stores/session";
import { loadWatchlist, saveWatchlist, type WatchItem } from "./storage";

ModuleRegistry.registerModules([
  ClientSideRowModelModule,
  ClientSideRowModelApiModule,
  RowApiModule,
  RowSelectionModule,
  HighlightChangesModule,
  CellStyleModule,
]);

interface Row {
  id: number;
  sym: string;
  ltp?: number;
  chg?: number;
  bid?: number;
  ask?: number;
  vol?: number;
  oi?: number;
}

// Plain-function formatters and class rules (UI_SPEC §7: no React components in hot columns).
const price = (p: ValueFormatterParams<Row, number>) => formatPrice(p.value ?? undefined);
const signClass = { "wl-up": (p: { value: number | undefined }) => direction(p.value) === "up", "wl-down": (p: { value: number | undefined }) => direction(p.value) === "down" };

const COLUMNS: ColDef<Row>[] = [
  { field: "sym", headerName: "Symbol", flex: 2, minWidth: 110, cellClass: "text-amber" },
  { field: "ltp", headerName: "LTP", flex: 1, type: "rightAligned", valueFormatter: price, enableCellChangeFlash: true },
  { field: "chg", headerName: "Chg %", flex: 1, type: "rightAligned", valueFormatter: (p) => formatPct(p.value ?? undefined), cellClassRules: signClass },
  { field: "bid", headerName: "Bid", flex: 1, type: "rightAligned", valueFormatter: price },
  { field: "ask", headerName: "Ask", flex: 1, type: "rightAligned", valueFormatter: price },
  { field: "vol", headerName: "Volume", flex: 1, type: "rightAligned", valueFormatter: (p) => formatVolume(p.value ?? undefined) },
  { field: "oi", headerName: "OI", flex: 1, type: "rightAligned", valueFormatter: (p) => (p.value ? formatQty(p.value) : "") },
];

const theme = themeQuartz.withParams({
  backgroundColor: "#0a0a0a",
  foregroundColor: "#c8c8c8",
  headerBackgroundColor: "#141414",
  headerTextColor: "#8a8a8a",
  borderColor: "#2a2a2a",
  accentColor: "#ffb000",
  rowHoverColor: "#141414",
  fontFamily: "var(--font-mono)",
  fontSize: 12,
  rowHeight: 18,
  headerHeight: 22,
  spacing: 3,
  wrapperBorderRadius: 0,
});

const toRow = (q: QuoteLite, sym: string): Row => ({ id: q.id, sym, ltp: q.ltp, chg: q.changePct, bid: q.bid, ask: q.ask, vol: q.volume, oi: q.oi });

export default function WatchlistPanel({ args }: PanelProps) {
  const name = args["name"] ?? "default";
  const user = useSession((s) => (s.state.status === "authenticated" ? s.state.user.id : ""));
  const [items, setItems] = useState<WatchItem[]>(() => loadWatchlist(user, name));
  const [adding, setAdding] = useState("");
  const [error, setError] = useState("");
  const api = useRef<GridApi<Row> | null>(null);
  const symbols = useRef(new Map<number, string>());
  symbols.current = new Map(items.map((i) => [i.id, i.sym]));

  const ids = useMemo(() => items.map((i) => i.id), [items]);
  useSubscription("quote", ids);
  const denied = useMd((s) => ids.filter((id) => s.denied[id] !== undefined).length);
  const deniedMsg = useMd((s) => (denied > 0 ? Object.values(s.denied)[0] : undefined));

  useEffect(() => saveWatchlist(user, name, items), [user, name, items]);

  // Live updates go straight to the grid as async transactions, never through React state.
  useEffect(
    () =>
      onQuotes((qs) => {
        const update: Row[] = [];
        for (const q of qs) {
          const sym = symbols.current.get(q.id);
          if (sym !== undefined) update.push(toRow(q, sym));
        }
        if (update.length > 0) api.current?.applyTransactionAsync({ update });
      }),
    [],
  );

  const rowData = useMemo(() => {
    const quotes = useMd.getState().quotes;
    return items.map((i) => {
      const q = quotes[i.id];
      return q ? toRow(q, i.sym) : { id: i.id, sym: i.sym };
    });
  }, [items]);

  const add = useCallback(async () => {
    const text = adding.trim().toUpperCase();
    if (!text) return;
    const [security, segment] = text.split(/\s+/);
    setError("");
    const inst = await resolveSecurity(security!, segment).catch(() => null);
    if (!inst) {
      setError(`No instrument matches ${text}`);
      return;
    }
    setItems((cur) => (cur.some((i) => i.id === inst.id) ? cur : [...cur, { id: inst.id, sym: inst.tradingsymbol }]));
    setAdding("");
  }, [adding]);

  const onReady = useCallback((e: GridReadyEvent<Row>) => {
    api.current = e.api;
  }, []);

  const onGridKey = (e: KeyboardEvent) => {
    if (e.key !== "Delete" && e.key !== "Backspace") return;
    const focused = api.current?.getFocusedCell();
    const row = focused ? api.current?.getDisplayedRowAtIndex(focused.rowIndex)?.data : undefined;
    if (row) setItems((cur) => cur.filter((i) => i.id !== row.id));
  };

  return (
    <section className="h-full flex flex-col" aria-label={`WL ${name}`}>
      <header className="h-[22px] shrink-0 flex items-center gap-2 px-2 bg-panel-header border-b border-line text-xs">
        <span className="text-amber font-semibold">WL</span>
        <span>{name}</span>
        <span className="text-muted">
          {items.length} instrument{items.length === 1 ? "" : "s"}
        </span>
        <input
          aria-label="Add to watchlist"
          className="ml-auto bg-bg border border-line px-1 w-40 focus-visible:outline focus-visible:outline-amber"
          placeholder="add: INFY  ·  NIFTY IDX"
          value={adding}
          onChange={(e) => setAdding(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === "Enter") void add();
          }}
        />
      </header>
      {error && (
        <p role="alert" className="px-2 text-down text-xs">
          {error}
        </p>
      )}
      {denied > 0 && (
        <p role="alert" className="px-2 text-warn text-xs">
          {deniedMsg} ({denied} of {items.length})
        </p>
      )}
      <div className="flex-1 min-h-0" onKeyDown={onGridKey}>
        <AgGridReact<Row>
          theme={theme}
          rowData={rowData}
          columnDefs={COLUMNS}
          getRowId={(p) => String(p.data.id)}
          onGridReady={onReady}
          cellFlashDuration={150}
          cellFadeDuration={100}
          asyncTransactionWaitMillis={50}
          suppressCellFocus={false}
          animateRows={false}
          overlayNoRowsTemplate="Empty watchlist — type a symbol above and press Enter"
        />
      </div>
    </section>
  );
}
