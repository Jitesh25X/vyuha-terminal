# syntax=docker/dockerfile:1.7
# vyuha-quant: `serve` (internal API for the gateway) and the scheduled jobs (instruments sync,
# ban check). Build context: repo root. Locked deps only (uv.lock, no dev group).

FROM ghcr.io/astral-sh/uv:0.13.6@sha256:af22a52f5eafef3d78e551599d405c02df61919d18028f0cea8eda93bc9986d0 AS uv

FROM python:3.13-slim-bookworm@sha256:281d39ff02e3b389401586134ff9e6572954687822a58055ec07a60ded4e0b7f
COPY --from=uv /uv /usr/local/bin/uv
ENV UV_COMPILE_BYTECODE=1 UV_LINK_MODE=copy UV_PYTHON_DOWNLOADS=never UV_PROJECT_ENVIRONMENT=/opt/venv \
    PATH=/opt/venv/bin:$PATH PYTHONUNBUFFERED=1
WORKDIR /app
COPY quant/pyproject.toml quant/uv.lock ./
RUN --mount=type=cache,target=/root/.cache/uv uv sync --frozen --no-dev --no-install-project
COPY quant/src ./src
RUN --mount=type=cache,target=/root/.cache/uv uv sync --frozen --no-dev \
    && useradd --system --uid 10001 --no-create-home vyuha
USER vyuha
ENTRYPOINT ["vyuha-quant"]
CMD ["serve"]
