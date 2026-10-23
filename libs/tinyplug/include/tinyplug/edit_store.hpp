#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <utility>

#include <tiny_core/edit_record.hpp>
#include <tiny_core/state_adapter.hpp> // Framework_keys
#include <tiny_core/state_merge.hpp>
#include <tiny_core/state_record.hpp>
#include <tiny_core/view_model.hpp>

#include <tiny_models.hpp>

#include "undo_history.hpp"

namespace tiny {

// The editor's sizing: `sizing()` when it declares one, else its `preferred_size()`.
template<typename E>
constexpr auto sizing_of() -> Sizing
{
    if constexpr (requires { { E::sizing() } -> std::convertible_to<Sizing>; }) return E::sizing();
    else return Sizing{.preferred = E::preferred_size()};
}

// The view model's optional `on_host_load`. A template, so the call is discarded when it isn't declared.
template<typename V>
auto call_load_hook(V& view, std::span<const double> knob_params, const Load_info& info) -> void
{
    if constexpr (view::Has_load_hook<V>) V::on_host_load(view, knob_params, info);
    else {
        (void)view;
        (void)knob_params;
        (void)info;
    }
}

// The framework's editor-side state: the view model (`models/view.hpp`), the window size and the
// count of host loads. Wrapper-owned and as long-lived as the wrapper, so a load with the window
// closed just writes it. The view's undoable part and (when `Sizing::undoable`) the size are undo
// sources; saves read copies published on main, from any thread.
class Edit_store {
public:

    using View = User_view;
    using Undoable = typename View::Undoable;
    using Restored = typename View::Restored;

    // What the editor holds. Main thread.
    class Handle {
    public:
        Handle() = default;
        explicit Handle(Edit_store* store) : _store{store} {}

        // The view's undoable part, as the editor should draw it.
        auto get() const -> const Undoable& { return _store->_view.undoable; }

        // Change the undoable part. It becomes an undo step at the end of the frame, or joins the
        // gesture or transaction that is open.
        template<typename F>
        auto edit(F&& f) const -> bool
        {
            auto next = _store->_view;
            f(next);
            if (std::memcmp(&next.undoable, &_store->_view.undoable, sizeof(Undoable)) == 0) return false;
            _store->_view.undoable = next.undoable;
            ++_store->_generation;
            return true;
        }

        // The restored part, edited in place: saved with the session, never an undo step.
        auto restored() const -> Restored& { return _store->_view.restored; }

        // Bumped by every change to the undoable part: an edit, an undo, a load.
        auto generation() const -> std::uint32_t { return _store->_generation; }

        // Host loads so far: compare across frames to notice one.
        auto loads() const -> std::uint32_t { return _store->_loads; }

    private:
        Edit_store* _store{};
    };

    // The window, as the editor holds it. Main thread.
    class Size {
    public:
        Size() = default;
        explicit Size(Edit_store* store) : _store{store} {}

        // Ask for this size, clamped to `sizing()`. The wrapper asks the host at the frame's end, and the size
        // the window really gets arrives in a later frame's `View_context::logical_size`. Inside
        // `undo_redo.begin()` / `end()` with `Sizing::undoable`, it's part of that step.
        auto request(std::int32_t w, std::int32_t h) const -> void { if (_store) _store->_request({w, h}); }

        // The size the session holds.
        auto get() const -> Rect_size { return _store ? _store->_size : Rect_size{}; }

    private:
        Edit_store* _store{};
    };

    explicit Edit_store(Sizing sizing) : _sizing{sizing}, _size{sizing.preferred}, _size_baseline{sizing.preferred}
    {
        _publish(); // A save may come before the first frame or load.
    }
    Edit_store(const Edit_store&) = delete;
    auto operator=(const Edit_store&) -> Edit_store& = delete;

    auto handle() -> Handle { return Handle{this}; }
    auto size_handle() -> Size { return Size{this}; }

