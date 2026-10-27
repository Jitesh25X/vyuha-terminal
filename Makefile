# Vyuha top-level tasks. Targets for services that don't exist yet print a notice and succeed,
# so `make verify` works from M0 onward. Claude Code fills them in as milestones land.
SHELL := /bin/bash
.SHELLFLAGS := -eu -o pipefail -c
PROFILES ?= core
# Pinned toolchains from scripts/bootstrap.sh take precedence over whatever else is on PATH.
VYUHA_TOOLS_DIR ?= $(HOME)/.local/share/vyuha-tools
export VYUHA_TOOLS_DIR
export PATH := $(VYUHA_TOOLS_DIR)/bin:$(PATH)
export VCPKG_ROOT ?= $(VYUHA_TOOLS_DIR)/vcpkg
COMPOSE := docker compose -f deploy/docker-compose.dev.yml --env-file .env $(foreach p,$(PROFILES),--profile $(p))

# $(1) = marker file that exists once the service is scaffolded, $(2) = command
define maybe
	@if [ -f "$(1)" ]; then $(2); else echo "· skip $(1) (not scaffolded yet)"; fi
endef

CLEAN_DIRS := engine/build gateway/bin web/dist web/test-results web/playwright-report

.PHONY: clean-space bootstrap up down migrate migrate-ch migrate-neo4j proto test test-engine test-gateway test-quant test-intel test-web bench e2e lint verify invariants

# Installs ONE component, just-in-time: make bootstrap C=cpp   (core|cpp|go|py|web)
# Add CHECK=1 to only report what is present / would be installed.
bootstrap:
	@if [ -z "$(C)" ]; then echo "usage: make bootstrap C=<core|cpp|go|py|web> [CHECK=1] (install only what the current task needs)"; exit 1; fi
	@./scripts/bootstrap.sh "$(C)" $(if $(CHECK),--check)

up:
	$(COMPOSE) up -d --wait

down:
	$(COMPOSE) down

# Postgres from M0; ClickHouse from M1.7 (profile md); Neo4j from M6.1 (profile graph)
migrate:
	migrate -path db/migrations -database "$$POSTGRES_URL" up

migrate-ch:
	for f in db/clickhouse/*.sql; do docker compose -f deploy/docker-compose.dev.yml exec -T clickhouse clickhouse-client --multiquery < $$f; done

migrate-neo4j:
	for f in db/neo4j/*.cypher; do docker compose -f deploy/docker-compose.dev.yml exec -T neo4j cypher-shell -u "$$NEO4J_USER" -p "$$NEO4J_PASSWORD" < $$f; done

proto:
	cd proto && buf lint && buf generate

test: test-engine test-gateway test-quant test-intel test-web

test-engine:
	$(call maybe,engine/CMakeLists.txt,cd engine && cmake --preset debug && cmake --build --preset debug && ctest --preset debug && cmake --preset asan && cmake --build --preset asan && ctest --preset asan)

test-gateway:
	$(call maybe,gateway/go.mod,cd gateway && go test ./...)

test-quant:
	$(call maybe,quant/pyproject.toml,cd quant && uv run pytest -q)

test-intel:
	$(call maybe,intel/pyproject.toml,cd intel && uv run pytest -q)

test-web:
	$(call maybe,web/package.json,cd web && pnpm test --run)

bench:
	$(call maybe,engine/CMakeLists.txt,cd engine && cmake --preset release && cmake --build --preset release && ./build/release/bench/vyuha_bench --benchmark_min_time=2s)

e2e:
	$(call maybe,web/package.json,cd web && pnpm exec playwright test)

lint:
	@if command -v buf >/dev/null; then cd proto && buf lint; else echo "· skip buf lint (buf arrives in M0.4)"; fi
	$(call maybe,engine/CMakeLists.txt,cd engine && clang-format --dry-run --Werror $$(git ls-files '*.cc' '*.h') && run-clang-tidy -p build/debug -quiet)
	$(call maybe,gateway/go.mod,cd gateway && golangci-lint run)
	$(call maybe,quant/pyproject.toml,cd quant && uv run ruff check && uv run mypy --strict src sdk)
	$(call maybe,intel/pyproject.toml,cd intel && uv run ruff check && uv run mypy --strict src)
	$(call maybe,web/package.json,cd web && pnpm lint && pnpm tsc --noEmit)

clean-space:
	@echo "Before:"; du -sh . 2>/dev/null || true
	rm -rf $(CLEAN_DIRS)
	find . -name __pycache__ -type d -prune -exec rm -rf {} + 2>/dev/null || true
	if command -v uv >/dev/null; then uv cache prune; fi
	if command -v pnpm >/dev/null; then pnpm store prune; fi
	if command -v go >/dev/null; then go clean -cache -testcache; fi
	if [ -n "$${VCPKG_ROOT:-}" ]; then rm -rf "$$VCPKG_ROOT/buildtrees" "$$VCPKG_ROOT/downloads" "$$VCPKG_ROOT/packages"; fi
	if command -v docker >/dev/null; then docker image prune -f && docker builder prune -f; fi
	@echo "After:"; du -sh . 2>/dev/null || true
	@for d in engine gateway quant intel web data; do [ -d $$d ] && du -sh $$d; done; true

invariants:
	@./scripts/check-invariants.sh

verify: lint test e2e invariants
	@echo "✔ verify complete"
