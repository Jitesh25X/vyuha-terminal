import { create, type MessageInitShape, toBinary } from "@bufbuild/protobuf";
import { describe, expect, it } from "vitest";
import { TickSchema } from "../../gen/vyuha/marketdata/v1/marketdata_pb";
import { Channel, ServerMsgSchema } from "../../gen/vyuha/ws/v1/ws_pb";
import { toastLevel } from "../../stores/toasts";
import { alertMessage, idsIn, Merger } from "./decode";

const tick = (id: number, ltp: number, tsMs: number, ltq = 1) =>
  create(TickSchema, {
    instrumentId: BigInt(id),
    ltp: BigInt(ltp),
    ltq,
    prevClose: 1_000_000n,
    exchangeTsNs: BigInt(tsMs) * 1_000_000n,
    bids: [{ price: BigInt(ltp - 500), qty: 10, orders: 2 }],
  });

const updates = (...ticks: ReturnType<typeof tick>[]) =>
  toBinary(
    ServerMsgSchema,
    create(ServerMsgSchema, {
      body: {
        case: "updates",
        value: { channel: Channel.QUOTE, items: ticks.map((t) => ({ value: { case: "quote" as const, value: { tick: t, changePct: 1.5 } } })) },
      },
    }),
  );

describe("Merger: outage signal", () => {
  const frame = (body: NonNullable<MessageInitShape<typeof ServerMsgSchema>["body"]>) => toBinary(ServerMsgSchema, create(ServerMsgSchema, { body }));

  it("turns an alert frame into one toast's worth of text", () => {
    const m = new Merger();
    m.push(frame({ case: "alert", value: { id: "ORDER_LOG_OUTAGE:1", severity: "critical", title: "ORDER_LOG_OUTAGE", bodyJson: '{"message":"ORDER LOG DOWN: new orders blocked"}' } }));
    expect(m.empty).toBe(false);
    expect(m.take().alerts).toEqual([{ id: "ORDER_LOG_OUTAGE:1", severity: "critical", message: "ORDER LOG DOWN: new orders blocked" }]);
    expect(m.take().alerts).toEqual([]);
  });

  it("carries the status bar banners and shows none once they clear", () => {
    const m = new Merger();
    const status = (banners: string[]) => frame({ case: "updates", value: { channel: Channel.SYSTEM, items: [{ value: { case: "system", value: { marketState: "open", engineOk: true, banners } } }] } });
    m.push(status(["ORDER LOG DOWN: new orders blocked"]));
    expect(m.take().system?.banners).toEqual(["ORDER LOG DOWN: new orders blocked"]);
    m.push(status([]));
    expect(m.take().system?.banners).toEqual([]);
  });

  it("falls back to the title when the body has no message", () => {
    expect(alertMessage("X", "{}")).toBe("X");
    expect(alertMessage("X", "not json")).toBe("X");
  });

  it("never plays down an unknown severity", () => {
    expect(toastLevel("critical")).toBe("error");
    expect(toastLevel("???")).toBe("error");
    expect(toastLevel("warn")).toBe("warn");
  });
});

describe("Merger", () => {
  it("keeps the latest quote per instrument and turns new trades into prints", () => {
    const m = new Merger();
    m.push(updates(tick(1, 1_010_000, 1000), tick(2, 2_000_000, 1000)));
    m.push(updates(tick(1, 1_020_000, 2000)));
    const d = m.take();
    expect(d.quotes.map((q) => [q.id, q.ltp])).toEqual([
      [1, 1_020_000],
      [2, 2_000_000],
    ]);
    expect(d.quotes[0]!.changePct).toBe(1.5);
    expect(d.prints.map((p) => [p.id, p.price, p.side])).toEqual([
      [1, 1_010_000, "flat"],
      [2, 2_000_000, "flat"],
      [1, 1_020_000, "up"],
    ]);
    expect(m.empty).toBe(true);
    // The same trade again (no new exchange time) is not a new print.
    m.push(updates(tick(1, 1_020_000, 2000)));
    expect(m.take().prints).toEqual([]);
  });

  it("decodes errors with entitlement ids, and ignores junk frames", () => {
    const m = new Merger();
    const err = toBinary(
      ServerMsgSchema,
      create(ServerMsgSchema, { body: { case: "error", value: { code: "MD_NOT_ENTITLED", message: "No market-data entitlement [7,9]" } } }),
    );
    expect(m.push(err)).toBe(true);
    expect(m.push(new Uint8Array([0xff, 0xff, 0xff]))).toBe(false);
    expect(m.take().errors).toEqual([{ code: "MD_NOT_ENTITLED", message: "No market-data entitlement [7,9]", ids: [7, 9] }]);
  });

  it("parses id lists", () => {
    expect(idsIn("x [1,22,333]")).toEqual([1, 22, 333]);
    expect(idsIn("no ids")).toEqual([]);
  });
});
