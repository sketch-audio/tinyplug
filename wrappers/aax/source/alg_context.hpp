#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "AAX.h"
#include "AAX_IMIDINode.h"

#include "byte_ring.hpp"

#include "plug_info.hpp"

#include "models/meters.hpp"
#include "models/params.hpp"
#include "processor.hpp"

#include <tinyplug/tinyplug.hpp>
#include <tiny_dsp/host_bypass.hpp>

namespace tiny::aax {

using User_params = params::Infos<models::Params>;
using User_meters = meters::Infos<models::Meters>;

inline constexpr auto num_params = User_params::num_params;
inline constexpr auto num_meters = User_meters::num_meters;

// The Pro Tools master bypass rides along as a pseudo-parameter one past the end of
// the user's address space, so it packs into the coefficient segments like any other
// value instead of needing its own port.
inline constexpr auto bypass_address = uint32_t{num_params};
inline constexpr auto num_coefs = size_t{num_params} + 1;

inline constexpr auto max_ichannels = size_t{2};
inline constexpr auto max_schannels = size_t{1};
inline constexpr auto max_ochannels = size_t{2};

// MARK: - coefficient segments

/*
    The framework has to carry an arbitrary (compile-time known) number of parameters
    into an algorithm whose context is a fixed struct of pointers. The trick is that
    AAX_FIELD_INDEX is just `offsetof(Ctx, member) / sizeof(void*)` — a pointer-slot
    index — so a plain C array of pointers occupies contiguous, arithmetically
    derivable field indices. Describe and the algorithm agree on them by both calling
    coef_field(), with no code generation and no named fields.

    Parameters are packed 15-to-a-segment so that a segment is exactly 128 bytes: the
    minimum host->DSP transfer size on HDX PCIe, and the size Avid recommends
    targeting. Consolidating also matters on Native, where each buffered port costs
    per-render-callback overhead.
*/
inline constexpr auto coefs_per_segment = size_t{15};
inline constexpr auto num_segments = (num_coefs + coefs_per_segment - 1) / coefs_per_segment;

inline constexpr auto segment_of(uint32_t address) -> size_t
{
    return address / coefs_per_segment;
}

inline constexpr auto slot_of(uint32_t address) -> size_t
{
    return address % coefs_per_segment;
}

#include AAX_ALIGN_FILE_BEGIN
#include AAX_ALIGN_FILE_ALG
#include AAX_ALIGN_FILE_END

// A block of parameter values in PLAIN space. Conversion happens on the data model
// side, off the real-time thread.
//
// `seq` lets the algorithm skip an untouched segment in O(1); when it does change,
// the algorithm diffs the values against its shadow copy to find which addresses
// actually moved. Doubles rather than floats because plain values may be large
// integers (sample counts) where a 24-bit mantissa is not enough.
struct Coef_segment {
    uint64_t seq;
    double value[coefs_per_segment];
};

// Static setup, posted once before the algorithm's instance-init callback runs.
struct Config_packet {
    double sample_rate;
    uint32_t max_frames;
    uint32_t pad;
};

// Host state that changes independently of parameters, and the latency the host has
// accepted. Posted whenever a notification changes it.
//
// `latency_seq` starts at 0 meaning "the host has not accepted a latency yet", so the
// algorithm does not mistake the packet's zero-initialised state for an instruction to
// drop the kernel's own initial latency to zero.
struct Runtime_packet {
    uint32_t latency_seq;
    uint32_t accepted_latency;
    uint8_t offline;
    uint8_t recording;
    uint8_t delay_comp;
    uint8_t pad;
};

// The algorithm's entire window on the world. Pointers only; the host repopulates
// every field before each call. Note that there is no route back to the data model
// object from here — that is the point.
struct Alg_context {
    float** audio_in;                 // AddAudioIn
    float** audio_out;                // AddAudioOut
    int32_t* num_frames;              // AddAudioBufferLength
    float* sample_rate;               // AddSampleRate
#if TINY_WANTS_SIDECHAIN
    int32_t* sidechain_index;         // AddSideChainIn
#endif
    AAX_IMIDINode* transport_node;    // AddMIDINode(Transport)

    const Config_packet* config;      // AddDataInPort
    const Runtime_packet* runtime;    // AddDataInPort

    struct Alg_state* state;          // AddPrivateData
    struct Return_ring* returns;      // AddPrivateData — algorithm -> data model
    struct Inbound_ring* inbound;     // AddPrivateData — data model -> algorithm

