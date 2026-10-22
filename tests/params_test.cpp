// Infos: one copy of the specs in address order, with the tree and AU orders as address lists.

#include <algorithm>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

#include <audio_bench/audio_bench.hpp>
#include <tiny_core/tiny_params.hpp>

namespace {

using audio_bench::Tests;
using audio_bench::expect_true;
using namespace tiny::params;

auto spec(uint32_t address, const char* identifier, const char* name) -> Node
{
    return Spec{.identity = {.address = address, .identifier = identifier}, .name = name};
}

// The tree lists addresses out of order, so all three orders differ.
struct Tree {
    static auto build_tree() -> Node
    {
        return Group{.nodes = {
            spec(2, "c", "C"),
            Group{.name = "Group", .identifier = "group", .nodes = {spec(0, "a", "A"), spec(1, "b", "B")}},
        }};
    }
};

struct Pinned : Tree {
    enum class Address : uint32_t { A, B, C, Num_params };
    static auto au_order() -> std::vector<Address> { return {Address::B, Address::C, Address::A}; }
};

struct Unpinned : Tree {
    enum class Address : uint32_t { A, B, C, Num_params };
};

auto equal(std::span<const uint32_t> order, std::vector<uint32_t> expected) -> bool
{
    return std::ranges::equal(order, expected);
}

auto add_tests() -> void
{
    Tests::add("specs are in address order", [] {
        const auto& specs = Infos<Pinned>::param_specs();
        expect_true(specs.size() == 3, "three specs");
        for (auto i = uint32_t{}; i < specs.size(); ++i) {
            expect_true(specs[i].identity.address == i, "spec at its address");
        }
        expect_true(Infos<Pinned>::param_spec(2).name == "C", "lookup by address");
    });

    Tests::add("presentation order is tree order", [] {
        expect_true(equal(Infos<Pinned>::presentation_order(), {2, 0, 1}), "C, then the group's A and B");
    });

    Tests::add("AU order is the model's, or address order", [] {
        expect_true(equal(Infos<Pinned>::au_order(), {1, 2, 0}), "pinned");
        expect_true(equal(Infos<Unpinned>::au_order(), {0, 1, 2}), "address order without au_order()");
    });

    Tests::add("the tree is kept as authored", [] {
        const auto& root = std::get<Group>(Infos<Pinned>::param_tree());
        expect_true(root.nodes.size() == 2 && std::holds_alternative<Group>(root.nodes[1]), "root and group");
    });
}

} // namespace

auto main() -> int
{
    add_tests();
    return audio_bench::Tests::run_all() == 0 ? 0 : 1;
}
