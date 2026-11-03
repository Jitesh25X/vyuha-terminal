# Run Vyuha on your Mac

How to run what exists today (M0, M1 and the M4.1 downloader), including the daily Upstox login. Commands
run from the repo root. For the staging VM, see `STAGING.md`.

**What works locally:** sign-in, the terminal shell, instrument search, live market data panels (`Q`, `WL`,
`CH`, `DOM`, `TS`, `OC`, `OI`) once a broker session is up, and historical downloads.
**What doesn't exist yet:** orders of any kind (M2 is next), and `MOV`/`HM` show "no constituents" until M6.1.

This guide needs the `m4.1-upstox-historical` branch: the status-bar login button comes from
`m1.12-staging-wip`, and neither is on `main` yet.

## 1. First-time setup (once)
Needs Docker Desktop running.
```sh
make bootstrap C=core
make bootstrap C=go
make bootstrap C=py
make bootstrap C=web
make bootstrap C=cpp        # the engine; long first build
make dev-secrets            # JWT key + VYUHA_MASTER_KEY in .env; back the master key up
make up PROFILES="core md"  # Postgres, NATS, ClickHouse
make migrate
make migrate-ch
cd quant && uv run vyuha-quant instruments sync && uv run vyuha-quant holidays seed && cd ..
cd web && pnpm install && cd ..
```

Then edit `.env`. Set the login address (it currently says `https://localhost:8443`, which nothing listens on
locally; the Vite dev server proxies `/api` to the gateway, so the broker can return there), and add the rate's
date if it's missing, or every `vyuha-quant` command stops with `RISK_FREE_RATE_AS_OF … is required`:
```sh
PUBLIC_BASE_URL=http://127.0.0.1:5173
RISK_FREE_RATE_AS_OF=2026-10-07
```

Create your admin user (it prompts for a password; TOTP enrolment happens at first sign-in):
```sh
set -a; . ./.env; set +a
cd gateway && go run ./cmd/vyuhactl user create -username YOURNAME -display-name "Your Name" -role admin; cd ..
```

