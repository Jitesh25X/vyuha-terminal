import { fromBinary } from "@bufbuild/protobuf";
import { Broker } from "../../gen/vyuha/common/v1/common_pb";
import { type Bar, FeedStatus_State, type Snapshot, type Tick } from "../../gen/vyuha/marketdata/v1/marketdata_pb";
import { Channel as PbChannel, type Item, ServerMsgSchema } from "../../gen/vyuha/ws/v1/ws_pb";
import type { BarLite, Channel, Diff, DepthLite, FeedLite, PrintLite, QuoteLite, SystemLite, WsError } from "./protocol";

// Pure decode + merge for the worker: kept free of WebSocket/DOM so vitest can cover it.

const NS_PER_MS = 1_000_000n;

const n = (v: bigint): number => Number(v);
const ms = (ns: bigint): number => Number(ns / NS_PER_MS);

export function channelName(c: PbChannel): Channel | undefined {
  switch (c) {
    case PbChannel.QUOTE:
      return "quote";
    case PbChannel.DEPTH:
      return "depth";
    case PbChannel.BAR_1M:
      return "bar.1m";
    case PbChannel.SYSTEM:
      return "system";
    default:
      return undefined;
  }
}

export function channelEnum(c: Channel): PbChannel {
  return { quote: PbChannel.QUOTE, depth: PbChannel.DEPTH, "bar.1m": PbChannel.BAR_1M, system: PbChannel.SYSTEM }[c];
}

export function quoteFrom(t: Tick, changePct: number): QuoteLite {
  return {
    id: n(t.instrumentId),
    ltp: n(t.ltp),
    ltq: t.ltq,
    changePct,
    open: n(t.open),
    high: n(t.high),
    low: n(t.low),
    prevClose: n(t.prevClose),
    avgPrice: n(t.avgPrice),
    volume: n(t.volume),
    oi: n(t.oi),
    bid: n(t.bid),
    ask: n(t.ask),
    bidQty: t.bidQty,
    askQty: t.askQty,
    exchangeTs: ms(t.exchangeTsNs),
    recvTs: ms(t.recvTsNs),
  };
}

function depthFrom(t: Tick): DepthLite {
  const lvl = (l: { price: bigint; qty: number; orders: number }) => ({ price: n(l.price), qty: l.qty, orders: l.orders });
  return { id: n(t.instrumentId), bids: t.bids.map(lvl), asks: t.asks.map(lvl), ltp: n(t.ltp), exchangeTs: ms(t.exchangeTsNs) };
}

function barFrom(b: Bar): BarLite {
  return {
    id: n(b.instrumentId),
    ts: ms(b.tsNs),
    open: n(b.open),
    high: n(b.high),
    low: n(b.low),
    close: n(b.close),
    volume: n(b.volume),
    final: b.final,
  };
}

const BROKERS: Record<number, FeedLite["broker"]> = { [Broker.ZERODHA]: "zerodha", [Broker.UPSTOX]: "upstox", [Broker.SIM]: "sim" };
const STATES: Record<number, FeedLite["state"]> = {
  [FeedStatus_State.CONNECTING]: "connecting",
  [FeedStatus_State.CONNECTED]: "connected",
  [FeedStatus_State.RECONNECTING]: "reconnecting",
  [FeedStatus_State.DOWN]: "down",
};

/** Merges server messages into one pending diff: latest value per key, all bars/prints/errors. */
export class Merger {
  private quotes = new Map<number, QuoteLite>();
  private depth = new Map<number, DepthLite>();
  private bars: BarLite[] = [];
  private prints: PrintLite[] = [];
  private errors: WsError[] = [];
  private system: SystemLite | undefined;
  /** Last trade seen per instrument, to turn conflated quotes into prints. */
  private lastTrade = new Map<number, { ts: number; price: number }>();

  get empty(): boolean {
    return this.quotes.size === 0 && this.depth.size === 0 && this.bars.length === 0 && this.errors.length === 0 && !this.system;
  }

  /** Decodes one binary frame. Returns false for frames that aren't a ServerMsg. */
  push(frame: Uint8Array): boolean {
    let msg;
    try {
      msg = fromBinary(ServerMsgSchema, frame);
    } catch {
      return false;
    }
    const b = msg.body;
    switch (b.case) {
      case "snapshot":
        this.items(b.value.items, true);
        break;
      case "updates":
        this.items(b.value.items, false);
        break;
      case "error":
        this.errors.push({ code: b.value.code, message: b.value.message, ids: idsIn(b.value.message) });
        break;
      default:
        break;
    }
    return true;
  }

  private items(items: Item[], snapshot: boolean): void {
    for (const it of items) {
      const v = it.value;
      switch (v.case) {
        case "quote":
          if (v.value.tick) this.quote(v.value, snapshot);
          break;
        case "depth":
          this.depth.set(n(v.value.instrumentId), depthFrom(v.value));
          break;
        case "bar":
          this.bars.push(barFrom(v.value));
          break;
        case "system":
          this.system = {
            marketState: v.value.marketState,
            engineOk: v.value.engineOk,
            dataLatencyMs: n(v.value.dataLatencyMs),
            feeds: v.value.feeds.map((f) => ({
              accountId: f.accountId,
              broker: BROKERS[f.broker] ?? "unknown",
              label: f.label,
              state: STATES[f.state] ?? "idle",
              subscribed: f.subscribed,
            })),
          };
          break;
        default:
          break;
      }
    }
  }

  private quote(s: Snapshot, snapshot: boolean): void {
    const q = quoteFrom(s.tick!, s.changePct);
    this.quotes.set(q.id, q);
    // Conflated updates carry the latest trade only; each new trade becomes one print.
    const prev = this.lastTrade.get(q.id);
    if (q.ltq > 0 && q.exchangeTs > 0 && (!prev || q.exchangeTs > prev.ts)) {
      if (!snapshot) {
        const side = !prev ? "flat" : q.ltp > prev.price ? "up" : q.ltp < prev.price ? "down" : "flat";
        this.prints.push({ id: q.id, ts: q.exchangeTs, price: q.ltp, qty: q.ltq, side });
      }
      this.lastTrade.set(q.id, { ts: q.exchangeTs, price: q.ltp });
    }
  }

  /** Takes the pending diff and resets. */
  take(): Diff {
    const d: Diff = {
      quotes: [...this.quotes.values()],
      depth: [...this.depth.values()],
      bars: this.bars,
      prints: this.prints,
      errors: this.errors,
    };
    if (this.system) d.system = this.system;
    this.quotes = new Map();
    this.depth = new Map();
    this.bars = [];
    this.prints = [];
    this.errors = [];
    this.system = undefined;
    return d;
  }

  forget(ids: number[]): void {
    for (const id of ids) this.lastTrade.delete(id);
  }
}

/** Parses the "[1,2,3]" suffix the gateway appends to MD_NOT_ENTITLED messages. */
export function idsIn(message: string): number[] {
  const m = /\[([\d,]+)\]\s*$/.exec(message);
  return m ? m[1]!.split(",").map(Number).filter((x) => Number.isSafeInteger(x) && x > 0) : [];
}
