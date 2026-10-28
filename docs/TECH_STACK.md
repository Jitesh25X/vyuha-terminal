# Tech Stack

**Install policy:** just-in-time. A dependency is installed only when the first task needing it starts (see each task's `needs:` in `TASKS.md`, PRD §10.1). Prefer minimal features/ports, slim images and CPU-only ML wheels.

**Version policy:** At M0, pin every dependency to the latest stable release on that day. Record the exact
versions in this file's lock table (§6), and keep them pinned in lockfiles (`vcpkg.json` baseline, `go.mod`, `uv.lock`, `pnpm-lock.yaml`, Docker image digests). Upgrade only through a dedicated PR. Majors below are the floor.

## 1. Mapping to Bloomberg / Aladdin

| Concern | Bloomberg / Aladdin (public info) | Vyuha choice |
|---|---|---|
| Terminal UI runtime | Bloomberg's R+ runtime is built on Chromium + Node.js, running JS/TS apps on a C++ infrastructure | React/TS web app; Electron (Chromium + Node) desktop wrapper post-MVP |
| Backend hot path | C++ services | C++23 engine |
| Messaging | BlazingMQ (open-sourced by Bloomberg) | NATS JetStream: simpler ops and first-class clients in all four languages. BlazingMQ was considered (see §5) |
| Market data API | BLPAPI (subscription + request/response) | NATS subjects + KV (subscription), gRPC/REST (request/response) |
| Portfolio risk & context (Aladdin) | Unified risk model across holdings, factor models, scenario analysis, Python "Studio" | Python quant service: Greeks, VaR, scenarios, factor model; Neo4j knowledge graph; Jupyter-friendly SDK |
| AI assistant (Aladdin Copilot / BloombergGPT era) | LLM assistants over firm data | Claude via tool-calling over an MCP server exposing desk data |

## 2. Languages & toolchains

| Area | Choice | Floor | Why |
|---|---|---|---|
| Engine | C++23 | GCC 14 or Clang 18 | `std::expected`, `std::span`, concepts. Deterministic, no GC pauses. Same code in backtest via bindings |
| Build | CMake ≥ 3.28 + Ninja + vcpkg (manifest mode) | | Standard, reproducible, CI-friendly |
| Gateway | Go (latest stable, ≥ 1.24) | | Excellent concurrency for WS fan-out, small static binaries; owner already knows Go |
| Quant / AI | Python 3.13, managed by **uv** | | Fast, lockfile-based; the scientific + LLM ecosystem lives here |
| Frontend | TypeScript ≥ 5.6, Node 22 LTS, **pnpm** | | Strict typing; pnpm for monorepo speed |
| Contracts | Protobuf + **Buf** CLI | | Lint + breaking-change detection + multi-language codegen |

## 3. Libraries by service

### engine (C++)
| Need | Library | Notes |
|---|---|---|
| Async IO, HTTP/1.1 client, WebSocket client | Boost.Asio + Boost.Beast | One `io_context` per connection group; TLS via OpenSSL 3 |
| TLS | OpenSSL 3 | |
| JSON (broker REST) | simdjson (ondemand) | Zero-copy parse |
| Protobuf | protobuf (vcpkg) | Upstox V3 feed + our contracts |
| RPC | gRPC C++ | `engine.v1.EngineService` |
| Bus | nats.c | JetStream publish + KV |
| Postgres (event writer) | libpq (via `libpqxx`) | Writer thread only, never on core thread |
| Logging | Quill | Async, low-latency |
| Formatting | fmt (bundled with Quill) | |
| Lock-free queues | Own `SpscRing<T,N>` (cache-line padded) + `moodycamel::ConcurrentQueue` for MPSC | Own SPSC is a good hand-written learning module |
| Python bindings | **nanobind** | Faster, smaller than pybind11 |
| Tests / bench | GoogleTest, GoogleMock, Google Benchmark, RapidCheck (property tests for OMS) | |
| Static analysis | clang-tidy, cppcheck; sanitizers ASan/UBSan/TSan as CMake presets | |

### gateway (Go)
| Need | Library |
|---|---|
| HTTP | stdlib `net/http` (1.22+ routing patterns) |
| OpenAPI | `oapi-codegen` (server stubs from `api/openapi.yaml`) |
| WebSocket | `github.com/coder/websocket` |
| Postgres | `jackc/pgx/v5` + `sqlc` |
| Migrations | `golang-migrate` (CLI + library), SQL files in `db/migrations` |
| NATS | `nats-io/nats.go` (+ `jetstream` pkg) |
| gRPC | `google.golang.org/grpc` |
| Auth | `alexedwards/argon2id`, `pquerna/otp`, `golang-jwt/jwt/v5` (EdDSA) |
| ClickHouse (recorder) | `ClickHouse/clickhouse-go/v2` (native protocol, batch) |
| Protobuf | `google.golang.org/protobuf` (Buf-generated) |
| Telemetry | OpenTelemetry Go SDK |
| Lint | golangci-lint |

### quant (Python)
| Need | Library |
|---|---|
| API | FastAPI + Uvicorn, Pydantic v2 |
| Dataframes | **Polars** (primary), NumPy, Numba for hot loops, SciPy (optimisation, stats) |
| Local analytics | DuckDB (Parquet lake queries) |
| ClickHouse | `clickhouse-connect` |
| Postgres | asyncpg (bulk jobs use COPY into a temp table + set-based SQL); SQLAlchemy 2 (async) for read models when a task needs them; no ORM writes to order tables |
| NATS | `nats-py` |
| Engine bindings | `vyuha_core` (nanobind wheel built from `engine/bindings/python`) |
| Calendar | `exchange_calendars` (XBOM) plus a desk override table for NSE special sessions |
| HTTP (downloads) | `httpx` with rate limiter (`aiolimiter`) |
| Tests | pytest, hypothesis, pytest-benchmark |
| Types / lint | mypy (strict), ruff |

### intel (Python)
| Need | Library |
|---|---|
| LLM | `anthropic` SDK (tool use, prompt caching). Provider interface lets an OpenAI-compatible local endpoint (vLLM/Ollama) be used instead |
| MCP server | `mcp` Python SDK (FastMCP), Streamable HTTP transport |
| Embeddings | `sentence-transformers` with **BAAI/bge-m3** (multilingual incl. Hindi, 1024-d), served in-process on CPU |
| Vector store | pgvector (HNSW index) in Postgres |
| Graph | `neo4j` Python driver; Cypher only through parameterised query templates |
| Feeds | `feedparser`, `httpx`, `trafilatura` (article text extraction) |
| PDF (manual uploads) | `pypdf` |
| Evals | pytest + YAML golden sets; scores to ClickHouse |

### web (TypeScript)
| Need | Library |
|---|---|
| Framework | React 19 + Vite (latest) |
| Docking / multi-panel | **Dockview** |
| Data grids | **AG Grid Community** (MIT): transaction updates, row virtualisation |
| Price charts | **TradingView Lightweight Charts v5** (Apache-2.0, keep the attribution requirement) |
| Analytics charts (payoff, scenario heatmap, VaR, factors) | Apache ECharts (canvas renderer) |
| Graph view (CTX) | Cytoscape.js |
| Command palette | cmdk |
| State | Zustand (client), TanStack Query v5 (server) |
| Protobuf | `@bufbuild/protobuf` (protobuf-es) |
| Styling | Tailwind CSS v4 + CSS variables design tokens; Radix primitives for dialogs/menus |
| Hotkeys | `tinykeys` |
| Tests | Vitest + Testing Library; Playwright (E2E + perf traces) |
| Lint | `tsc --noEmit` strict (`noUncheckedIndexedAccess`, `exactOptionalPropertyTypes`). ESLint/Prettier not installed at M0.9 (owner choice 2026-10-28); adding them later needs owner approval |

## 4. Infrastructure
| Component | Choice | Floor | Why |
|---|---|---|---|
| OLTP | PostgreSQL + pgvector | 17 | Orders, users, audit, strategies, embeddings in one ACID store |
| Time-series | ClickHouse | 24.8 LTS | Columnar, very fast for ticks and analytics; Apache-2.0 |
| Bus | NATS Server with JetStream | 2.10 | Streams + KV + work-queues in one small binary |
| Graph | Neo4j Community | 5.26 LTS | Mature Cypher + APOC core; best tooling for path queries |
| Data lake | Parquet on local NVMe (`/data/lake`) | | Single host. MinIO community images were discontinued in 2025, so no object store is needed in the MVP; any S3-compatible store can be added later |
| Reverse proxy | Caddy 2 | | Automatic TLS |
| Private access | Tailscale | | Don't expose the terminal to the public internet |
| Metrics / logs | Prometheus 3, Grafana, Loki, OTel Collector | | |
| Containers | Docker Engine + Compose v2 | | |
| CI | GitHub Actions (ubuntu-24.04 runners) | | |

## 5. Considered and rejected (for the record)
| Option | Why not (MVP) |
|---|---|
| Rust for engine | Strong option. C++ chosen to match the owner's learning goal and the Bloomberg-like stack. Revisit for new isolated components |
| NautilusTrader as the core | Excellent open-source engine (Rust/Python). Rejected to keep one C++ core with India-specific OMS/risk semantics. Study its design for backtest/live parity |
| Kafka / Redpanda | Heavier ops than needed at ≤ 10 users |
| BlazingMQ | Bloomberg's C++ broker. Fewer client languages (no first-class Go/TS) and smaller community than NATS |
| TimescaleDB for ticks | Good, but ClickHouse compresses and scans ticks far faster for analytics |
| Redis / Valkey | NATS KV covers last-value cache. One fewer service |
| Apache AGE (graph in Postgres) | Less mature for path-heavy queries and tooling |
| Tauri for desktop | WebKitGTK on Linux is weaker for heavy canvas/WebGL; Electron gives a consistent Chromium |
| VectorBT open-source | Unmaintained relative to PRO; our backtester must share the live core anyway |
| LangChain/LangGraph | A thin tool loop on the Anthropic SDK + MCP is simpler to audit |

## 6. Pinned versions (fill at M0)
Toolchains are installed one component at a time by `scripts/bootstrap.sh <core|cpp|go|py|web>` (Ubuntu 24.04 via apt, macOS via Homebrew). Go, Node, uv and buf are official release archives checked against the sha256 pinned in the script, unpacked into `$VYUHA_TOOLS_DIR` (default `~/.local/share/vyuha-tools`, no sudo), and the Makefile puts `$VYUHA_TOOLS_DIR/bin` first on `PATH`. **Change a version here and in `scripts/bootstrap.sh` together.** Rows marked "at Mx.y" are pinned by the task that first pulls or adds them (lockfiles / image digests), not before.

**Core list** (`bootstrap.sh core`, the only thing M0 installs): git, jq, make, curl, ca-certificates, xz-utils, Docker Engine + Compose v2 (Ubuntu: `docker.io`, `docker-compose-v2` from the Ubuntu archive, skipped if Docker is already present; macOS: Docker Desktop must already be installed).

| Component | Version | Pinned on |
|---|---|---|
| GCC / Clang | Ubuntu: GCC 14 (`g++-14`, CI reference). macOS: Apple Clang from Xcode Command Line Tools (≥ 16 for `std::expected`). clang-format / clang-tidy / libFuzzer: **LLVM 18** on both (`clang-*-18` apt, `llvm@18` brew) so formatting is identical everywhere | 2026-10-28 |
| CMake | ≥ 3.28 (Ubuntu 24.04 apt 3.28.3; macOS Homebrew current), Ninja from the same source | 2026-10-28 |
| vcpkg baseline commit | `19c330612911442c9939e4a89f85c97603089389` (vcpkg tool 2026-09-26; `engine/vcpkg.json` `builtin-baseline`, bootstrap fetches only that commit, depth 1). Ports: quill 13.1.0 (M1.3; bundles fmt), gtest 1.18.0, benchmark 1.9.5, nanobind 3.1.0 (overlay without `python3`, see `engine/vcpkg-overlays/README.md`), robin-map 1.4.1 | 2026-10-28 |
| Go | 1.27.3 | 2026-10-28 |
| Python | 3.13.14 (via uv 0.13.6) | 2026-10-28 |
| quant (M1.1) | `quant/uv.lock` (`exclude-newer` 7 days): polars 2.0.0, httpx 0.28.1, pydantic 2.14.1, asyncpg 0.32.0, exchange-calendars 4.13.2 (pulls pandas transitively; not imported by library code); dev: pytest 9.1.1, mypy 2.4.0, ruff 0.17.1 | 2026-10-28 |
| Node / pnpm | Node 22.23.4 (22 LTS) / pnpm 10.34.9 via corepack | 2026-10-28 |
| sqlc | 1.31.1 (`bootstrap.sh go`, pinned + sha256) | 2026-10-28 |
| golangci-lint | 2.15.0 (`bootstrap.sh go`, pinned + sha256; config `gateway/.golangci.yml`: standard + gosec, errorlint, bodyclose, noctx, misspell, unconvert) | 2026-10-28 |
| Gateway Go modules | `gateway/go.mod` + `go.sum`: pgx 5.11.0, golang-migrate 4.20.1 (pgx/v5 driver), OTel 1.47.0 (+ otelhttp 0.72.0), protobuf 1.36.12, grpc 1.83.2 (via OTel exporter); oapi-codegen 2.8.1 as a `tool` directive. Buf Go plugins pinned to match in `proto/buf.gen.go.yaml` | 2026-10-28 |
| Gateway auth modules | alexedwards/argon2id 1.0.0 (m=64 MiB, t=2, p=2), pquerna/otp 1.5.0 (SHA1, 6 digits, 30 s, ±1 step), golang-jwt/jwt/v5 5.3.1 (EdDSA only); oapi-codegen/runtime 1.8.0 | 2026-10-28 |
| PostgreSQL image digest | `pgvector/pgvector:pg17@sha256:ac08538c6f8b9904c33c8224c5e5706dbe760aca29db1d096972b4052c22a75d` (PostgreSQL 17.11, pgvector 0.8.7) | 2026-10-28 |
| ClickHouse image digest | at M1.7 | |
| NATS image digest | `nats:2.10-alpine@sha256:b83efabe3e7def1e0a4a31ec6e078999bb17c80363f881df35edc70fcb6bb927` (nats-server 2.10.29) | 2026-10-28 |
| Neo4j image digest | at M6.1 | |
| buf | 1.75.0 (`bootstrap.sh proto`; same version in `.github/workflows/proto.yml`) | 2026-10-28 |
| Web (M0.9) | `web/package.json` exact + `pnpm-lock.yaml`; pnpm `minimumReleaseAge` 7 days. react/react-dom 19.3.1, vite 8.3.5, typescript 7.0.2, dockview 8.6.1 (core DOM API, no `dockview-react`), cmdk 1.1.1, zustand 5.0.15, @tanstack/react-query 5.104.5, tailwindcss + @tailwindcss/vite 4.3.3, tinykeys 4.0.1, vitest 5.0.5, @playwright/test 1.64.1 (Chromium 156 only), @types/react(-dom) 19.3.0 | 2026-10-28 |
| AG Grid / Lightweight Charts / @bufbuild/protobuf | at M1.9 (`web/pnpm-lock.yaml`) | |
| anthropic / mcp / sentence-transformers | at M6.4 / M7.1 / M6.2 (`intel/uv.lock`) | |
| LLM model IDs (`LLM_MODEL_FAST`, `LLM_MODEL_SMART`) | at M6.4 / M7.2 | |
