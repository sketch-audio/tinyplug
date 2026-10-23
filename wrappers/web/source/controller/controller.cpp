// The web format's controller module: everything but the DSP, on the page's main thread. Param authority (the page
// is the DAW), the editor, undo, the state link, the worker. The processor module is across the pump, which moves
// frames between this module's rings and its own.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <emscripten/emscripten.h>
#include <nlohmann/json.hpp>

#include <tiny_plugin.hpp>
#include <tiny_platform/platform_view.hpp>

#include "plug_info.hpp"
#include "../manifest.hpp"
#include "../wire_layout.hpp"

namespace tiny::web {

// What the pump needs from this module's memory. Byte offsets; ring capacities in bytes.
struct Controller_table {
    std::uint32_t version{1};
    std::uint32_t to_processor{};   // Main thread -> processor (params, bypass, transport, editor notes).
    std::uint32_t from_worker{};    // Worker thread -> processor (replies).
    std::uint32_t from_processor{}; // Processor -> main thread (meters, latency, notes out).
    std::uint32_t to_worker{};      // Processor -> worker thread.
    std::uint32_t ring_capacity{};
    std::uint32_t blocks{};         // num_blocks Block_entry, twins of the processor's.
    std::uint32_t num_blocks{};
    Block_entry state_edit{};
    Block_entry state_snapshot{};
    Block_entry state_load{};
};

namespace {

constexpr auto num_params = User_params::num_params;
constexpr auto num_meters = User_meters::num_meters;
using Ring = wire::Byte_ring<inbound_bytes, Kind>;
static_assert(inbound_bytes == outbound_bytes, "The pump moves between rings of one size.");

template<typename T>
auto address_of(const T& x) -> std::uint32_t { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&x)); }

class Controller {
public:

    Controller()
    {
        _editor.emplace(Edit_context{
            .actions = _actions.actor(),
            .format = Format::Web,
            .state_adapter = _state_adapter.actor(),
            .undo_redo = _undo_history.actor(),
            .tasks = _tasks.actor(),
            .view = _edit_store.handle(),
#if TINY_HAS_STATE
            .state = _state_link.actor(),
#endif
#if TINY_HAS_NOTES_IN
            .notes = Note_sender{[this](const midi::Performance& e) { return _to_processor.push_value(Kind::Editor_note, e); }},
#endif
        });
        _undo_history.bind_actions(_actions.actor());
        _edit_store.bind(_undo_history);

#if TINY_HAS_STATE
        state::connect_remote(_state_link, [this](std::span<const std::byte> bytes, std::uint32_t seq) {
            if (bytes.size() != _edit_scratch.bytes.size()) return false;
            _edit_scratch.seq = seq;
            std::copy(bytes.begin(), bytes.end(), _edit_scratch.bytes.begin());
            _state_edit.publish(_edit_scratch);
            return true;
        }, _state_inbox);
        _state_link.bind(_undo_history);
#endif

#if TINY_HAS_WORKER
        try_bind_worker(*_editor, Worker_editor_actor{[this](const auto& m) { return _worker_from_edit.push(m); }});
        // On the worker's thread, after each cycle: the processor's messages in, the worker's replies out.
        _worker_runner.set_post_cycle([this] {
            _to_worker.drain([this](Kind kind, const void* payload, std::uint32_t bytes) {
                using From_processor = typename User_work::From_processor;
                if (kind != Kind::Worker_message || bytes != sizeof(From_processor)) return;
                auto m = From_processor{};
                std::memcpy(&m, payload, sizeof(m));
                _worker_from_proc.push(m);
            });
            auto reply = typename User_work::To_processor{};
            while (_worker_to_proc.pop(reply)) {
                if (!_from_worker.push(Kind::Worker_reply, &reply, static_cast<std::uint32_t>(sizeof(reply)))) break;
            }
        });
#endif

        for (auto addr = decltype(num_params){}; addr < num_params; ++addr) {
            _host[addr] = params::Value_helper::default_value(User_params::param_spec(addr), params::Space::Host);
        }

        _table = Controller_table{
            .to_processor = address_of(_to_processor),
            .from_worker = address_of(_from_worker),
            .from_processor = address_of(_from_processor),
            .to_worker = address_of(_to_worker),
            .ring_capacity = Ring::capacity,
        };
#if TINY_HAS_BLOCKS
        [this]<std::uint32_t... I>(std::integer_sequence<std::uint32_t, I...>) {
            ((_block_entries[I] = Block_entry{
                .offset = address_of(std::get<I>(_stores)),
                .frame_bytes = std::tuple_element_t<I, Block_stores>::frame_bytes,
                .slot_offset = std::tuple_element_t<I, Block_stores>::offset_slots,
            }), ...);
        }(std::make_integer_sequence<std::uint32_t, User_blocks::num_blocks>{});
        _table.blocks = address_of(_block_entries);
        _table.num_blocks = User_blocks::num_blocks;
#endif
#if TINY_HAS_STATE
        _table.state_edit = {address_of(_state_edit), State_edit_store::frame_bytes, State_edit_store::offset_slots};
        _table.state_snapshot = {address_of(_state_snapshot), State_snapshot_store::frame_bytes, State_snapshot_store::offset_slots};
        _table.state_load = {address_of(_state_load), State_load_store::frame_bytes, State_load_store::offset_slots};
#endif
    }

