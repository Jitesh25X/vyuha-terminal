#include <nanobind/nanobind.h>
#include <nanobind/stl/string_view.h>

#include "vyuha/core/version.h"

// Python bindings for backtests (clock, SimBroker, RiskGate, OMS arrive in M4.3).
// NOLINTBEGIN: NB_MODULE expands to nanobind's C-API glue (static globals, casts, varargs); not our code.
NB_MODULE(vyuha_core, m) {
  m.doc() = "Vyuha engine core";
  m.def("version", &vyuha::version, "Engine version string.");
}
// NOLINTEND
