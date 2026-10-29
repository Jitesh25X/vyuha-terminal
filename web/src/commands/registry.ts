import type { FunctionDef, Needs } from "./types";

// Function registry (UI_SPEC §3). Panels not built yet open the shared stub; each later task
// replaces its `load` with panels/<FUNC>/index.tsx and adds a keyboard-only Playwright test.

const stub = () => import("../panels/stub");

function fn(mnemonic: string, title: string, needs: Needs, help: string, load: FunctionDef["load"] = stub): FunctionDef {
  return { mnemonic, title, needs, help, load };
}

export const FUNCTIONS: readonly FunctionDef[] = [
  fn("Q", "Quote", "security", "LTP, change, OHLC, volume, OI, bid/ask, last trade. e.g. RELIANCE Q", () => import("../panels/Q")),
  fn("CH", "Chart", "security", "Candles + volume from history, live 1-minute bars. CH interval=1m|5m|15m|1h|1d", () => import("../panels/CH")),
  fn("DOM", "Depth", "security", "5-level book (30 with Upstox full_d30)", () => import("../panels/DOM")),
  fn("TS", "Time & sales", "security", "Trade prints from the stream (at 50 ms conflation)", () => import("../panels/TS")),
  fn("WL", "Watchlist", "none", "Named lists (WL name=fno). Type a symbol in the box + Enter to add; Delete removes the focused row", () => import("../panels/WL")),
  fn("OC", "Option chain", "underlying", "Strikes ± N around ATM; CE/PE LTP, OI, ΔOI, IV, Greeks, volume; PCR, max pain. OC expiry=next|YYYY-MM-DD strikes=20 greeks=all", () => import("../panels/OC")),
  fn("OI", "OI analysis", "underlying", "OI and ΔOI by strike, build-up classification, PCR history (this session). OI expiry=next strikes=20", () => import("../panels/OI")),
  fn("PAY", "Payoff", "optional", "Strategy payoff at expiry and T+0 with Greeks"),
  fn("MOV", "Movers", "index", "Gainers/losers/volume shockers/OI gainers within index"),
  fn("HM", "Heatmap", "index", "Sector/constituent heatmap by % change, sized by weight"),
  fn("ORD", "Order ticket", "security", "Order ticket with live risk preview"),
  fn("BLT", "Blotter", "none", "Orders + trades, filters by account/trader/strategy/status"),
  fn("POS", "Positions", "none", "Net positions with MTM, realised, costs, Greeks"),
  fn("PNL", "P&L", "none", "Intraday P&L curve, by trader/strategy; day/week/month"),
  fn("RISK", "Risk summary", "none", "Greeks, exposures, VaR, limit utilisation, kill/discipline state"),
  fn("SCN", "Scenarios", "none", "Spot × IV grid heatmap; custom shocks; days-forward"),
  fn("VAR", "VaR", "none", "Historical/parametric VaR, component VaR, P&L distribution"),
  fn("STR", "Stress", "none", "Named stress scenarios table"),
  fn("FAC", "Factors", "none", "Factor exposures, VaR contribution, specific risk"),
  fn("EXP", "Exposure", "none", "Sector/underlying/beta-delta exposure treemap"),
  fn("LIM", "Limits", "none", "View (trader) / edit (risk manager) limits"),
  fn("KILL", "Kill switch", "none", "Same as Ctrl+Shift+K"),
  fn("N", "News", "optional", "Stories; affects-me filter; extraction tags; sentiment"),
  fn("CTX", "Context graph", "security", "Graph centred on a security/position with impact paths"),
  fn("CAL", "Events calendar", "optional", "Results, dividends, policy dates, expiries, holidays"),
  fn("AI", "Copilot", "optional", "Chat with tool traces; brief viewer; @agent mentions"),
  fn("DESK", "Desk agents", "none", "Agent roster, activity feed of notes/alerts, feedback"),
  fn("DL", "Data downloads", "none", "Job builder + progress"),
  fn("STRAT", "Strategies", "none", "List, editor, versions, lifecycle actions"),
  fn("BT", "Backtests", "optional", "Run config, sweep/walk-forward, report"),
  fn("ALRT", "Alerts", "none", "Rules + inbox"),
  fn("AUD", "Audit", "none", "Audit log viewer + verify + export (risk+)"),
  fn("COMP", "Compliance", "none", "Daily compliance report (risk+)"),
  fn("ADM", "Admin", "none", "Users, accounts, IPs, feeds, costs (admin)"),
  fn("SYS", "System", "none", "Service health, feed status, latency histograms"),
  fn("HELP", "Help", "optional", "Function docs & keyboard map. HELP <FUNC> opens one function", () => import("../panels/HELP")),
];

const byMnemonic = new Map(FUNCTIONS.map((f) => [f.mnemonic, f]));

export function findFunction(mnemonic: string): FunctionDef | undefined {
  return byMnemonic.get(mnemonic.toUpperCase());
}