    ~Controller()
    {
        _tasks.shutdown(); // First: no task may outlive the editor or worker it captures.
        close_view();
    }

    // The editor on the page's canvas (a CSS selector). Its frame loop then runs the view's `run_frame`.
    auto open_view(const char* selector) -> Rect_size
    {
        close_view();
        const auto size = _edit_store.size();
        auto delegate = std::make_shared<View_delegate>(size,
            [this](View_context& context) { this->_draw(context); });
        _edit_store.set_apply([this](Rect_size s) { if (_view) _view->resize(s.w, s.h); });
        _view = Platform_views::make_owning(delegate);
        _view->on_create();
        _editor->on_gui_create(Gui_info{.window = _view->token()});
        _selector = selector;
        _view->receive_parent(_selector.data());
        _view->resize(size.w, size.h);
        _tasks.bind_main(std::this_thread::get_id());
        _view->on_show();
        _editor->on_gui_show();
        return size;
    }

    auto close_view() -> void
    {
        if (!_view) return;
        _editor->on_gui_hide();
        view_impl::close_session(_receiver, _ui_params, _actions, _undo_history);
        _edit_store.set_apply({});
        _view->on_hide();
        _editor->on_gui_destroy();
        _view->on_destroy();
        _view = nullptr;
    }

    auto table() const -> const Controller_table* { return &_table; }

    auto start(double sample_rate) -> void
    {
#if TINY_HAS_WORKER
        _worker_runner.start(sample_rate);
#else
        (void)sample_rate;
#endif
    }

    // The values the processor module configures with, plain.
    auto plain_params(double* out) const -> void
    {
        for (auto addr = decltype(num_params){}; addr < num_params; ++addr) {
            out[addr] = params::Value_helper::host_to_plain(_host[addr], User_params::param_spec(addr).semantics);
        }
    }

    // The page's automation: the host's own edit, so no undo step.
    auto set_param(std::uint32_t address, double plain) -> void
    {
        if (address >= num_params) return;
        const auto& spec = User_params::param_spec(address);
        _host[address] = params::Value_helper::plain_to_host(plain, spec.semantics);
        _to_processor.push_value(Kind::Param_set, Param_set{.value = plain, .address = address});
    }

    auto param_knob(std::uint32_t address) const -> double
    {
        if (address >= num_params) return 0;
        return params::Value_helper::host_to_knob(_host[address], User_params::param_spec(address).semantics);
    }

    auto send(Kind kind, const void* payload, std::uint32_t bytes) -> bool { return _to_processor.push(kind, payload, bytes); }

    // As the editor does inside a draw: queued, handled at the next tick.
    auto push_action(const User_action& action) -> void { _actions.actor().push(action); }
    auto undo() -> void { _undo_history.actor().undo(); }
    auto redo() -> void { _undo_history.actor().redo(); }

    // Once a frame on the main thread: `run_frame` without the draw, until the view exists.
    auto tick() -> void
    {
        _tasks.bind_main(std::this_thread::get_id());
        _drain_from_processor();
#if TINY_HAS_STATE
        auto snapshot = State_snapshot_frame{};
        if (const auto seq = _state_snapshot.read(snapshot); seq != 0 && seq != _snapshot_seen) {
            _snapshot_seen = seq;
            state::post_snapshot<State_model>(_state_inbox, snapshot.bytes, snapshot.gen);
        }
        _state_link.sync();
#endif
#if TINY_HAS_BLOCKS
        [this]<std::uint32_t... I>(std::integer_sequence<std::uint32_t, I...>) {
            (_read_block(std::integral_constant<std::uint32_t, I>{}), ...);
        }(std::make_integer_sequence<std::uint32_t, User_blocks::num_blocks>{});
#endif
        if (_view) return; // The view's frame runs the editor's half: actions, undo, main-thread tasks, meters.
#if TINY_HAS_METERS
        _mailbox.read(_meters);
#endif
        _tasks.run_main();

        for (auto addr = decltype(num_params){}; addr < num_params; ++addr) _ui_params[addr] = param_knob(addr);
        auto state = Processor_state{.params = _ui_params};
        view_impl::resolve_frame(_receiver, _ui_params, _actions, _undo_history, state, [](auto, auto) {});
#if TINY_HAS_WORKER
        try_drain_worker_to_editor(*_editor, _worker_to_edit);
#endif
    }

