# vcpkg overlay ports

| Port | Why | Upstream copy |
|---|---|---|
| `nanobind` | Official port depends on vcpkg's `python3` port, which builds CPython + OpenSSL/zlib/libffi/sqlite from source (against PRD §10.1 lean footprint). This overlay is identical except that dependency is removed; `vyuha_core` builds against the Python that imports it (`find_package(Python 3.13)`). | `ports/nanobind` at the `builtin-baseline` in `../vcpkg.json` (nanobind 3.1.0) |

When bumping the vcpkg baseline, re-copy `portfile.cmake`, `usage` and `vcpkg.json` from the new baseline and remove `python3` from `dependencies` again.
