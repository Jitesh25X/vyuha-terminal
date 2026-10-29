"""Internal FastAPI app called by the gateway (never exposed to browsers; binds loopback by default).

The gateway owns authentication, RBAC and market-data entitlement (ADR-020): it sends only prices the
requesting user may see, and quant computes analytics on them. Nothing here reads market data itself.
"""

from __future__ import annotations

from fastapi import FastAPI

from .. import __version__
from ..config import Settings
from ..optionchain import ChainPolicy, ChainRequest, ChainResult, compute_chain


def create_app(settings: Settings) -> FastAPI:
    app = FastAPI(
        title="vyuha-quant internal", version=__version__, docs_url=None, redoc_url=None, openapi_url=None
    )

    policy = ChainPolicy(
        rate=settings.risk_free_rate,
        rate_as_of=settings.risk_free_rate_as_of,
        mid_max_spread=settings.iv_mid_max_spread,
        ltp_max_age_s=settings.iv_ltp_max_age_s,
    )

    @app.get("/health")
    def health() -> dict[str, str]:
        return {"status": "ok", "version": __version__}

    @app.post("/internal/optionchain")
    def optionchain(req: ChainRequest) -> ChainResult:
        # Sync handler: FastAPI runs it in its threadpool, so the IV loop never blocks the event loop.
        return compute_chain(req, policy)

    return app