    auto meters() const -> const float* { return _meters.data(); }
    auto latency() const -> std::uint32_t { return _latency; }

    // MIDI the processor sent since the last take, as Midi_out records.
    auto take_midi(Midi_out* out, std::uint32_t max) -> std::uint32_t
    {
        const auto n = std::min<std::uint32_t>(max, static_cast<std::uint32_t>(_midi.size()));
        std::copy_n(_midi.begin(), n, out);
        _midi.erase(_midi.begin(), _midi.begin() + n);
        return n;
    }

    auto save_state() -> const std::string&
    {
        _saved = _state_adapter.session_state().dump();
        return _saved;
    }

    // A host load: the page's setState. One undo step, the processor told.
    auto load_state(std::string_view text) -> bool
    {
        const auto doc = nlohmann::ordered_json::parse(text, nullptr, false);
        if (doc.is_discarded()) return false;
        this->_update_state(_state_adapter.param_values(doc), _state_adapter.editor_state(doc), _state_adapter.state_record(doc),
                            _state_adapter.edit_record(doc));
        return true;
    }

private:

    // The editor's frame, as the native views': `run_frame` with this controller as the receiver.
    auto _draw(View_context& context) -> void
    {
#if TINY_HAS_WORKER
        try_drain_worker_to_editor(*_editor, _worker_to_edit);
#endif
        for (auto addr = decltype(num_params){}; addr < num_params; ++addr) _ui_params[addr] = param_knob(addr);
        view_impl::run_frame(_receiver, _ui_params, _view_meters, _ui_blocks, context, &*_editor, _actions, _undo_history, _tasks,
            [this](auto w, auto h) { if (_view) _view->resize(static_cast<std::int32_t>(w), static_cast<std::int32_t>(h)); });
    }

    Controller_table _table{};
    std::unique_ptr<Platform_view> _view{};
    std::string _selector{};
    std::array<double, num_meters> _view_meters{};
    blocks::Frames<models::Resolved::Blocks> _ui_blocks{}; // Retained between draws.

    // The wire, this side. One writer per ring.
    Ring _to_processor{};
    Ring _from_worker{};
    Ring _from_processor{};
    Ring _to_worker{};
#if TINY_HAS_BLOCKS
    Block_stores _stores{};
    std::array<Block_entry, User_blocks::num_blocks> _block_entries{};
    std::array<std::uint64_t, User_blocks::num_blocks> _block_seen{};
    blocks::Mailbox<models::Resolved::Blocks> _block_mailbox{};
#endif
#if TINY_HAS_STATE
    State_load_store _state_load{};
    State_edit_store _state_edit{};
    State_snapshot_store _state_snapshot{};
    State_edit_frame _edit_scratch{};
    std::uint64_t _snapshot_seen{};
    state::Snapshot_inbox<State_model> _state_inbox{};
    state::Editor_link<State_model> _state_link{};
#endif
#if TINY_HAS_METERS
    meters::Mailbox<models::Resolved::Meters> _mailbox{};
#endif
    std::array<float, num_meters> _meters{};

    std::array<double, num_params> _host{}; // Host space: the authority.
    std::array<double, num_params> _ui_params{}; // Knob space, as the editor sees them.
    std::uint32_t _latency{};
    std::vector<Midi_out> _midi{};
    std::string _saved{};

    Task_manager _tasks{};
    Undo_history _undo_history{};
    Action_queue _actions{};
    Edit_store _edit_store{sizing_of<User_editor>()}; // The editor's view state, window size and load count.
    std::optional<User_editor> _editor{};

#if TINY_HAS_WORKER
    using Worker_from_proc_q = Lock_free_queue<typename User_work::From_processor, User_work::inbound_capacity, Queue_concurrency::spsc>;
    using Worker_from_edit_q = Lock_free_queue<typename User_work::From_editor,    User_work::inbound_capacity, Queue_concurrency::spsc>;
    using Worker_to_proc_q   = Lock_free_queue<typename User_work::To_processor,   User_work::outbound_capacity>;
    using Worker_to_edit_q   = Lock_free_queue<typename User_work::To_editor,     User_work::outbound_capacity>;
    Worker_from_proc_q _worker_from_proc{};
    Worker_from_edit_q _worker_from_edit{};
    Worker_to_proc_q   _worker_to_proc{};
    Worker_to_edit_q   _worker_to_edit{};
    User_worker _worker{
        Worker_replies{
            [this](const auto& m) { return _worker_to_proc.push(m); },
            [this](const auto& m) { return _worker_to_edit.push(m); }
        },
        _tasks.actor()
    };
    Worker_runner<User_worker> _worker_runner{&_worker, &_worker_from_proc, &_worker_from_edit};
#endif

