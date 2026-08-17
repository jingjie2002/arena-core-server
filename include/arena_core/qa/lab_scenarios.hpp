#pragma once

#include "arena_core/core/cpp17_guard.hpp"
#include "arena_core/qa/lab_run.hpp"

#include <map>

namespace arena_core::qa {

std::map<LabScenarioKind, LabScenarioHandler> make_lab_scenario_handlers();

}  // namespace arena_core::qa
