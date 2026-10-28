#!/usr/bin/env bash
# Installs ONE component's toolchain, just-in-time (PRD §10.1). Supports Ubuntu 24.04 (apt) and macOS (Homebrew).
# Usage: scripts/bootstrap.sh <core|proto|cpp|go|py|web> [--check]
#   --check   report what is present / would be installed; install nothing
# Pinned versions live here and in docs/TECH_STACK.md §6 — change both together.
# Go, Node, uv and buf go into $VYUHA_TOOLS_DIR (no sudo); the Makefile puts $VYUHA_TOOLS_DIR/bin first on PATH.
# Must stay compatible with bash 3.2 (macOS /bin/bash): no associative arrays, no empty-array expansion.
set -euo pipefail

GO_VERSION=1.27.3
NODE_VERSION=22.23.4
PNPM_VERSION=10.34.9
UV_VERSION=0.13.6
BUF_VERSION=1.75.0
SQLC_VERSION=1.31.1
GOLANGCI_LINT_VERSION=2.15.0
PYTHON_VERSION=3.13.14
LLVM_MAJOR=18 # clang-format / clang-tidy / libFuzzer on both platforms, so formatting is identical everywhere
GCC_MAJOR=14  # Ubuntu compiler (CI reference); macOS compiles with Apple Clang from the Command Line Tools
CMAKE_FLOOR=3.28

go_sha256() {
  case "$1" in
    darwin-amd64) echo b6f8e931e4ab497fcde6da8ae4499a58f9d258e55c8a80824a516eae5950a750 ;;
    darwin-arm64) echo dd3908cf20959d798c1e06ebe5b72213f0505ce01373b40ce3110975a6cd99c2 ;;
    linux-amd64) echo 65efbc80fdc1fa2cd2631cad5b481777a68e8725f34e89f50fc6ac714e379b40 ;;
    linux-arm64) echo a06e99309dbcbdca1a8ccd2282c3ee146e55930886e26b4f1193269587165f49 ;;
  esac
}
node_sha256() {
  case "$1" in
    darwin-arm64) echo 037650d0839f457747396f052073fb48333d31bf5abd3c98b74da20d89ebb1ad ;;
    darwin-x64) echo f42abca7aaac61efc57d49fb42ea84fc53bbeba9a749cb03a291ce6e4e7a69c1 ;;
    linux-arm64) echo 6287aa46b6c375d816e63ae66d024fbbfd70404825aabdfdf898a970871d2f4b ;;
    linux-x64) echo 1805bcd35e6815559bd2eb97db438d3fdb0eb3a5e6a977b7c1badca76337e9ce ;;
  esac
}
uv_sha256() {
  case "$1" in
    aarch64-apple-darwin) echo 60fbd5c8a1ed681845eea28d9d622fcde03c51e91a43465b64a1e17db1ed3472 ;;
    x86_64-apple-darwin) echo 63ba17fdda55ad1a230d4b37fc6017ffa10c23688c83818bd3bfdf914c652a02 ;;
    aarch64-unknown-linux-gnu) echo 229e194255c7958d86d56e8790d969a6f74736592133f0dfd0076f754c642c1d ;;
    x86_64-unknown-linux-gnu) echo f29c3e94ffabd7e6f3ba521dba169a9b7ec12100bf3257087f0f24f6123f3645 ;;
  esac
}

buf_sha256() {
  case "$1" in
    Darwin-arm64) echo f71b009ebd8530b12fd85b060c6c245e15d6e5110ccc8da48d8b6fa71e59330b ;;
    Darwin-x86_64) echo ff6d0979143c661d59178a032c57be883b2ac03dc9bf7d00ffca222b0354bd46 ;;
    Linux-aarch64) echo 00eb190aa82df17f393e05b04889cc60a0b13b3259d336e21a87d5f23ab4fd26 ;;
    Linux-x86_64) echo fad1e38d6909bbb583a4da81d8c02a01046e2cd663bde7e337dd24277393764b ;;
  esac
}

sqlc_sha256() {
  case "$1" in
    darwin_arm64) echo 21602158c99eb1f2bae197a66abfb1941d1e9e50b23125bb193349c6b1acc71e ;;
    darwin_amd64) echo c5af76772e3785d21663a62697056b383f07629979b1bd25b93872e73dbd519b ;;
    linux_arm64) echo b7cae247740d0c51a1e657479e5b2d21e6fef428f596682a01bc55bf4ab8a23d ;;
    linux_amd64) echo 497ae4fcdfa64c5b0c311ffe4c2bd991e43991e82e5367792ed78bc2dca27354 ;;
  esac
}

