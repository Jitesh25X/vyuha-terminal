// Messages between the main thread and the market-data worker (lib/ws/worker.ts). Everything is
// plain numbers/strings so postMessage's structured clone stays cheap. Prices are int64 units
// (₹1 = 10,000) carried as numbers: exact up to ₹900 billion, far above any traded price.

export type Channel = "quote" | "depth" | "bar.1m" | "system";

/** Latest quote for one instrument (from Tick/Snapshot, depth stripped). */
export interface QuoteLite {
  id: number;
  ltp: number;
  ltq: number;
  changePct: number;
  open: number;
  high: number;
  low: number;
  prevClose: number;
  avgPrice: number;
  volume: number;
  oi: number;
  bid: number;
  ask: number;
  bidQty: number;
  askQty: number;
  exchangeTs: number; // epoch ms
  recvTs: number; // epoch ms (engine receive)
}

export interface LevelLite {
  price: number;
  qty: number;
  orders: number;
}

export interface DepthLite {
  id: number;
  bids: LevelLite[];
  asks: LevelLite[];
  ltp: number;
  exchangeTs: number;
}

export interface BarLite {
  id: number;
  ts: number; // bar open, epoch ms
  open: number;
  high: number;
  low: number;
  close: number;
  volume: number;
  final: boolean;
}

/** A trade print for TS: emitted when a quote update carries a new trade (ltq > 0, new exchange time). */
export interface PrintLite {
  id: number;
  ts: number;
  price: number;
  qty: number;
  side: "up" | "down" | "flat";
}

export interface FeedLite {
  accountId: string;
  broker: "zerodha" | "upstox" | "sim" | "unknown";
  label: string;
  state: "connecting" | "connected" | "reconnecting" | "down" | "idle";
  subscribed: number;
}

export interface SystemLite {
  marketState: string; // pre_open | open | closed | holiday
  engineOk: boolean;
  dataLatencyMs: number;
  feeds: FeedLite[];
}

export type ConnState = "connecting" | "open" | "closed";

export interface WsError {
  code: string;
  message: string;
  /** Instrument ids named in MD_NOT_ENTITLED messages ("… [1,2]"). */
  ids: number[];
}

/** One frame's worth of changes, posted at most once per animation frame. */
export interface Diff {
  quotes: QuoteLite[];
  depth: DepthLite[];
  bars: BarLite[];
  prints: PrintLite[];
  system?: SystemLite;
  errors: WsError[];
  conn?: ConnState;
  /** The server closed with 4401: the main thread refreshes the access token and restarts. */
  authExpired?: boolean;
}

export type ToWorker =
  | { type: "start"; url: string; token: string }
  | { type: "token"; token: string }
  | { type: "sub"; channel: Channel; ids: number[]; conflateMs: number }
  | { type: "unsub"; channel: Channel; ids: number[] }
  | { type: "stop" };

export type FromWorker = { type: "diff"; diff: Diff };