    Ui_receiver _receiver{
        .get_param = [this](std::uint32_t id) { return param_knob(id); },
#if TINY_HAS_METERS
        .read_meters = [this](std::span<float> out) {
            _mailbox.read(out);
            std::copy_n(out.begin(), std::min(out.size(), _meters.size()), _meters.begin()); // The page's copy.
        },
#endif
#if TINY_HAS_BLOCKS
        .read_blocks = [this](blocks::Frames<models::Resolved::Blocks>& out) { _block_mailbox.read(out); },
#endif
        .action_handler = [this](const User_action& action) { this->_handle_action(action); },
#if TINY_HAS_STATE
        .sync_state = [this]() { _state_link.sync(); },
#endif
        .end_frame = [this]() { _edit_store.end_frame(); },
    };

    State_adapter _state_adapter{{
        .load_model = []() {
            return State_adapter::Load_model{.param_tree = &User_params::param_tree(), .num_params = User_params::num_params};
        },
        .save_model = [this]() {
            auto knob = std::vector<double>(num_params);
            for (auto addr = decltype(num_params){}; addr < num_params; ++addr) knob[addr] = param_knob(addr);
            return State_adapter::Save_model{
                .version = 1,
                .param_tree = &User_params::param_tree(),
                .param_values = std::move(knob),
#if TINY_HAS_STATE
                .state_record = state::encode_record(_state_link.view()),
#endif
                .edit_record = encode_edit_record(_edit_store.save()),
                .legacy_editor = legacy_editor_map(_edit_store),
            };
        },
    }};

    auto _drain_from_processor() -> void
    {
        _from_processor.drain([this](Kind kind, const void* payload, std::uint32_t) {
            switch (kind) {
#if TINY_HAS_METERS
                case Kind::Meter: {
                    const auto& m = *static_cast<const Meter*>(payload);
                    if (m.address < num_meters) _mailbox.post(m.address, m.value);
                    break;
                }
#endif
                case Kind::Latency: _latency = static_cast<const Samples*>(payload)->samples; break;
                case Kind::Midi_out: if (_midi.size() < 4096) _midi.push_back(*static_cast<const Midi_out*>(payload)); break;
                default: break;
            }
        });
    }

#if TINY_HAS_BLOCKS
    template<std::uint32_t I>
    auto _read_block(std::integral_constant<std::uint32_t, I> i) -> void
    {
        auto frame = blocks::Frame_at<models::Resolved::Blocks, I>{};
        const auto seq = std::get<I>(_stores).read(frame);
        if (seq == 0 || seq == _block_seen[I]) return;
        _block_seen[I] = seq;
        _block_mailbox.post(i, frame);
    }
#endif

    // An editor action. The page's own host has no gesture API, so a gesture only brackets undo.
    auto _handle_action(const User_action& action) -> void
    {
        if (const auto* a = std::get_if<Set_param>(&action)) {
            const auto& spec = User_params::param_spec(a->address);
            _host[a->address] = params::Value_helper::knob_to_host(a->value, spec.semantics);
            _to_processor.push_value(Kind::Param_set, Param_set{
                .value = params::Value_helper::knob_to_plain(a->value, spec.semantics),
                .address = a->address,
            });
        }
    }

    auto _load_param(std::uint32_t address, double knob) -> void
    {
        const auto& spec = User_params::param_spec(address);
        _host[address] = params::Value_helper::knob_to_host(knob, spec.semantics);
        _to_processor.push_value(Kind::Param_set, Param_set{.value = params::Value_helper::knob_to_plain(knob, spec.semantics), .address = address});
    }

    auto _knob_snapshot() const -> std::array<double, num_params>
    {
        auto out = std::array<double, num_params>{};
        for (auto addr = decltype(num_params){}; addr < num_params; ++addr) out[addr] = param_knob(addr);
        return out;
    }

