# syntax=docker/dockerfile:1.7
# gateway + recorder + vyuhactl (static Go binaries) and the DB migrations. Build context: repo root.
# gateway/gen is generated (gitignored): `make proto` first (make staging-build does).

FROM golang:1.27.3-bookworm@sha256:d9b6a1beb9474adc2a1ce842573639b44b2a0dd49b6669f51d3821baf9ec37c9 AS build
ENV GOTOOLCHAIN=local CGO_ENABLED=0
WORKDIR /src/gateway
COPY gateway/go.mod gateway/go.sum ./
RUN --mount=type=cache,target=/go/pkg/mod go mod download
COPY gateway/ ./
RUN test -f gen/vyuha/ws/v1/ws.pb.go || { echo "gateway/gen missing: run make proto before building"; exit 1; }
ARG VERSION=staging
RUN --mount=type=cache,target=/go/pkg/mod --mount=type=cache,target=/root/.cache/go-build \
    for c in gateway recorder vyuhactl; do \
      go build -trimpath -ldflags "-s -w -X main.version=${VERSION}" -o /out/$c ./cmd/$c || exit 1; \
    done

FROM gcr.io/distroless/static-debian12:nonroot@sha256:afa5c872c891853ca7fcf1f12c3edb23f7eeef36189728842dd51042ff57f7ab
COPY --from=build /out/ /usr/local/bin/
COPY db/migrations /opt/vyuha/db/migrations
COPY db/clickhouse /opt/vyuha/db/clickhouse
ENTRYPOINT ["/usr/local/bin/gateway"]
