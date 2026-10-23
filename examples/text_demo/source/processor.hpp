#pragma once

#include <tinyplug/tinyplug.hpp>

namespace tiny::process {

// Passes audio through: the demo is its editor.
class Processor {
public:

    auto configure(const Config&) -> void {}
    auto reset(const Reset::Any&) -> void {}
    auto handle(const Event::Param&) -> void {}
    auto process(Context& context) -> void;

    auto latency_samps() const -> uint32_t { return 0; }
    auto tail_samps() const -> uint32_t { return 0; }

};
static_assert(Interface<Processor>);

} // namespace tiny::process
