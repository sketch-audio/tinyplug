#pragma once

// The model-aware half of the framework. Generated <tiny_models.hpp> comes first: it pulls in
// the core vocabulary and the plug-in's models, so nothing below has an ordering rule.
#include <tiny_models.hpp>

#include "tiny_edit.hpp"
#include "tiny_events.hpp"
#include "tiny_processor.hpp"
#include "tiny_state_link.hpp"
#include "tiny_view.hpp"
#include "tiny_worker.hpp"