    // As CLAP's: persistent params land, missing ones go to defaults, one undo step for all of it.
    auto _update_state(const Maybe_values<double>& knob_values, State_map legacy_editor, [[maybe_unused]] std::span<const std::byte> record,
                       const std::optional<Edit_record>& edit_record) -> void
    {
        using namespace params;
#if TINY_HAS_STATE
        const auto doc = state::decode_record_or_default<State_model>(record);
#endif
        const auto before = _knob_snapshot();
        for (auto i = decltype(num_params){}; i < num_params; ++i) {
            const auto& spec = User_params::param_spec(i);
            const auto knob = i < knob_values.size() ? knob_values[i] : std::optional<double>{Value_helper::default_value(spec, Space::Knob)};
            if (knob && State_rules::is_persistent(spec)) _load_param(i, *knob);
        }
        const auto after = _knob_snapshot();
        auto changes = std::vector<Set_param>{};
        _undo_history.push_host_load(before, after, changes);
#if TINY_HAS_STATE
        _state_load.publish(doc); // The processor's copy, as CLAP's on_session_load: a load is not an edit.
        _state_link.load(doc); // Into the same step.
#endif
        load_editor_state(_edit_store, std::move(legacy_editor), edit_record, after);
    }
};

Controller* instance{};

} // namespace
} // namespace tiny::web

using tiny::web::instance;

extern "C" {

EMSCRIPTEN_KEEPALIVE auto tiny_ctl_create() -> const tiny::web::Controller_table*
{
    if (!instance) instance = new tiny::web::Controller{};
    return instance->table();
}

EMSCRIPTEN_KEEPALIVE auto tiny_ctl_start(double sample_rate) -> void { instance->start(sample_rate); }

// Opens the editor on a canvas; returns its logical size as w << 16 | h.
EMSCRIPTEN_KEEPALIVE auto tiny_ctl_open_view(const char* selector) -> std::uint32_t
{
    const auto size = instance->open_view(selector);
    return static_cast<std::uint32_t>(size.w) << 16 | static_cast<std::uint32_t>(size.h);
}
EMSCRIPTEN_KEEPALIVE auto tiny_ctl_close_view() -> void { instance->close_view(); }
EMSCRIPTEN_KEEPALIVE auto tiny_ctl_plain_params(double* out) -> void { instance->plain_params(out); }
EMSCRIPTEN_KEEPALIVE auto tiny_ctl_tick() -> void { instance->tick(); }
EMSCRIPTEN_KEEPALIVE auto tiny_ctl_set_param(std::uint32_t address, double plain) -> void { instance->set_param(address, plain); }
EMSCRIPTEN_KEEPALIVE auto tiny_ctl_param_knob(std::uint32_t address) -> double { return instance->param_knob(address); }
EMSCRIPTEN_KEEPALIVE auto tiny_ctl_send(std::uint32_t kind, const void* payload, std::uint32_t bytes) -> bool
{
    return instance->send(static_cast<tiny::web::Kind>(kind), payload, bytes);
}
EMSCRIPTEN_KEEPALIVE auto tiny_ctl_meters() -> const float* { return instance->meters(); }
EMSCRIPTEN_KEEPALIVE auto tiny_ctl_latency() -> std::uint32_t { return instance->latency(); }
EMSCRIPTEN_KEEPALIVE auto tiny_ctl_take_midi(tiny::web::Midi_out* out, std::uint32_t max) -> std::uint32_t { return instance->take_midi(out, max); }

// What the editor would push during a draw: 0 start, 1 set (knob), 2 end. For hosts without a view, and tests.
EMSCRIPTEN_KEEPALIVE auto tiny_ctl_action(std::uint32_t kind, std::uint32_t address, double knob) -> void
{
    switch (kind) {
        case 0: instance->push_action(tiny::Action_start{.address = address}); break;
        case 1: instance->push_action(tiny::Set_param{.address = address, .value = knob}); break;
        case 2: instance->push_action(tiny::Action_end{.address = address}); break;
        default: break;
    }
}
EMSCRIPTEN_KEEPALIVE auto tiny_ctl_undo() -> void { instance->undo(); }
EMSCRIPTEN_KEEPALIVE auto tiny_ctl_redo() -> void { instance->redo(); }

EMSCRIPTEN_KEEPALIVE auto tiny_ctl_save_state() -> const char* { return instance->save_state().c_str(); }
EMSCRIPTEN_KEEPALIVE auto tiny_ctl_load_state(const char* text) -> bool { return instance->load_state(text); }

EMSCRIPTEN_KEEPALIVE auto tiny_ctl_manifest() -> const char*
{
    static const auto text = tiny::web::manifest();
    return text.c_str();
}

} // extern "C"
