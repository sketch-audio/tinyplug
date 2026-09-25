#pragma once

#include <array>
#include <atomic>
#include <charconv>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "AAX_CEffectParameters.h"

#include "adapters.hpp"
#include "alg_context.hpp"

#include "plug_info.hpp"

#include <tiny_plugin.hpp>

namespace tiny::aax {

/*
    The data model half of the two-component design.

    It owns everything that is not audio: the AAX parameter objects, the editor, the
    worker, undo history, and state chunks. It has no processor and no audio buffers —
    the DSP kernel lives in the algorithm's private data and is reachable only through
    coefficient packets out and the Direct Data return channel in.
*/
class Parameters : public AAX_CEffectParameters {
public:

    using Super = AAX_CEffectParameters;
    Parameters() : Super{}
    {
        _editor = std::make_unique<User_editor>(Edit_context{
            .actions = _actions.actor(),
            .format = Format::Aax,
            .state_adapter = _state_adapter.actor(),
            .undo_redo = _undo_history.actor(),
            .tasks = _tasks.actor(),
#if TINY_HAS_STATE
            .state = _state_link.actor(),
#endif
#if TINY_HAS_NOTES_IN
            // Held here until Direct Data carries them to the algorithm on the inbound ring.
            .notes = Note_sender{[this](const midi::Performance& e) { return _editor_notes.push(e); }},
#endif
        });
#if TINY_HAS_STATE
        _setup_state();
#endif

#if TINY_HAS_WORKER
        try_bind_worker(*_editor, Worker_editor_actor{
            [this](const auto& m) { return _worker_from_edit.push(m); }
        });
#endif
    }
    ~Parameters() override { _tasks.shutdown(); } // First: no task may outlive the editor or worker it captures.

    static AAX_CEffectParameters* AAX_CALLBACK Create()
    {
        return new Parameters;
    }

    AAX_Result EffectInit() override;
    AAX_Result NotificationReceived(AAX_CTypeID inNotificationType, const void* inNotificationData, uint32_t inNotificationDataSize) override;

    // Packet generation. The host calls GenerateCoefficients after every parameter
    // update, and timestamps every packet posted within it — which is what gives the
    // decoupled design its automation accuracy.
    AAX_Result UpdateParameterNormalizedValue(AAX_CParamID iParamID, double aValue, AAX_EUpdateSource inSource) override;
    AAX_Result GenerateCoefficients() override;
    AAX_Result TimerWakeup() override;

    // Fills the algorithm's private data blocks at every reset. The only channel that
    // reaches the algorithm synchronously *and* current at reset time — see Reset_state.
    AAX_Result ResetFieldData(AAX_CFieldIndex iFieldIndex, void* oData, uint32_t iDataSize) const override;

    // The Direct Data module's channel to us. Nothing here dereferences algorithm
    // memory; both directions carry framed bytes.
    AAX_Result SetCustomData(AAX_CTypeID iDataBlockID, uint32_t inDataSize, const void* iData) override;
    AAX_Result GetCustomData(AAX_CTypeID iDataBlockID, uint32_t inDataSize, void* oData, uint32_t* oDataWritten) const override;

    AAX_Result GetNumberOfChunks(int32_t* oNumChunks) const AAX_OVERRIDE;
	AAX_Result GetChunkIDFromIndex(int32_t iIndex, AAX_CTypeID* oChunkID) const AAX_OVERRIDE;
    AAX_Result GetChunkSize(AAX_CTypeID iChunkID, uint32_t* oSize) const AAX_OVERRIDE;
	AAX_Result GetChunk(AAX_CTypeID iChunkID, AAX_SPlugInChunk* oChunk) const AAX_OVERRIDE;
	AAX_Result SetChunk(AAX_CTypeID iChunkID, const AAX_SPlugInChunk* iChunk) AAX_OVERRIDE;
	AAX_Result CompareActiveChunk(const AAX_SPlugInChunk* iChunkP, AAX_CBoolean* oIsEqual) const AAX_OVERRIDE;

#if TINY_HAS_METERS
    auto read_meters(std::span<float> out) -> void
    {
        _mailbox.read(out);
    }
#endif
#if TINY_HAS_BLOCKS
    auto read_blocks(blocks::Frames<models::Resolved::Blocks>& out) -> void
    {
        _block_mailbox.read(out);
    }
#endif

#if TINY_HAS_STATE
    // [GUI] Once per frame, from the view: take in snapshots, send edits, close a waiting step.
    auto sync_state() -> void
    {
        const auto lock = std::lock_guard{_state_mutex};
        _state_link.sync();
    }
#endif

