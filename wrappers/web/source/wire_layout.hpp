#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <utility>

#include <tiny_core/wire.hpp>
#include <tiny_models.hpp>

// The web format's channels between the processor module (AudioWorklet) and the controller (main thread).
// Every payload is trivially copyable and laid out for wire.js, which reads the same offsets.
namespace tiny::web {

enum class Kind : std::uint32_t {
    // Controller -> processor.
    Param_set = 0,  // Param_set
    Param_ramp,     // Param_ramp
    Bypass,         // Flag
    Host_reset,     // (none): the page asks for a reset; whether it restarts is `Stream_sync`'s call.
    Transport,      // Transport
    Midi,           // Midi: from the page or Web MIDI, at an offset.
    Editor_note,    // midi::Performance, from the editor.
    Worker_reply,   // User_work::To_processor, as bytes.

    // Processor -> controller.
    Meter = 64,     // Meter
    Latency,        // Samples: the latency the processor now has, applied.
    Midi_out,       // Midi_out
    Worker_message, // User_work::From_processor, as bytes.
};

struct Param_set {
    double value{}; // Plain space.
    std::uint32_t address{};
    std::int32_t offset{}; // Frames into the next quantum.
};
static_assert(sizeof(Param_set) == 16);

struct Param_ramp {
    double value{}; // Plain space, the ramp's target.
    std::uint32_t address{};
    std::int32_t samples{};
    std::int32_t offset{};
    std::uint32_t pad{};
};
static_assert(sizeof(Param_ramp) == 24);

struct Flag {
    std::uint32_t on{};
    std::uint32_t pad{};
};
static_assert(sizeof(Flag) == 8);

// The page's transport: a position anchored to a processor frame, so it advances between messages.
struct Transport {
    double tempo{};             // BPM; 0 when the page has none.
    std::int64_t position{};    // Samples, at `anchor`.
    std::int64_t anchor{};      // The processor's frame count the position refers to.
    std::int32_t numer{};       // 0 when the page has no signature.
    std::int32_t denom{};
    std::uint32_t playing{};
    std::uint32_t pad{};
};
static_assert(sizeof(Transport) == 40);

struct Midi {
    std::uint8_t status{};
    std::uint8_t data1{};
    std::uint8_t data2{};
    std::uint8_t pad{};
    std::int32_t offset{};
};
static_assert(sizeof(Midi) == 8);

struct Midi_out {
    std::int32_t frame{}; // Within the quantum it was sent in.
    std::uint32_t size{};
    std::uint8_t bytes[8]{};
};
static_assert(sizeof(Midi_out) == 16);

struct Meter {
    std::uint32_t address{};
    float value{};
};
static_assert(sizeof(Meter) == 8);

struct Samples {
    std::uint32_t samples{};
    std::uint32_t pad{};
};
static_assert(sizeof(Samples) == 8);

inline constexpr auto inbound_bytes = std::size_t{16 * 1024};
inline constexpr auto outbound_bytes = std::size_t{16 * 1024};

using Inbound_ring = wire::Byte_ring<inbound_bytes, Kind>;
using Outbound_ring = wire::Byte_ring<outbound_bytes, Kind>;

// One store per block address, the same type on both sides of the pump.
#if TINY_HAS_BLOCKS
template<typename Seq> struct Stores_of;
template<std::uint32_t... I> struct Stores_of<std::integer_sequence<std::uint32_t, I...>> {
    using type = std::tuple<wire::Block_store<blocks::Frame_at<models::Resolved::Blocks, I>>...>;
};
using Block_stores = Stores_of<std::make_integer_sequence<std::uint32_t, User_blocks::num_blocks>>::type;
#endif

#if TINY_HAS_STATE
using State_model = models::Resolved::State;
inline constexpr auto state_writers = state::writers_of<State_model>;

// Controller -> processor: the latest edit and its sequence. Latest wins, as AAX's inbox: an edit carries whole
// documents, and the link retries what the processor refuses.
struct State_edit_frame {
    std::uint32_t seq{};
    std::uint32_t pad{};
    std::array<std::byte, state::edit_bytes<State_model, state_writers>> bytes{};
};

// Processor -> controller: {gen, snapshot}, only where the processor writes.
struct State_snapshot_frame {
    std::uint32_t gen{};
    std::uint32_t pad{};
    std::array<std::byte, state::snapshot_bytes<State_model>> bytes{};
};

// Controller -> processor: a host load's whole document, for `on_session_load`. Latest wins; the pump moves it
// before any edit, so an edit that follows a load is applied to the loaded document.
using State_load_store = wire::Block_store<State_model>;
using State_edit_store = wire::Block_store<State_edit_frame>;
using State_snapshot_store = wire::Block_store<State_snapshot_frame>;
#endif

// One block address's store, for the JS side: wire::Block_store at `offset`.
struct Block_entry {
    std::uint32_t offset{};
    std::uint32_t frame_bytes{};
    std::uint32_t slot_offset{};
};

// What the processor module publishes about its memory, read once by the JS side. Byte offsets into the
// module's memory; capacities in bytes.
struct Wire_table {
    std::uint32_t version{1};
    std::uint32_t inbound{};
    std::uint32_t inbound_capacity{};
    std::uint32_t outbound{};
    std::uint32_t outbound_capacity{};
    std::uint32_t inputs{};     // max_channels pointers' worth of float buffers, each max_frames long.
    std::uint32_t sidechain{};
    std::uint32_t outputs{};
    std::uint32_t max_channels{};
    std::uint32_t max_frames{};
    std::uint32_t params{};     // num_params doubles, plain: the values `configure` comes up holding.
    std::uint32_t num_params{};
    std::uint32_t blocks{};     // num_blocks Block_entry.
    std::uint32_t num_blocks{};
    Block_entry state_edit{};   // Zero offsets without a state model.
    Block_entry state_snapshot{};
    Block_entry state_load{};
};

} // namespace tiny::web