    auto bind(Undo_history& history) -> void
    {
        _history = &history;
        if constexpr (has_view) {
            history.bind_state(Undo_history::Source::View, Undo_history::State_hooks{
                .commit = [this] { return _commit_view(); },
                .replay = [this](const std::byte* base, const std::byte* next, std::size_t n) { _replay_view(base, next, n); },
            });
        }
        if (_sizing.undoable) {
            history.bind_state(Undo_history::Source::Size, Undo_history::State_hooks{
                .commit = [this] { return _commit_size(); },
                .replay = [this](const std::byte* base, const std::byte* next, std::size_t n) { _replay_size(base, next, n); },
            });
        }
    }

    // MARK: main thread

    // Each frame's end: view edits become a step (or join one that is open), a size the host changed
    // outside any gesture is just the size now, and what saves read is published.
    auto end_frame() -> void
    {
        if (const auto request = std::exchange(_requested, std::nullopt); request && _apply) _apply(*request); // The editor's resize.
        if constexpr (has_view) _commit_view();
        const auto open = _history && _history->open();
        if (!_sizing.undoable || !open) _size_baseline = _size;
        _publish_if_changed();
    }

    auto sizing() const -> const Sizing& { return _sizing; }

    // The size the window opens at: the session's, or the preferred one.
    auto size() const -> Rect_size { return _size; }

    // The window is this size now: a host's frame drag, or the host echoing our own request.
    auto resized(Rect_size size) -> void
    {
        _size = _sizing.clamp(size);
        _size_set = true;
    }

    // How an undo resizes an open window. Empty while it's closed: an undo then only moves the size
    // the window opens at.
    auto set_apply(std::function<void(Rect_size)> apply) -> void { _apply = std::move(apply); }

    // A host load, inside its undo step (after push_host_load and the document). `record` is the
    // session's edit record; `legacy_size` the size an older build kept in its editor map.
    auto load(Load_info info, const std::optional<Edit_record>& record, std::optional<Rect_size> legacy_size,
              std::span<const double> knob_params) -> void
    {
        // A session says the whole size, preferred when it has none; a preset says nothing about it.
        if (record && record->size) resized(*record->size);
        else if (legacy_size) resized(*legacy_size);
        else if (info.kind == Load_info::Kind::Session) {
            _size = _sizing.preferred;
            _size_set = false;
        }
        _size_baseline = _size;

        if constexpr (has_view) {
            const auto was = _view.undoable;
            auto next = _view;
            if (record) {
                next.undoable = state::decode_record_or_default<Undoable>(record->undoable);
                next.restored = state::decode_record_or_default<Restored>(record->restored);
                info.view_restored = true;
            }
            else if (info.kind == Load_info::Kind::Session) {
                next = View{}; // A session from before the view model: what a new instance holds.
            }
            // A preset carries no view: it keeps what the editor had, for the hook to adjust.
            call_load_hook(next, knob_params, info);

            _view = next;
            if (_history && std::memcmp(&was, &_view.undoable, sizeof(Undoable)) != 0) {
                _history->amend_host_state(Undo_history::Source::View, as_bytes(was), as_bytes(_view.undoable), sizeof(Undoable));
            }
            _baseline = _view.undoable;
            ++_generation;
        }
        else {
            (void)info;
            (void)knob_params;
        }
        ++_loads;
        _publish();
    }

    // MARK: any thread

    // The edit record a save writes: the view's two parts and the size, as last published on main.
    auto save() const -> Edit_record
    {
        const auto lock = std::lock_guard{_mutex};
        return _published;
    }

    // Bumped by every publish: a save that caches what it built compares it.
    auto revision() const -> std::uint32_t { return _revision.load(std::memory_order_acquire); }

    // What an older build reads from the editor map: the window size, when one was set.
    auto legacy_keys() const -> Framework_keys
    {
        const auto lock = std::lock_guard{_mutex};
        auto keys = Framework_keys{};
        if (_published.size) keys.size = std::pair{_published.size->w, _published.size->h};
        return keys;
    }

private:

    Sizing _sizing{};
    Undo_history* _history{};

    View _view{};
    Undoable _baseline{};
    std::uint32_t _generation{};
    std::uint32_t _loads{};