golangci_sha256() {
  case "$1" in
    darwin-arm64) echo d1e409481a8ca846c4bc97e781eb5cb1bd5c23353a0b3977a1cbda1b1b6447d5 ;;
    darwin-amd64) echo 767b899a28285e88ded2823d5dcc4bc5ef681173eb2d80f029a7b17ee4ece3b8 ;;
    linux-arm64) echo b4d8f17068b35ee6060cc2f69a16fb48740d706fbba81661c318649c114a6998 ;;
    linux-amd64) echo 9b7fe6a6bf6334b98decf8d86b11415ea7889afdabdd1959fdce23af2a9ebcea ;;
  esac
}

TOOLS_DIR="${VYUHA_TOOLS_DIR:-$HOME/.local/share/vyuha-tools}"
BIN_DIR="$TOOLS_DIR/bin"
USER_PATH="$PATH"
export PATH="$BIN_DIR:$PATH"
cd "$(dirname "$0")/.."

log() { echo "· $*"; }
ok() { echo "✔ $*"; }
die() { echo "✖ $*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

[ $# -ge 1 ] || die "usage: $0 <core|proto|cpp|go|py|web> [--check]"
component="$1"
check_only=0
[ "${2:-}" = "--check" ] && check_only=1

arch="$(uname -m)"
case "$(uname -s)" in
  Darwin)
    platform=macos
    have brew || die "Homebrew is required on macOS: https://brew.sh"
    xcode-select -p >/dev/null 2>&1 || die "Xcode Command Line Tools missing: run 'xcode-select --install'"
    ;;
  Linux)
    # shellcheck disable=SC1091
    . /etc/os-release
    [ "${ID:-}" = ubuntu ] || die "unsupported Linux distro '${ID:-?}' (Ubuntu 24.04 only)"
    [ "${VERSION_ID:-}" = 24.04 ] || log "warning: built for Ubuntu 24.04, found ${VERSION_ID:-?}"
    platform=ubuntu
    ;;
  *) die "unsupported OS: $(uname -s)" ;;
esac
case "$arch" in
  x86_64 | amd64) goarch=amd64 nodearch=x64 uvarch=x86_64 bufarch=x86_64 ;;
  arm64 | aarch64) goarch=arm64 nodearch=arm64 uvarch=aarch64 bufarch=arm64 ;;
  *) die "unsupported CPU architecture: $arch" ;;
esac
if [ $platform = macos ]; then goos=darwin uvtarget="$uvarch-apple-darwin" bufos=Darwin; else goos=linux uvtarget="$uvarch-unknown-linux-gnu" bufos=Linux; fi
[ "$bufos-$bufarch" = Linux-arm64 ] && bufarch=aarch64

SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"
apt_updated=0

# Installs the missing OS packages among "$@" (apt or brew package names).
pkg_install() {
  local missing="" p
  for p in "$@"; do
    if [ $platform = ubuntu ]; then
      dpkg -s "$p" >/dev/null 2>&1 || missing="$missing $p"
    else
      brew list --versions "$p" >/dev/null 2>&1 || missing="$missing $p"
    fi
  done
  if [ -z "$missing" ]; then
    ok "packages present:$(printf ' %s' "$@")"
    return
  fi
  if [ $check_only = 1 ]; then
    log "would install:$missing"
    return
  fi
  log "installing:$missing"
  if [ $platform = ubuntu ]; then
    if [ $apt_updated = 0 ]; then
      $SUDO apt-get update -qq
      apt_updated=1
    fi
    # shellcheck disable=SC2086
    $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends $missing
  else
    # shellcheck disable=SC2086
    brew install $missing
  fi
}

sha256_of() { if have sha256sum; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }

# Downloads $1 to $3 and checks it against the pinned sha256 $2.
fetch_verified() {
  curl -fsSL --retry 3 -o "$3" "$1"
  if [ "$(sha256_of "$3")" != "$2" ]; then
    rm -f "$3"
    die "checksum mismatch for $1"
  fi
}

