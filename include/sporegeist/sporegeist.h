#ifndef SPOREGEIST_H
#define SPOREGEIST_H

#include "sporegeist/agent_loop.h"
#include "sporegeist/allocator.h"
#include "sporegeist/actor.h"
#include "sporegeist/context.h"
#include "sporegeist/executor_boundary.h"
#include "sporegeist/graph.h"
#include "sporegeist/hash.h"
#include "sporegeist/journal.h"
#include "sporegeist/memory.h"
#include "sporegeist/model_adapter.h"
#include "sporegeist/orchestrator.h"
#include "sporegeist/policy.h"
#include "sporegeist/policy_config.h"
#include "sporegeist/policy_gate.h"
#include "sporegeist/recommendation.h"
#include "sporegeist/run_config.h"
#include "sporegeist/risk.h"
#include "sporegeist/schema.h"
#include "sporegeist/sexpr.h"
#include "sporegeist/sim_executor.h"
#include "sporegeist/sim_config.h"
#include "sporegeist/status.h"

#define SPG_VERSION_MAJOR 0
#define SPG_VERSION_MINOR 1
#define SPG_VERSION_PATCH 0
#define SPG_VERSION_STRING "0.1.0"

#endif
