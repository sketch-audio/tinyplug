#include "window_registry.hpp"

#include <algorithm>
#include <mutex>
#include <vector>

namespace tiny {

namespace {

struct Entry {
    uint64_t id{};
    void* native{};
};

// Never more than a handful (one per open editor window in this binary), so a
// vector scan beats a map.
struct Table {
    std::mutex mutex{};
    std::vector<Entry> entries{};
    uint64_t next_id{1}; // 0 is the "unset" token.
};

auto table() -> Table&
{
    static auto instance = Table{};
    return instance;
}

} // namespace

auto Window_registry::add(void* native) -> Window_token
{
    if (!native) return {};

    auto& t = table();
    const auto lock = std::scoped_lock{t.mutex};
    const auto id = t.next_id++;
    t.entries.push_back(Entry{.id = id, .native = native});
    return Window_token{id};
}

auto Window_registry::remove(Window_token token) -> void
{
    if (!token) return;

    auto& t = table();
    const auto lock = std::scoped_lock{t.mutex};
    const auto [first, last] = std::ranges::remove_if(t.entries, [token](const auto& e) { return e.id == token.id; });
    t.entries.erase(first, last);
}

auto Window_registry::resolve(Window_token token) -> void*
{
    if (!token) return nullptr;

    auto& t = table();
    const auto lock = std::scoped_lock{t.mutex};
    const auto it = std::ranges::find(t.entries, token.id, &Entry::id);
    return it != t.entries.end() ? it->native : nullptr;
}

auto Window_registry::sole() -> void*
{
    auto& t = table();
    const auto lock = std::scoped_lock{t.mutex};
    return t.entries.size() == 1 ? t.entries.front().native : nullptr;
}

} // namespace tiny
