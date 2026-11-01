# syntax=docker/dockerfile:1.7
# Caddy (TLS + reverse proxy) with the built web terminal. Build context: repo root.
# web/src/gen is generated (gitignored): `make proto` first (make staging-build does).

FROM node:22.23.4-bookworm-slim@sha256:4b150b48b4116e2a22f2c39cc27025fbf4f153e2078fd3b14d159fdb5c7279da AS build
WORKDIR /src/web
RUN corepack enable
COPY web/package.json web/pnpm-lock.yaml web/.npmrc web/pnpm-workspace.yaml ./
RUN --mount=type=cache,target=/root/.local/share/pnpm/store pnpm install --frozen-lockfile
COPY web/ ./
RUN test -f src/gen/vyuha/ws/v1/ws_pb.ts || { echo "web/src/gen missing: run make proto before building"; exit 1; }
RUN pnpm build

FROM caddy:2.10.2-alpine@sha256:4c6e91c6ed0e2fa03efd5b44747b625fec79bc9cd06ac5235a779726618e530d
COPY --from=build /src/web/dist /srv/web
COPY deploy/staging/Caddyfile /etc/caddy/Caddyfile