    const Coef_segment* coefs[num_segments];   // AddDataInPort × num_segments
};

#include AAX_ALIGN_FILE_BEGIN
#include AAX_ALIGN_FILE_RESET
#include AAX_ALIGN_FILE_END

// MARK: - field indices

// Everything Describe and the algorithm need to agree on, in one place.
enum : AAX_CFieldIndex {
    field_audio_in = AAX_FIELD_INDEX(Alg_context, audio_in),
    field_audio_out = AAX_FIELD_INDEX(Alg_context, audio_out),
    field_num_frames = AAX_FIELD_INDEX(Alg_context, num_frames),
    field_sample_rate = AAX_FIELD_INDEX(Alg_context, sample_rate),
#if TINY_WANTS_SIDECHAIN
    field_sidechain = AAX_FIELD_INDEX(Alg_context, sidechain_index),
#endif
    field_transport = AAX_FIELD_INDEX(Alg_context, transport_node),
    field_config = AAX_FIELD_INDEX(Alg_context, config),
    field_runtime = AAX_FIELD_INDEX(Alg_context, runtime),
    field_state = AAX_FIELD_INDEX(Alg_context, state),
    field_returns = AAX_FIELD_INDEX(Alg_context, returns),
    field_inbound = AAX_FIELD_INDEX(Alg_context, inbound),
    field_coefs_base = AAX_FIELD_INDEX(Alg_context, coefs),
};

inline constexpr auto coef_field(size_t segment) -> AAX_CFieldIndex
{
    return field_coefs_base + static_cast<AAX_CFieldIndex>(segment);
}

// MARK: - rings

// Sized for roughly 50 ms of production at the worst plausible callback rate — the
// Direct Data timer drains at only ~33 Hz, and the host may split render buffers
// down to 32 samples during automation.
inline constexpr auto return_ring_bytes = std::bit_ceil(
    std::max(size_t{8192}, (size_t{num_meters} + 1) * size_t{4096})
);
inline constexpr auto inbound_ring_bytes = size_t{4096};

struct Return_ring : Byte_ring<return_ring_bytes> {};
struct Inbound_ring : Byte_ring<inbound_ring_bytes> {};

// MARK: - custom data bridge

/*
    The Direct Data module has no channel of its own to the data model, so it uses the
    SDK's documented inter-module hook (AAX_IEffectParameters::Get/SetCustomData). Both
    directions carry framed bytes, never pointers, so the algorithm -> data model path
    stays value semantics end to end.
*/

// Algorithm -> data model: one drained ring entry, pushed by Direct Data.
inline constexpr auto custom_data_return = AAX_CTypeID{'tRTN'};

// Data model -> algorithm: one pending worker reply, pulled by Direct Data.
inline constexpr auto custom_data_worker_reply = AAX_CTypeID{'tWKR'};

// Header for a `custom_data_return` block; `payload_bytes` of payload follow it.
struct Return_block {
    uint32_t kind{};
    uint32_t payload_bytes{};
};
static_assert(sizeof(Return_block) == 8);

// MARK: - algorithm state

// The algorithm's persistent memory. Lives in an AAX private data block, which the
// host allocates and — per the SDK — never relocates between render callbacks.
// Constructed and destroyed by the instance-init callback (see alg_proc.cpp), NOT by
// ResetFieldData: that runs on the host and its block is copied into the algorithm's
// memory pool, which would require this type to be trivially relocatable.
struct Alg_state {
    plugin::Processor processor{};
    Host_bypass bypass{};

    // Shadow of the last coefficient state, for diffing. Seeded so that the first
    // delivered segment reports every parameter as changed.
    std::array<double, num_coefs> shadow{};
    std::array<uint64_t, num_segments> shadow_seq{};

    std::array<float, num_meters> meters{};
    std::array<double, num_meters> last_meters{};

    std::array<const float*, max_ichannels> ibuffers{};
    std::array<const float*, max_schannels> sbuffers{};
    std::array<float*, max_ochannels> obuffers{};

    uint32_t latency_seq{};
    uint32_t accepted_latency{};
    uint32_t reported_latency{}; // Last value proposed to the host — don't feedback latency changes.
    bool was_skipped{}; // Detects the can_skip -> processing edge, for Resync_params.
    int8_t last_render_mode{-1};
    bool constructed{};
};

// AddPrivateData takes an int32_t size. This bounds the Alg_state object, not the
// kernel's heap allocations — those happen normally in reset() on Native.
static_assert(sizeof(Alg_state) < size_t{1} << 31, "Alg_state is too large for an AAX private data block.");

} // namespace tiny::aax