link_bin() {
  mkdir -p "$BIN_DIR"
  ln -sf "$1" "$BIN_DIR/${2:-$(basename "$1")}"
}

# version_ge A B: true if dotted version A >= B.
version_ge() {
  local IFS=.
  local a b i x y
  a=($1)
  b=($2)
  for i in 0 1 2; do
    x=${a[$i]:-0}
    y=${b[$i]:-0}
    [ "$x" -gt "$y" ] && return 0
    [ "$x" -lt "$y" ] && return 1
  done
  return 0
}

path_hint() {
  case ":$USER_PATH:" in
    *":$BIN_DIR:"*) ;;
    *) log "for your shell, add to ~/.zshrc or ~/.bashrc:  export PATH=\"$BIN_DIR:\$PATH\"" ;;
  esac
}

# ── core: git, jq, make, curl, Docker Engine + Compose v2 ─────────────────────
do_core() {
  if [ $platform = ubuntu ]; then
    local pkgs="git jq make curl ca-certificates xz-utils"
    have docker || pkgs="$pkgs docker.io"
    docker compose version >/dev/null 2>&1 || pkgs="$pkgs docker-compose-v2"
    # shellcheck disable=SC2086
    pkg_install $pkgs
    if [ $check_only = 0 ] && [ "$(id -u)" -ne 0 ] && ! id -nG | grep -qw docker; then
      $SUDO usermod -aG docker "$(id -un)"
      log "added $(id -un) to the docker group: log out and back in, then re-run this command"
    fi
  else
    # git, make and curl ship with macOS + Command Line Tools; jq ships with macOS 15+.
    have jq || pkg_install jq
    have docker || die "Docker Desktop is required on macOS: https://docs.docker.com/desktop/setup/install/mac-install/"
  fi
  [ $check_only = 1 ] && return
  local t
  for t in git jq make curl docker; do have "$t" || die "$t not found on PATH"; done
  docker compose version >/dev/null 2>&1 || die "Docker Compose v2 plugin not found"
  docker info >/dev/null 2>&1 || die "Docker daemon not reachable (start Docker Desktop, or: sudo systemctl start docker)"
  ok "core ready: git $(git --version | awk '{print $3}'), $(jq --version), docker $(docker version --format '{{.Server.Version}}'), compose $(docker compose version --short)"
}

# ── proto: buf CLI (single binary, pinned + checksummed); codegen uses remote BSR plugins ──
do_proto() {
  if [ -x "$BIN_DIR/buf" ] && [ "$("$BIN_DIR/buf" --version)" = "$BUF_VERSION" ]; then
    ok "buf $BUF_VERSION already installed"
    return
  fi
  if [ $check_only = 1 ]; then
    log "would install buf $BUF_VERSION into $BIN_DIR"
    return
  fi
  local file="buf-$bufos-$bufarch"
  mkdir -p "$BIN_DIR"
  fetch_verified "https://github.com/bufbuild/buf/releases/download/v$BUF_VERSION/$file" "$(buf_sha256 "$bufos-$bufarch")" "$BIN_DIR/buf.tmp"
  chmod +x "$BIN_DIR/buf.tmp"
  mv "$BIN_DIR/buf.tmp" "$BIN_DIR/buf"
  ok "proto ready: buf $("$BIN_DIR/buf" --version)"
  path_hint
}

# ── cpp: compiler, CMake, Ninja, LLVM tools, vcpkg (shallow, at engine/vcpkg.json baseline) ─────
do_cpp() {
  local llvm_bin
  if [ $platform = ubuntu ]; then
    pkg_install "g++-$GCC_MAJOR" "clang-$LLVM_MAJOR" "clang-format-$LLVM_MAJOR" "clang-tidy-$LLVM_MAJOR" \
      cmake ninja-build pkg-config zip unzip tar
    llvm_bin="/usr/lib/llvm-$LLVM_MAJOR/bin"
  else
    pkg_install cmake ninja pkg-config "llvm@$LLVM_MAJOR"
    llvm_bin="$(brew --prefix "llvm@$LLVM_MAJOR")/bin"
  fi
  if [ $check_only = 1 ]; then
    log "would set up vcpkg in ${VCPKG_ROOT:-$TOOLS_DIR/vcpkg}"
    return
  fi
  local t
  for t in clang-format clang-tidy run-clang-tidy; do
    if [ -x "$llvm_bin/$t" ]; then link_bin "$llvm_bin/$t"; fi
  done
  local cmake_version
  cmake_version="$(cmake --version | head -1 | awk '{print $3}')"
  version_ge "$cmake_version" "$CMAKE_FLOOR" || die "CMake $cmake_version < $CMAKE_FLOOR"
  setup_vcpkg
  ok "cpp ready: cmake $cmake_version, $(clang-format --version | head -1)"
  path_hint
}

