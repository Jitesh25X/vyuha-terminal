# Staging runbook (M1.12)

> **WIP: not finished, not verified.** M1.12 is blocked until the staging VM exists (ADR-023). Merged to
> `main` so M2 can build on the same tree, not because staging works. Don't treat anything below as done
> or production-ready, and don't count it towards M1.12 or the M1 exit.
>
> WIP files: this runbook, everything in `deploy/staging/` (Dockerfiles, compose file, Caddyfile,
> `init-env.sh`, `roles.sql`, `systemd/`) and the `make staging-build` target. The images have never been
> built, and nothing has run on a VM. Remove this notice only when M1.12 is ticked in `docs/TASKS.md`.

Goal: one VM with two static IPs, a real Kite session and a real Upstox session streaming Nifty 50 and
NIFTY options (near expiries), and a measured tick → screen p95 under 400 ms.

Deployment model: Docker Compose (ARCHITECTURE §9, ADR-022). Files: `deploy/staging/`. Live trading
stays off (`LIVE_TRADING_ENABLED=false` is fixed in the compose file; there is no order path before M2).

## 0. Before you start
- **VM:** Ubuntu 24.04 LTS, 8 vCPU / 32 GB / 500 GB NVMe, Mumbai region (ARCHITECTURE §9).
- **Static IPs:** two secondary private IPs on the VM's NIC, each mapped by the cloud provider to its
  own public static (elastic) IP. One per broker account. NSE rule: unrelated clients never share a
  public IP.
- **Broker apps:** a Kite Connect app and an Upstox app, one per account holder. Register:
  - the account's public static IP with the broker;
  - the redirect URL — Kite `https://<PUBLIC_HOST>/api/v1/brokers/callback/zerodha`, Upstox
    `https://<PUBLIC_HOST>/api/v1/brokers/callback/upstox` (must match exactly).
- **DNS:** `PUBLIC_HOST` resolves to the VM. Ports 80/443 open for Caddy's certificate; restrict access
  to the desk (Tailscale or a cloud firewall allowlist).
- **Time:** `chrony` running (the engine refuses large clock skew; latency figures rely on it).

## 1. Prepare the VM
```sh
sudo apt-get update && sudo apt-get install -y git make chrony
# Docker Engine + compose plugin: https://docs.docker.com/engine/install/ubuntu/
sudo cp deploy/netplan-static-ips.example.yaml /etc/netplan/60-vyuha.yaml   # edit interface + addresses
sudo netplan apply && ip -4 addr        # both private IPs must be listed
```
Put the repository at `/opt/vyuha` (the systemd units assume it).

## 2. Bootstrap check (M0.2 deferral, part of M1.12's done check)
```sh
cd /opt/vyuha && make bootstrap C=core   # must pass and install nothing beyond the core list
make bootstrap C=proto                   # buf: generates protobuf code for the images
```

## 3. Build the images
```sh
make staging-build                        # vyuha/{engine,gateway,quant,web}:staging
```
The engine image compiles gRPC/abseil/protobuf through vcpkg: expect about an hour the first time.

## 4. Secrets and databases
```sh
cd deploy/staging
./init-env.sh init <PUBLIC_HOST> <ACME_EMAIL>   # .env (0600) + secrets/jwt_ed25519.pem; never overwritten
$EDITOR .env                                     # RISK_FREE_RATE (decimal) + RISK_FREE_RATE_AS_OF (YYYY-MM-DD)
docker compose up -d postgres nats clickhouse
docker compose run --rm migrate                  # Postgres schema (owner role)
./init-env.sh roles                              # vyuha_gateway / vyuha_engine / vyuha_quant logins
./init-env.sh clickhouse                         # ClickHouse schema
```
Back up `.env` and `secrets/` off the VM. Losing `VYUHA_MASTER_KEY` makes stored broker secrets unreadable.

## 5. Reference data
```sh
docker compose run --rm --no-deps quant instruments sync    # Kite + Upstox instrument files (F-MD-1)
docker compose run --rm --no-deps quant holidays seed
# F&O ban list (ADR-019, manual): download fo_secban.csv in a browser, then
docker compose run --rm --no-deps -v "$PWD/fo_secban.csv:/in.csv:ro" quant ban import /in.csv
```
Schedules (instrument sync 08:00 IST daily, ban check 08:45 IST weekdays):
```sh
sudo cp systemd/vyuha-*.service systemd/vyuha-*.timer /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now vyuha-instruments-sync.timer vyuha-ban-check.timer
```

## 6. Users and broker accounts
```sh
docker compose run --rm -it vyuhactl user create -username meera -display-name "Meera" -role trader
docker compose run --rm -it vyuhactl account create -broker zerodha -client-code <KITE_ID> \
  -holder "<name>" -static-ip <private IP 1> -public-ip <public IP 1> -data-entitled
docker compose run --rm -it vyuhactl account create -broker upstox -client-code <UPSTOX_ID> \
  -holder "<name>" -static-ip <private IP 2> -public-ip <public IP 2> -data-entitled
docker compose run --rm vyuhactl account map -account zerodha:<KITE_ID> -user meera -trade -default
docker compose run --rm vyuhactl account map -account upstox:<UPSTOX_ID> -user meera -trade
```
`account create` asks for the API key and secret with echo off; they are stored encrypted.

## 7. Start the services
```sh
docker compose up -d engine gateway recorder quant caddy
docker compose ps                                   # all running / healthy
curl -fsS https://<PUBLIC_HOST>/api/v1/health       # {"status":"ok",…}
```
Sign in at `https://<PUBLIC_HOST>` (first login enrolls TOTP).

## 8. Daily broker login (before 09:15 IST)
The status bar shows **LOGIN KITE <id>** / **LOGIN UPSTOX <id>** for each account whose session isn't
connected. Each opens the broker's own login and 2FA; the gateway exchanges the token from the account's
static IP and starts the engine session. Tokens expire 06:00 IST (Kite) / 03:30 IST (Upstox).
An engine restart drops sessions: log in again.

## 9. Stream and measure (the done check)
Record only what M1.12 needs: subscribe to Nifty 50 constituents and NIFTY options of the near expiries
(`WL`, `OC NIFTY`). The recorder stores what the engine streams; nothing else is subscribed.

1. During market hours, open `WL` with the Nifty 50 and `OC NIFTY` for the near expiry.
2. Run `SYS`. It shows tick → screen p50/p95/p99 (engine receive → paint in this browser, clock offset
   from Ping/Pong) and a histogram. Let it collect at least 15 minutes; **p95 must be < 400 ms**.
3. Record in `docs/TASKS.md` (M1.12): the p50/p95/p99, sample count, the clock RTT shown, the browser's
   location relative to the VM, and the `make bootstrap C=core` result.

## Troubleshooting
| Symptom | Check |
|---|---|
| Login redirect says `STATIC_IP_NOT_ON_HOST` | the account's `-static-ip` is not on the VM (`ip -4 addr`) |
| `BROKER_ACCOUNT_MISMATCH` | signed in at the broker as a different client than the account's client code |
| Status bar `engine DOWN` | `docker compose logs engine`; the gateway reaches it on 127.0.0.1:7443 |
| Panels say "No market-data entitlement" | account not `-data-entitled`, or user not mapped to it |
| SYS shows no samples | no live quotes painted yet, or no clock estimate (stream not OPEN) |