    Rect_size _size{};
    Rect_size _size_baseline{};
    bool _size_set{};
    std::function<void(Rect_size)> _apply{};
    std::optional<Rect_size> _requested{}; // The editor's, applied at the frame's end.

    // Held as the size now, so a resize inside a transaction is part of its step before the host echoes it.
    auto _request(Rect_size size) -> void
    {
        resized(size);
        _requested = _size;
    }

    View _published_view{};
    std::optional<Rect_size> _published_size{};
    mutable std::mutex _mutex{};
    Edit_record _published{};
    std::atomic<std::uint32_t> _revision{};

    template<typename T>
    static auto as_bytes(const T& value) -> const std::byte* { return reinterpret_cast<const std::byte*>(&value); }

    auto _commit_view() -> Undo_history::Commit
    {
        if (!_history || std::memcmp(&_baseline, &_view.undoable, sizeof(Undoable)) == 0) return Undo_history::Commit::None;
        _history->record_state(Undo_history::Source::View, as_bytes(_baseline), as_bytes(_view.undoable), sizeof(Undoable));
        _baseline = _view.undoable;
        return Undo_history::Commit::Recorded;
    }

    auto _replay_view(const std::byte* base, const std::byte* next, std::size_t n) -> void
    {
        if (n != sizeof(Undoable)) return;
        auto b = Undoable{};
        auto x = Undoable{};
        std::memcpy(&b, base, n);
        std::memcpy(&x, next, n);
        state::merge_into(_view.undoable, b, x); // Only what the step changed.
        state::merge_into(_baseline, b, x);
        ++_generation;
    }

    auto _commit_size() -> Undo_history::Commit
    {
        if (!_history || _size == _size_baseline) return Undo_history::Commit::None;
        _history->record_state(Undo_history::Source::Size, as_bytes(_size_baseline), as_bytes(_size), sizeof(Rect_size));
        _size_baseline = _size;
        return Undo_history::Commit::Recorded;
    }

    auto _replay_size(const std::byte*, const std::byte* next, std::size_t n) -> void
    {
        if (n != sizeof(Rect_size)) return;
        auto size = Rect_size{};
        std::memcpy(&size, next, n);
        _size = _size_baseline = size;
        _size_set = true;
        if (_apply) _apply(size);
    }

    auto _publish_if_changed() -> void
    {
        const auto size = _size_set ? std::optional{_size} : std::nullopt;
        if (std::memcmp(&_published_view, &_view, sizeof(View)) == 0 && size == _published_size) return;
        _publish();
    }

    auto _publish() -> void
    {
        auto record = Edit_record{};
        if (_size_set) record.size = _size;
        if constexpr (has_view) {
            record.undoable = state::encode_record(_view.undoable);
            record.restored = state::encode_record(_view.restored);
        }
        _published_view = _view;
        _published_size = record.size;
        const auto lock = std::lock_guard{_mutex};
        _published = std::move(record);
        _revision.fetch_add(1, std::memory_order_release);
    }
};

// The editor half of a host load, after its parameters and document, inside its undo step. `legacy` is
// the editor map a session or preset from before the view model carries (empty for none): the
// framework's keys come out of it (the window size; a native exporter's preset name and id, which make
// the load a preset's), and what's left reaches the view model as Load_info::legacy.
inline auto load_editor_state(Edit_store& store, State_map legacy, const std::optional<Edit_record>& record,
                              std::span<const double> knob_params, Load_info info = {}) -> void
{
    const auto keys = framework_keys::take(legacy);
    if (!keys.preset_name.empty()) {
        info.kind = Load_info::Kind::Preset;
        info.name = keys.preset_name;
        info.id = keys.preset_id;
    }
    info.legacy = legacy.empty() ? nullptr : &legacy;
    const auto size = keys.size ? std::optional{Rect_size{keys.size->first, keys.size->second}} : std::nullopt;
    store.load(info, record, size, knob_params);
}

// What a session saves for older builds in its editor map: the framework's keys, nothing else.
inline auto legacy_editor_map(const Edit_store& store) -> State_map
{
    auto map = State_map{};
    framework_keys::write(map, store.legacy_keys());
    return map;
}

} // namespace tiny