setup_vcpkg() {
  local root="${VCPKG_ROOT:-$TOOLS_DIR/vcpkg}" baseline=""
  [ -f engine/vcpkg.json ] && baseline="$(jq -r '."builtin-baseline" // empty' engine/vcpkg.json)"
  if [ -z "$baseline" ]; then
    log "skip vcpkg: engine/vcpkg.json has no builtin-baseline yet (set in M0.5)"
    return
  fi
  if [ -x "$root/vcpkg" ] && [ "$(git -C "$root" rev-parse HEAD)" = "$baseline" ]; then
    ok "vcpkg already at $baseline"
    return
  fi
  # Depth-1 fetch of exactly the baseline commit: enough for manifest mode, no full history.
  mkdir -p "$root"
  git -C "$root" init -q
  git -C "$root" remote add origin https://github.com/microsoft/vcpkg.git 2>/dev/null || true
  git -C "$root" fetch -q --depth 1 origin "$baseline"
  git -C "$root" checkout -q FETCH_HEAD
  "$root/bootstrap-vcpkg.sh" -disableMetrics
  link_bin "$root/vcpkg"
}

# ── go: official tarball + sqlc + golangci-lint binaries, pinned + checksummed ─
# Other Go tools (oapi-codegen, migrate) are `tool` directives in gateway/go.mod, not installed here.
do_go() {
  install_go
  install_sqlc
  install_golangci_lint
}

install_golangci_lint() {
  if [ -x "$BIN_DIR/golangci-lint" ] && "$BIN_DIR/golangci-lint" version 2>/dev/null | grep -q "version $GOLANGCI_LINT_VERSION "; then
    ok "golangci-lint $GOLANGCI_LINT_VERSION already installed"
    return
  fi
  if [ $check_only = 1 ]; then
    log "would install golangci-lint $GOLANGCI_LINT_VERSION into $BIN_DIR"
    return
  fi
  local name="golangci-lint-$GOLANGCI_LINT_VERSION-$goos-$goarch" tmp
  tmp="$(mktemp -d)"
  fetch_verified "https://github.com/golangci/golangci-lint/releases/download/v$GOLANGCI_LINT_VERSION/$name.tar.gz" \
    "$(golangci_sha256 "$goos-$goarch")" "$tmp/$name.tar.gz"
  tar -C "$tmp" -xzf "$tmp/$name.tar.gz"
  mkdir -p "$BIN_DIR"
  mv "$tmp/$name/golangci-lint" "$BIN_DIR/golangci-lint"
  rm -rf "$tmp"
  ok "golangci-lint ready: $("$BIN_DIR/golangci-lint" version | head -1)"
}

install_sqlc() {
  if [ -x "$BIN_DIR/sqlc" ] && [ "$("$BIN_DIR/sqlc" version)" = "v$SQLC_VERSION" ]; then
    ok "sqlc $SQLC_VERSION already installed"
    return
  fi
  if [ $check_only = 1 ]; then
    log "would install sqlc $SQLC_VERSION into $BIN_DIR"
    return
  fi
  local file="sqlc_${SQLC_VERSION}_${goos}_$goarch.tar.gz" tmp
  tmp="$(mktemp -d)"
  fetch_verified "https://github.com/sqlc-dev/sqlc/releases/download/v$SQLC_VERSION/$file" "$(sqlc_sha256 "${goos}_$goarch")" "$tmp/$file"
  tar -C "$tmp" -xzf "$tmp/$file"
  mkdir -p "$BIN_DIR"
  mv "$tmp/sqlc" "$BIN_DIR/sqlc"
  rm -rf "$tmp"
  ok "sqlc ready: $("$BIN_DIR/sqlc" version)"
}

