# Terminal UI Spec

Goal: a dense, keyboard-first terminal that feels like an institutional workstation. It is inspired by
Bloomberg's interaction model (command line, function mnemonics, linked panels, amber-on-black) and
must not copy Bloomberg's name, logo, trademarked function codes or screens.

## 1. Screen anatomy
```
┌──────────────────────────────────────────────────────────────────────────────────────┐
│ VYUHA ▸ [command bar: RELIANCE EQ CH_______________________]  ⌘K  14:32:07 IST  MEERA │  ← top bar (28px)
├──────────────────────────────────────────────────────────────────────────────────────┤
│ ┌─[amber]─ CH RELIANCE 1m ──────────┐ ┌─[amber]─ DOM RELIANCE ─┐ ┌─[cyan]─ OC NIFTY ─┐ │
│ │                                   │ │ bid      qty  ask       │ │ CE  strike  PE    │ │
│ │        candles + volume           │ │ ...                     │ │ ...               │ │
│ └───────────────────────────────────┘ └─────────────────────────┘ └───────────────────┘ │
│ ┌─ POS ─────────────────────────────────────────────┐ ┌─ AI ─────────────────────────┐ │
│ │ acct  instr  qty  avg  ltp  mtm  Δ₹  Θ₹  ...      │ │ copilot chat + tool traces   │ │
│ └───────────────────────────────────────────────────┘ └──────────────────────────────┘ │
├──────────────────────────────────────────────────────────────────────────────────────┤
│ OPEN ● KITE ZR1234 ● UPSTOX UP5678 ● data 42ms │ engine OK │ P&L −₹3,240 (6% of limit) │ │  ← status bar (22px)
└──────────────────────────────────────────────────────────────────────────────────────┘
```
Sticky status-bar warnings stay until their cause clears, for every user: `ORDER LOG DOWN: new orders blocked` (ADR-027) and `Greeks stale — new option positions blocked` (D-28).

## 2. Command grammar
```
command   := [security] [segment] function [args]
security  := tradingsymbol | symbol | "#"instrument_id
segment   := EQ | FUT | OPT | IDX | NSE | BSE        (optional; disambiguates)
function  := mnemonic from §3 (case-insensitive)
args      := key=value ...                           e.g.  CH interval=5m  OC expiry=next
```
- Typing is fuzzy over instruments (trigram + prefix, ranked by: exact symbol > held positions > watchlist > liquidity) and functions.
- `Enter` opens in the focused panel. `Shift+Enter` opens in a new tab. `Alt+Enter` opens in a popped-out window.
- `F1`/`HELP <FUNC>` opens the function help.
- History: `↑/↓` in an empty bar. `Esc` clears.

## 3. Functions (MVP)
| Mnemonic | Name | Needs security | Description |
|---|---|---|---|
| `Q` | Quote | yes | LTP, change, OHLC, volume, OI, bid/ask, circuit band, 52w range |
| `CH` | Chart | yes | Candles/line, intervals 1m…1M, volume, indicators (SMA/EMA/VWAP/BB/RSI/MACD/SuperTrend), drawings, OI overlay for F&O |
| `DOM` | Depth | yes | 5-level book (30 with Upstox full_d30), click-to-ticket |
| `TS` | Time & sales | yes | Tick prints from stream |
| `WL` | Watchlist | no | Multiple named lists; columns configurable |
| `OC` | Option chain | underlying | Strikes ± N around ATM; CE/PE LTP, OI, ΔOI, IV, Greeks, volume; PCR, max pain; expiry selector |
| `OI` | OI analysis | underlying | OI and ΔOI by strike, long/short build-up classification, PCR history |
| `PAY` | Payoff | positions/selection | Strategy payoff at expiry and T+0 with Greeks; for staged tickets or current positions |
| `MOV` | Movers | index | Gainers/losers/volume shockers/OI gainers within index |
| `HM` | Heatmap | index | Sector/constituent heatmap by % change, sized by weight |
| `ORD` | Order ticket | yes | See §5 |
| `BLT` | Blotter | no | Orders + trades, filters by account/trader/strategy/status |
| `POS` | Positions | no | Net positions with MTM, realised, costs, Greeks; group by account/trader/strategy/underlying |
| `PNL` | P&L | no | Intraday P&L curve, by trader/strategy; day/week/month |
| `RISK` | Risk summary | no | Greeks, exposures, VaR, limit utilisation bars, kill/discipline state |
| `SCN` | Scenarios | no | Spot × IV grid heatmap; custom shocks; days-forward |
| `VAR` | VaR | no | Historical/parametric VaR, component VaR by trader/underlying, P&L distribution |
| `STR` | Stress | no | Named stress scenarios table |
| `FAC` | Factors | no | Factor exposures, VaR contribution, specific risk |
| `EXP` | Exposure | no | Sector/underlying/beta-delta exposure treemap |
| `LIM` | Limits | no | View (trader) / edit (risk manager) limits |
| `KILL` | Kill switch | no | Same as Ctrl+Shift+K. The dialog shows progress while cancels go out, e.g. `cancelling 12/20` (ADR-028) |
| `N` | News | optional | Stories; "affects me" filter; extraction tags; sentiment |
| `CTX` | Context graph | yes or "mine" | Graph centred on a security/position with impact paths (Cytoscape) |
| `CAL` | Events calendar | optional | Results, dividends, policy dates, expiries, holidays |
| `AI` | Copilot | optional | Chat with tool traces; brief viewer; `@agent` mentions |
| `DESK` | Desk agents | no | Agent roster (status, runs, cost), activity feed of notes/alerts, feedback |
| `DL` | Data downloads | no | Job builder + progress |
| `STRAT` | Strategies | no | List, editor (Monaco), versions, lifecycle actions |
| `BT` | Backtests | optional strategy | Run config, sweep/walk-forward, report (equity, DD, trades, costs) |
| `ALRT` | Alerts | no | Rules + inbox |
| `AUD` | Audit | no (risk+) | Audit log viewer + verify + export |
| `COMP` | Compliance | no (risk+) | Daily compliance report |
| `ADM` | Admin | no (admin) | Users, accounts, IPs, feeds, costs |
| `SYS` | System | no | Service health, feed status, latency histograms |
| `HELP` | Help | optional | Function docs & keyboard map |

