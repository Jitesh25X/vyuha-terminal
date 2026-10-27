# web/ — React terminal

Read `docs/UI_SPEC.md`, `docs/API.md §8` and the root `CLAUDE.md` first.

## Layout
```
src/
  app/            routes (login, mfa, terminal), providers
  shell/          TopBar, CommandBar, StatusBar, Workspace (Dockview), LinkGroups, PopOut
  commands/       function registry: { mnemonic, title, needsSecurity, component, help }
  panels/<FUNC>/  one folder per function (Q, CH, DOM, OC, ORD, POS, RISK, AI, ...)
  components/     shared grid/cell renderers, NumberCell, FlashCell, Sparkline
  lib/ws/         worker.ts (socket + protobuf decode + merge), client.ts (main-thread API)
  lib/api/        typed REST client (generated from openapi.yaml via openapi-typescript)
  lib/format/     price/qty/₹ formatting (en-IN), IST time
  stores/         zustand stores (quotes, orders, positions, risk, ui)
  theme/          tokens.css, fonts
  gen/            buf-generated protobuf (do not edit)
e2e/              Playwright tests incl. perf trace test
```

## Rules
- TS strict, no `any`, no default exports except panels' lazy entry points.
- New function = new folder in `panels/` + entry in `commands/registry.ts` + help text + a Playwright keyboard-only test.
- Hot data never goes through React state per tick. Use store selectors plus AG Grid transactions / chart `update()`.
- Every number uses `lib/format`. Prices come in as int64 units (bigint or number-safe) and are formatted at render.
- Live vs paper must be visually unmistakable (UI_SPEC §5). Danger actions need confirmation.
- No direct `fetch` in components. Use TanStack Query hooks in `lib/api`.
- Accessibility: focus-visible rings, all actions keyboard-reachable.
