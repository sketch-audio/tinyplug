#pragma once

#include <cstddef>
#include <cstdint>

#include <tiny_core/wire.hpp>

namespace tiny::aax {

// What an entry in an AAX ring carries. The two directions use disjoint sets, but
// share one enum so the framing code stays single-source.
enum class Ring_kind : uint32_t {
    // Algorithm -> data model.
    Meter = 0,
    Worker_from_processor,
    Propose_latency,

    // Data model -> algorithm.
    Worker_to_processor,
    Editor_note, // One `midi::Performance`.
};

// Payload for Ring_kind::Meter.
struct Ring_meter {
    uint32_t address{};
    uint32_t pad{};
    double value{};
};
static_assert(sizeof(Ring_meter) == 16);

// Payload for Ring_kind::Propose_latency.
struct Ring_latency {
    uint32_t samples{};
    uint32_t pad{};
};
static_assert(sizeof(Ring_latency) == 8);

// Algorithm -> data model: the algorithm is the local producer, the Direct Data timer the remote
// consumer. Data model -> algorithm: the Direct Data timer is the remote producer.
template<size_t Capacity>
using Byte_ring = wire::Byte_ring<Capacity, Ring_kind>;

} // namespace tiny::aax
