#include "direct_data.hpp"

#include <algorithm>
#include <cstring>

#include "AAX_IEffectParameters.h"
#include "AAX_IPrivateDataAccess.h"
#include "AAX_Errors.h"

namespace tiny::aax {

namespace {

// The ring head, laid out exactly as the first 16 bytes of the private data block.
struct Ring_head {
    uint64_t write_pos{};
    uint64_t read_pos{};
};
static_assert(sizeof(Ring_head) == 16);

} // namespace

AAX_Result Direct_data::TimerWakeup_PrivateDataAccess(AAX_IPrivateDataAccess* private_data)
{
    if (private_data == nullptr) return AAX_ERROR_NULL_OBJECT;

    _drain_returns(private_data);
    _push_worker_replies(private_data);

    return AAX_SUCCESS;
}

// MARK: - algorithm -> data model

auto Direct_data::_drain_returns(AAX_IPrivateDataAccess* access) -> void
{
    auto* params = EffectParameters();
    if (params == nullptr) return;

    auto head = Ring_head{};
    if (access->ReadPortDirect(field_returns, Return_ring::offset_write_pos, sizeof(head), &head) != AAX_SUCCESS) {
        return;
    }

    const auto pending = head.write_pos - head.read_pos;
    if (pending == 0) return;

    // Everything below write_pos is committed and will not be overwritten until we
    // advance read_pos, so this read needs no seqlock retry.
    const auto to_read = static_cast<size_t>(std::min<uint64_t>(pending, _scratch.size()));
    const auto start = static_cast<uint32_t>(head.read_pos & Return_ring::mask);
    const auto first = std::min<size_t>(to_read, Return_ring::capacity - start);

    if (access->ReadPortDirect(field_returns, Return_ring::offset_data + start,
                               static_cast<uint32_t>(first), _scratch.data()) != AAX_SUCCESS) {
        return;
    }
    if (first < to_read) {
        if (access->ReadPortDirect(field_returns, Return_ring::offset_data,
                                   static_cast<uint32_t>(to_read - first), _scratch.data() + first) != AAX_SUCCESS) {
            return;
        }
    }

    // Forward whole entries only; a truncated tail waits for the next wakeup.
    alignas(8) auto block = std::array<unsigned char, sizeof(Return_block) + Return_ring::max_payload_bytes>{};

    auto offset = size_t{};
    while (offset + sizeof(Ring_header) <= to_read) {
        auto header = Ring_header{};
        std::memcpy(&header, _scratch.data() + offset, sizeof(header));

        const auto total = sizeof(Ring_header) + ring_align_up(header.payload_bytes);
        if (header.payload_bytes > Return_ring::max_payload_bytes) break; // Corrupt; cannot resync.
        if (offset + total > to_read) break;

        // Reframe as a custom-data block: [Return_block][payload].
        const auto out = Return_block{.kind = header.kind, .payload_bytes = header.payload_bytes};
        std::memcpy(block.data(), &out, sizeof(out));
        std::memcpy(block.data() + sizeof(out), _scratch.data() + offset + sizeof(header), header.payload_bytes);

        params->SetCustomData(custom_data_return,
                              static_cast<uint32_t>(sizeof(out) + header.payload_bytes),
                              block.data());

        offset += total;
    }

    if (offset == 0) return;

    const auto new_read_pos = head.read_pos + offset;
    access->WritePortDirect(field_returns, Return_ring::offset_read_pos, sizeof(new_read_pos), &new_read_pos);
}

// MARK: - data model -> algorithm

auto Direct_data::_push_worker_replies([[maybe_unused]] AAX_IPrivateDataAccess* access) -> void
{
#if TINY_HAS_WORKER
    auto* params = EffectParameters();
    if (params == nullptr) return;

    using To_processor = typename User_worker::Model::To_processor;
    if constexpr (std::is_same_v<To_processor, std::monostate>) {
        return;
    }
    else {
        auto head = Ring_head{};
        if (access->ReadPortDirect(field_inbound, Inbound_ring::offset_write_pos, sizeof(head), &head) != AAX_SUCCESS) {
            return;
        }

        auto write_pos = head.write_pos;
        auto wrote_any = false;

        for (;;) {
            auto msg = To_processor{};
            auto written = uint32_t{};
            const auto result = params->GetCustomData(custom_data_worker_reply, sizeof(msg), &msg, &written);
            if (result != AAX_SUCCESS || written != sizeof(msg)) break;

            const auto payload_bytes = static_cast<uint32_t>(sizeof(msg));
            const auto total = sizeof(Ring_header) + ring_align_up(payload_bytes);
            if ((write_pos - head.read_pos) + total > Inbound_ring::capacity) break; // Full: drop.

            alignas(8) auto entry = std::array<unsigned char, sizeof(Ring_header) + Inbound_ring::max_payload_bytes>{};
            const auto entry_header = Ring_header{
                .kind = static_cast<uint32_t>(Ring_kind::Worker_to_processor),
                .payload_bytes = payload_bytes
            };
            std::memcpy(entry.data(), &entry_header, sizeof(entry_header));
            std::memcpy(entry.data() + sizeof(entry_header), &msg, payload_bytes);

            const auto start = static_cast<uint32_t>(write_pos & Inbound_ring::mask);
            const auto first = std::min<size_t>(total, Inbound_ring::capacity - start);
            access->WritePortDirect(field_inbound, Inbound_ring::offset_data + start,
                                    static_cast<uint32_t>(first), entry.data());
            if (first < total) {
                access->WritePortDirect(field_inbound, Inbound_ring::offset_data,
                                        static_cast<uint32_t>(total - first), entry.data() + first);
            }

            write_pos += total;
            wrote_any = true;
        }

        if (wrote_any) {
            // Publish last, so the algorithm never observes a partially written entry.
            access->WritePortDirect(field_inbound, Inbound_ring::offset_write_pos, sizeof(write_pos), &write_pos);
        }
    }
#endif
}

} // namespace tiny::aax