    auto get_editor() -> User_editor*
    {
        return _editor.get();
    }

    auto get_tasks() -> Task_manager*
    {
        return &_tasks;
    }

    // Framework-owned editor window-size cache (Parameters lifetime, survives view
    // recreation). Primed from the persisted chunk in SetChunk; read by the Gui to open
    // pre-sized; updated on every editor-initiated resize.
    auto resized(Rect_size size) -> void
    {
        _last_size = size;
    }

    auto get_last_size() const -> std::optional<Rect_size>
    {
        return _last_size;
    }

    // Undo history and action queue live on Parameters (plug-in lifetime), not in the
    // Gui, so the editor's Edit_context (built once at construction) stays valid across
    // window open/close and host preset loads are captured with the window closed.
    auto undo_history() -> Undo_history*
    {
        return &_undo_history;
    }

    auto actions() -> Action_queue*
    {
        return &_actions;
    }

    auto state_adapter() -> State_adapter*
    {
        return &_state_adapter;
    }

    auto drain_worker_to_editor() -> void
    {
#if TINY_HAS_WORKER
        try_drain_worker_to_editor(*_editor, _worker_to_edit);
#endif
    }

private:

    // Chunks. Pro Tools calls the chunk methods from any thread, several at once, so off main they
    // touch only `_chunk_image` (see State_image), which main keeps current in TimerWakeup. A
    // GetChunkSize/GetChunk pair keeps the image it sized. No parser is shared.
    struct Chunk_image {
        std::vector<char> bytes{}; // A whole AAX_SPlugInChunk: header, then fSize bytes of data.
        auto chunk() const -> const AAX_SPlugInChunk* { return reinterpret_cast<const AAX_SPlugInChunk*>(bytes.data()); }
    };
    using Chunk_ptr = std::shared_ptr<const Chunk_image>;

    auto _build_chunk(AAX_CChunkDataParser& parser) const -> void;
    auto _apply_chunk(const AAX_SPlugInChunk* chunk) -> AAX_Result;
    auto _refresh_chunk() const -> Chunk_ptr; // [main] Rebuilds the image, unless a load is waiting.
    auto _on_main() const -> bool { return std::this_thread::get_id() == _main_thread; }

    // Coefficient staging. One Coef_segment per port; a segment is rebuilt and posted
    // only when one of the parameters packed into it has changed.
    auto _mark_dirty(uint32_t address) -> void;
    auto _fill_segment(size_t segment) -> void;
    auto _post_segment(size_t segment) -> void;
    auto _post_runtime() -> void;
    auto _plain_value(uint32_t address) const -> double;

    std::unique_ptr<User_editor> _editor{};
    std::optional<Rect_size> _last_size{};
    Task_manager _tasks{};

    Undo_history _undo_history{};
    Action_queue _actions{};

#if TINY_HAS_NOTES_IN
    mutable Lock_free_queue<midi::Performance, process::Note_io::editor_capacity, Queue_concurrency::spsc> _editor_notes{}; // Drained from the const GetCustomData.
#endif

#if TINY_HAS_STATE
    // The editor's half of the document. `_state_mutex` guards the link between the GUI and
    // ResetFieldData, which seeds a rebuilt algorithm from it. The outbox holds one patch
    // until Direct Data pulls it; while it is full the editor keeps folding edits into the next.
    state::Editor_link<State_model> _state_link{};
    state::Snapshot_inbox<State_model> _state_inbox{};
    mutable std::mutex _state_mutex{};

    mutable std::mutex _state_out_mutex{};
    mutable std::array<unsigned char, state_edit_bytes> _state_out{};
    mutable State_edit_header _state_out_header{};
    mutable bool _state_out_full{};

    auto _setup_state() -> void;

