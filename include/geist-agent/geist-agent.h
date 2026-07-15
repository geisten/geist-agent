#ifndef GEIST_AGENT_H
#define GEIST_AGENT_H

#include "geist-agent/agent_loop.h"
#include "geist-agent/allocator.h"
#include "geist-agent/actor.h"
#include "geist-agent/context.h"
#include "geist-agent/executor_boundary.h"
#include "geist-agent/graph.h"
#include "geist-agent/hash.h"
#include "geist-agent/journal.h"
#include "geist-agent/memory.h"
#include "geist-agent/model_adapter.h"
#include "geist-agent/orchestrator.h"
#include "geist-agent/policy.h"
#include "geist-agent/policy_config.h"
#include "geist-agent/policy_gate.h"
#include "geist-agent/recommendation.h"
#include "geist-agent/run_config.h"
#include "geist-agent/risk.h"
#include "geist-agent/schema.h"
#include "geist-agent/sexpr.h"
#include "geist-agent/sim_executor.h"
#include "geist-agent/sim_config.h"
#include "geist-agent/status.h"

#define SPG_VERSION_MAJOR 0
#define SPG_VERSION_MINOR 1
#define SPG_VERSION_PATCH 0
#define SPG_VERSION_STRING "0.1.0"

#endif