Monaco editor in `STRAT` uses `@monaco-editor/react`, with Python language support and SDK type stubs for completions.

## 4. Keyboard map (global)
| Keys | Action |
|---|---|
| `` ` `` or `Ctrl+K` | Focus command bar |
| `Ctrl+1…9` | Focus panel N |
| `Ctrl+Tab` / `Ctrl+Shift+Tab` | Next/prev tab in panel |
| `Ctrl+W` | Close tab |
| `Ctrl+L` then `A/C/M/G/0` | Set panel link group amber/cyan/magenta/green/none |
| `B` / `S` | Buy/Sell ticket for selected row/security (when a grid or chart has focus) |
| `Ctrl+Enter` | Submit ticket |
| `Esc` | Close dialog / cancel ticket |
| `Ctrl+Shift+X` | Cancel all my open orders (confirm) |
| `Ctrl+Shift+K` | Kill switch dialog |
| `Ctrl+Shift+F` | Flatten selected position (confirm) |
| `Ctrl+S` | Save layout |
| `Ctrl+/` | Keyboard help |
| `Alt+P` | Toggle paper/live default for ticket (if role allows live) |

## 5. Order ticket
- Fields in tab order: Account → Side → Qty (lots, with units shown) → Type → Price → Trigger → Product → Validity → Bracket (SL, Target) → Submit.
- Price can be nudged with `↑/↓` by one tick, or `Shift+↑/↓` by 10 ticks. `L` sets LTP, `B` best bid, `A` best ask.
- **Live preview** (debounced 150 ms via `/orders/preview`): notional, margin required/available, gate pass/fail with reason codes, limit headroom bars, reward:risk, slices count, estimated costs.
- Header colour: live BUY green border, live SELL red border, paper = cyan border with a `PAPER` tag.
- The ticket stays open with errors on rejection. On success it closes and focus returns to the source panel.
- A short option refused with `R-SHORT-OPT:no-cover` shows "buy the hedge first": working orders don't count as cover, so the hedge has to be filled before the short is sent (D-31).
- AI-drafted tickets show an `AI DRAFT` badge and the copilot's rationale (collapsible). The human must review and submit.

## 6. Visual design tokens
```css
:root {
  --bg: #000000; --panel: #0A0A0A; --panel-header: #141414; --grid: #1E1E1E; --border: #2A2A2A;
  --text-primary: #FFB000;   /* amber */
  --text-secondary: #C8C8C8; --text-muted: #8A8A8A;
  --up: #00D26A; --down: #FF3B3B; --flat: #C8C8C8;
  --accent: #3AB8FF; --warn: #FFD400; --paper: #00E5FF; --kill: #FF1744;
  --link-amber: #FFB000; --link-cyan: #00E5FF; --link-magenta: #FF4FD8; --link-green: #00D26A;
  --font-mono: "JetBrains Mono", ui-monospace, monospace;
  --font-label: "IBM Plex Sans Condensed", system-ui, sans-serif;
  --fs-base: 12px; --row-h: 18px; --header-h: 22px;
}
```
- Numbers use `font-variant-numeric: tabular-nums`. Indian digit grouping (₹12,34,567.50) via `Intl.NumberFormat('en-IN')`.
- Tick flash: background `rgba(0,210,106,.25)` / `rgba(255,59,59,.25)` for 150 ms, via CSS class toggled in rAF.
- Stale (> 3 s since last tick): opacity 0.5 and a `STALE` tag in the panel header.
- Light theme: not in MVP.

## 7. Performance architecture
- WebSocket in a **dedicated Web Worker**. It decodes protobuf, merges per-key latest values, and posts a batched diff to the main thread once per animation frame (structured clone of compact typed arrays).
- Zustand store keyed by instrument id. Components subscribe with selectors to avoid re-renders.
- AG Grid uses `applyTransactionAsync` with `getRowId`. Cell renderers are plain functions (no React components) in hot columns.
- Charts: Lightweight Charts `update()` for the last bar only.
- Hidden panels (inactive tabs) unsubscribe after 10 s and resubscribe on show.
- Perf budget test (Playwright): 1,000 rows × 4 Hz updates for 60 s, ≥ 55 fps average and no long tasks > 50 ms.

## 8. Default workspaces by role
- **trader:** CH (amber) + DOM (amber) + OC (cyan) / POS + BLT / AI
- **risk_manager:** RISK (desk) + SCN / POS (group by trader) + BLT (all) / CTX(mine) + N
- **analyst:** STRAT + BT / CH + DL / AI

## 9. Multi-monitor
- Pop-out uses `window.open`. Windows coordinate via `BroadcastChannel('vyuha')` for link-group changes and layout. Only the main window holds the WebSocket; pop-outs get data over BroadcastChannel from the main window's worker.
- When the main window closes, pop-outs show "main window closed — reopen" and stop updating.

## 10. Accessibility & safety
- Visible focus ring (2 px `--accent`).
- Contrast: amber `#FFB000` on black ≈ 11:1 (passes AA).
- Every destructive or live action shows a confirmation naming the account, `LIVE`/`PAPER`, and the order summary.