    // The document as the editor holds it; the algorithm's copy trails it by a Direct Data wakeup.
    auto _state_record() const -> std::vector<std::byte>
    {
        const auto lock = std::lock_guard{_state_mutex};
        return state::encode_record(_state_link.view());
    }

    auto _load_state(std::span<const std::byte> record) -> void;
#endif

    // Snapshot all current param values in knob space (AAX normalized == knob).
    // Defined in the .cpp where the AAX parameter manager / adapters are visible.
    auto _snapshot_knob_params() -> std::array<double, num_params>;

    // State adapter lives on Parameters too (the editor's Edit_context references it
    // for life). save_model reads the current params via _snapshot_knob_params.
    State_adapter _state_adapter{{
        .load_model = []() {
            return State_adapter::Load_model{
                .param_tree = &User_params::param_tree(),
                .num_params = User_params::num_params
            };
        },
        .save_model = [this]() {
            const auto knob = _snapshot_knob_params();
            return State_adapter::Save_model{
                .version = 1,
                .param_tree = &User_params::param_tree(),
                .param_values = std::vector<double>(knob.begin(), knob.end()),
                .editor_state = _editor ? _editor->save_state() : State_map{},
#if TINY_HAS_STATE
                .state_record = _state_record(),
#endif
            };
        },
    }};

    // Packets out.
    std::array<Coef_segment, num_segments> _segments{};
    std::array<bool, num_segments> _segment_dirty{};
    uint64_t _seq{1};                   // 0 is the algorithm's "never seen" sentinel.
    Runtime_packet _runtime{.latency_seq = 0, .accepted_latency = 0, .offline = 0, .recording = 0, .delay_comp = 1, .pad = 0};
    std::atomic<bool> _runtime_dirty{true};

#if TINY_HAS_METERS
    // Meters in. The mailbox is the cache: retains every level the ring has delivered, so a
    // view created at any point simply reads it. Replaced `_last_meters` + dump.
    meters::Mailbox<tiny::models::Resolved::Meters> _mailbox{};
#endif
#if TINY_HAS_BLOCKS
    // Blocks in, posted from the Direct Data thread and read by the GUI.
    blocks::Mailbox<models::Resolved::Blocks> _block_mailbox{};
#endif

    // Latency. The kernel proposes from the algorithm; the host owns the accepted
    // value and hands it back through a notification.
    std::atomic<bool> _pending_latency{false};

    std::thread::id _main_thread{}; // Where EffectInit ran.
    mutable State_image<Chunk_ptr> _chunk_image{};
    mutable std::mutex _chunk_mutex{}; // Guards `_chunk_pairs`.
    mutable std::vector<std::pair<std::thread::id, Chunk_ptr>> _chunk_pairs{}; // Sized, not yet fetched.

#if TINY_HAS_WORKER
    // Worker channel. Editor <-> worker is direct (both live here). Processor <-> worker
    // traverses the Direct Data rings.
    using Worker_from_proc_q = Lock_free_queue<typename User_work::From_processor, User_work::inbound_capacity, Queue_concurrency::spsc>;
    using Worker_from_edit_q = Lock_free_queue<typename User_work::From_editor,    User_work::inbound_capacity, Queue_concurrency::spsc>;
    using Worker_to_proc_q   = Lock_free_queue<typename User_work::To_processor,   User_work::outbound_capacity>;
    using Worker_to_edit_q   = Lock_free_queue<typename User_work::To_editor,     User_work::outbound_capacity>;

    Worker_from_proc_q _worker_from_proc{};
    Worker_from_edit_q _worker_from_edit{};
    mutable Worker_to_proc_q _worker_to_proc{};   // Drained from the const GetCustomData.
    Worker_to_edit_q   _worker_to_edit{};

    User_worker _worker{
        Worker_replies{
            [this](const auto& m) { return _worker_to_proc.push(m); },
            [this](const auto& m) { return _worker_to_edit.push(m); }
        },
        _tasks.actor()
    };

    // Last so its destructor (which joins the worker thread) runs first.
    Worker_runner<User_worker> _worker_runner{&_worker, &_worker_from_proc, &_worker_from_edit};
#endif

};

} // namespace tiny::aax