## 2. Add your Upstox account (once)
1. In the [Upstox developer console](https://account.upstox.com/developer/apps), register this redirect URL
   on your app, character for character:
   `http://127.0.0.1:5173/api/v1/brokers/callback/upstox`
2. Find your Mac's current address. Vyuha binds every broker call to the account's address and refuses to
   start if it isn't on this machine (invariant 4):
   ```sh
   ipconfig getifaddr en0
   ```
3. Create and map the account. It prompts for the API key and secret, and stores them encrypted:
   ```sh
   set -a; . ./.env; set +a
   cd gateway
   go run ./cmd/vyuhactl account create -broker upstox -client-code YOUR_UCC -holder "Your Name" \
     -static-ip <address from step 2> -data-entitled
   go run ./cmd/vyuhactl account map -account upstox:YOUR_UCC -user YOURNAME -trade -default
   cd ..
   ```
   `-client-code` must match your Upstox user id exactly, or login fails with `BROKER_ACCOUNT_MISMATCH`.
   `-trade` is what allows the daily login; no order path exists yet.

**About the address:** your Mac's LAN address is not a registered static IP. Binding to it proves the address
is on this Mac; behind your router it doesn't control which public IP Upstox sees. That's fine for downloads:
[Upstox's circular](https://upstox.com/developer/api-documentation/announcements/algo-trading-circular/) (checked
2026-11-02) says historical candles aren't restricted by static IP. The circular doesn't mention login or live
market data, so treating those as unrestricted is an inference until tried. **Never use this setup for live
orders.** No order path exists yet, and nothing in the code enforces this until M3.6. A different Wi-Fi network
changes the address (see §7).

## 3. Start everything (each session)
Three terminals. Docker must be up (`make up PROFILES="core md"`).

| Terminal | Command | Serves |
|---|---|---|
| 1 Engine | `set -a; . ./.env; set +a; engine/build/release/vyuha_engine` | gRPC on 127.0.0.1:7443 |
| 2 Gateway | `make dev-gateway` | API on 127.0.0.1:8080 |
| 3 Web | `cd web && pnpm dev` | http://127.0.0.1:5173 |

No engine binary yet? `cd engine && cmake --preset release && cmake --build --preset release --target vyuha_engine`.

Optional: `set -a; . ./.env; set +a; cd quant && env -u VYUHA_MASTER_KEY uv run vyuha-quant serve` for `OC`/`OI`, and
`cd gateway && go run ./cmd/recorder` to record ticks to ClickHouse.

Open http://127.0.0.1:5173 (use this exact address, not `localhost`) and sign in.

## 4. Daily Upstox login
Upstox tokens end at **03:30 IST** every day, whenever you got them, so log in once each day you use it.

1. With all three services running, click **LOGIN UPSTOX \<id\>** in the status bar.
2. Log in on Upstox's own page with its 2FA. Vyuha never sees your password or automates this.
3. You return to the terminal, and the login indicator clears.

An engine restart drops the session, so log in again after one. A gateway restart during the login itself
invalidates that attempt; just retry.

| Message | Meaning | Fix |
|---|---|---|
| `STATIC_IP_NOT_ON_HOST` | The account's address isn't on this Mac | See §7 |
| `BROKER_ACCOUNT_MISMATCH` | You logged in as a different Upstox user | Use the account's user id |
| Upstox rejects the redirect | The URL differs from the registered one | Compare with §2 and `PUBLIC_BASE_URL` |
| Engine error after Upstox login | Engine isn't running | Start terminal 1. The token was stored, so downloads still work; log in again for live data |

## 5. Day-to-day checks
```sh
make down                   # stop the containers; data stays
make test-quant             # one service's tests; make test for all
make e2e                    # browser tests against a throwaway database
```

## 6. Download historical data (M4.1)
The first job is Nifty 50 constituents + NIFTY and BANKNIFTY, 1-minute from Jan 2022 and daily from 2015.

1. **Get the constituents list by hand** (like the ban list, ADR-019; Vyuha doesn't script NSE sites).
   Download `ind_nifty50list.csv` from
   [niftyindices.com](https://www.niftyindices.com/IndexConstituent/ind_nifty50list.csv) and save it as
   `data/reference/ind_nifty50list.csv`. It's today's membership, so stocks that left the index earlier
   aren't included.
2. **See what would run.** This needs no token and makes no broker request:
   ```sh
   set -a; . ./.env; set +a
   env -u VYUHA_MASTER_KEY uv run --project quant vyuha-quant download plan --spec quant/jobs/first_1minute.json
   env -u VYUHA_MASTER_KEY uv run --project quant vyuha-quant download plan --spec quant/jobs/first_day.json
   ```
3. **Do the daily login (§4), then run the jobs one at a time, from the repo root** (`DATA_LAKE_DIR` is relative
   to where you start them):
   ```sh
   cd gateway && go build -o bin/vyuhactl ./cmd/vyuhactl && cd ..
   gateway/bin/vyuhactl download run -account upstox:YOUR_UCC -user YOURNAME -spec quant/jobs/first_1minute.json
   gateway/bin/vyuhactl download run -account upstox:YOUR_UCC -user YOURNAME -spec quant/jobs/first_day.json
   ```

**Expect** about 3,070 requests and just under an hour for 1-minute (paced at one request per 1.125 s, which
keeps under Upstox's 2,000 per 30 minutes), and about 2 minutes for daily. Roughly 24 million 1-minute rows;
allow a few GB in `data/lake/` until the real size is known.

**Stopped part-way?** Run the same command again: it continues from the last finished chunk, including after
a crash or the 03:30 IST token expiry (`BROKER_TOKEN_EXPIRED`; log in first). A re-run with nothing left makes
0 requests. Each morning a re-run fetches only the days since the last one.

The final JSON line reports `requests`, `http_429` (should be 0), `new_bars`, `days_without_bars` (logged,
never filled), `dropped` (rows that failed validation) and `chunks_incomplete` (had rejected rows: unreadable, invalid, or a whole day outside any known session).

**Exit code 3 means the download is incomplete**, and the report says `"ok": false`. `chunks_incomplete` chunks
weren't recorded, so run the same command again to retry them. Exit code 1 is any other failure. A trading day with no bars counts as a gap too (`days_without_bars`), so a chunk holding one is retried; days
before an instrument's first bar count as not yet listed (`days_before_first_bar`). Muhurat sessions from 2022
and Saturday sessions from 2024 are in `holidays seed`, with the NSE circular each came from. A stock that really
has no bars on a day (suspended, or no trades) exits 3 on every run until an admin records it, for that instrument and
that day: `uv run vyuha-quant download known-gap --symbol NSE:X --day 2024-02-14 --reason "suspended" --user owner`.

## 7. Troubleshooting
| Problem | Fix |
|---|---|
| `STATIC_IP_NOT_ON_HOST` after changing network | `ipconfig getifaddr en0`, then update the account (dev only; this edit isn't audited): `docker compose -f deploy/docker-compose.dev.yml exec postgres psql -U vyuha -d vyuha -c "UPDATE broker_account SET static_ip = '<new>' WHERE client_code = 'YOUR_UCC'"` |
| `uv` says `Permission denied` under `~/.cache/uv/wheels-v6/pypi/scipy` | Part of the cache is owned by root: `sudo chown -R "$USER" ~/.cache/uv` |
| Sign-in fails or the page can't reach the API | Is the gateway up? Are you on `127.0.0.1:5173`? |
| Panels say no entitlement (`MD_NOT_ENTITLED`) | Create the account with `-data-entitled` and map it to your user |
| `DOWNLOAD_ALREADY_RUNNING` | Another Upstox download is running; wait, or stop it. A killed job releases this at once |
| `no live access token` | Do the daily login (§4) |
