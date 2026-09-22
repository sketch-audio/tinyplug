#pragma once

// The model-free half of the framework. A plug-in's `models/*.hpp` include this and nothing
// from <tinyplug/...>: models are read by the framework, never the other way round.

#include "denormal_guard.hpp"
#include "gesture_recognizers.hpp"
#include "host_formatter.hpp"
#include "lock_free_queue.hpp"
#include "platform_defs.hpp"
#include "state_adapter.hpp"
#include "task_manager.hpp"
#include "tiny_input.hpp"
#include "tiny_log.hpp"
#include "tiny_meters.hpp"
#include "tiny_notifications.hpp"
#include "tiny_params.hpp"
#include "tiny_state.hpp"
#include "tiny_utils.hpp"
#include "tiny_work.hpp"
#include "value_helper.hpp"
#include "window_token.hpp"