install_go() {
  if [ -x "$BIN_DIR/go" ] && [ "$("$BIN_DIR/go" env GOVERSION)" = "go$GO_VERSION" ]; then
    ok "go $GO_VERSION already installed"
    return
  fi
  if [ $check_only = 1 ]; then
    log "would install Go $GO_VERSION into $TOOLS_DIR/go"
    return
  fi
  local file="go$GO_VERSION.$goos-$goarch.tar.gz" tmp
  tmp="$(mktemp -d)"
  fetch_verified "https://go.dev/dl/$file" "$(go_sha256 "$goos-$goarch")" "$tmp/$file"
  rm -rf "$TOOLS_DIR/go"
  mkdir -p "$TOOLS_DIR"
  tar -C "$TOOLS_DIR" -xzf "$tmp/$file"
  rm -rf "$tmp"
  link_bin "$TOOLS_DIR/go/bin/go"
  link_bin "$TOOLS_DIR/go/bin/gofmt"
  ok "go ready: $("$BIN_DIR/go" version)"
  path_hint
}

# ── py: uv (pinned + checksummed) + CPython via uv ────────────────────────────
do_py() {
  if [ -x "$BIN_DIR/uv" ] && [ "$("$BIN_DIR/uv" --version | awk '{print $2}')" = "$UV_VERSION" ]; then
    ok "uv $UV_VERSION already installed"
  elif [ $check_only = 1 ]; then
    log "would install uv $UV_VERSION into $BIN_DIR"
  else
    local file="uv-$uvtarget.tar.gz" tmp
    tmp="$(mktemp -d)"
    fetch_verified "https://github.com/astral-sh/uv/releases/download/$UV_VERSION/$file" "$(uv_sha256 "$uvtarget")" "$tmp/$file"
    tar -C "$tmp" -xzf "$tmp/$file"
    mkdir -p "$BIN_DIR"
    cp "$tmp/uv-$uvtarget/uv" "$tmp/uv-$uvtarget/uvx" "$BIN_DIR/"
    rm -rf "$tmp"
  fi
  if [ $check_only = 1 ]; then
    log "would install CPython $PYTHON_VERSION via uv (if missing)"
    return
  fi
  "$BIN_DIR/uv" python install "$PYTHON_VERSION"
  ok "py ready: $("$BIN_DIR/uv" --version), python $PYTHON_VERSION"
  path_hint
}

# ── web: Node LTS tarball (pinned + checksummed) + pnpm via corepack ──────────
do_web() {
  if [ -x "$BIN_DIR/node" ] && [ "$("$BIN_DIR/node" --version)" = "v$NODE_VERSION" ]; then
    ok "node $NODE_VERSION already installed"
  elif [ $check_only = 1 ]; then
    log "would install Node $NODE_VERSION into $TOOLS_DIR/node"
  else
    local name="node-v$NODE_VERSION-$goos-$nodearch" tmp
    tmp="$(mktemp -d)"
    fetch_verified "https://nodejs.org/dist/v$NODE_VERSION/$name.tar.xz" "$(node_sha256 "$goos-$nodearch")" "$tmp/$name.tar.xz"
    tar -C "$tmp" -xJf "$tmp/$name.tar.xz"
    rm -rf "$TOOLS_DIR/node"
    mkdir -p "$TOOLS_DIR"
    mv "$tmp/$name" "$TOOLS_DIR/node"
    rm -rf "$tmp"
    local t
    for t in node npm npx corepack; do link_bin "$TOOLS_DIR/node/bin/$t"; done
  fi
  if [ $check_only = 1 ]; then
    log "would activate pnpm $PNPM_VERSION via corepack"
    return
  fi
  "$BIN_DIR/corepack" enable --install-directory "$BIN_DIR" pnpm
  COREPACK_ENABLE_DOWNLOAD_PROMPT=0 "$BIN_DIR/corepack" prepare "pnpm@$PNPM_VERSION" --activate
  ok "web ready: node $("$BIN_DIR/node" --version), pnpm $(COREPACK_ENABLE_DOWNLOAD_PROMPT=0 "$BIN_DIR/pnpm" --version)"
  path_hint
}

case "$component" in
  core) do_core ;;
  proto) do_proto ;;
  cpp) do_cpp ;;
  go) do_go ;;
  py) do_py ;;
  web) do_web ;;
  *) die "unknown component '$component' (core|proto|cpp|go|py|web)" ;;
esac
