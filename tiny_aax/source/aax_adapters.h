#pragma once

#include <algorithm>
#include <charconv>

#include "AAX.h"
#include "AAX_ITaperDelegate.h"

#include "tinyplug/tinyplug.h"

namespace tiny::aax {

// MARK: - FloatSemanticsTaperDelegate

template<typename T>
class FloatSemanticsTaperDelegate final : public AAX_ITaperDelegate<T> {
public:

    using Float_semantics = tiny::Float_semantics;

    FloatSemanticsTaperDelegate(Float_semantics semantics) : _semantics{semantics} {}
    ~FloatSemanticsTaperDelegate() = default;

    AAX_ITaperDelegate<T>* Clone() const override
    {
        return new FloatSemanticsTaperDelegate(_semantics);
    }

    T GetMaximumValue() const override
    {
        return _semantics.max_val;
    }

    T GetMinimumValue() const override
    {
        return _semantics.min_val;
    }

    T ConstrainRealValue(T value) const override
    {
        return std::clamp(value, _semantics.min_val, _semantics.max_val);
    }

    T NormalizedToReal(double normalizedValue) const override
    {
        const auto& adapter = _semantics.knob_adapter;
        return adapter.norm_to_plain(_semantics, static_cast<T>(normalizedValue));
    }

    double RealToNormalized(T realValue) const override
    {
        const auto& adapter = _semantics.knob_adapter;
        return static_cast<double>(adapter.plain_to_norm(_semantics, realValue));
    }

private:

    Float_semantics _semantics;
    
};

// MARK: - tree_to_aax_id

template<Enum Id>
inline auto tree_to_aax_id(const Param_node<Id>& root) -> std::vector<std::string>
{
    auto result = std::vector<std::string>{};

    const auto visit = [&](const Param_node<Id>& node, const auto& self) -> void {
        std::visit(
            Inline_visitor{
                [&](const Param_spec<Id>& spec) {
                    result.push_back(std::format("0x{:08X}", to_underlying(spec.id)));
                },
                [&](const Param_group<Id>& group) {
                    for (const auto& child : group.nodes) self(child, self);
                }
            }
        , node);
    };
    
    visit(root, visit);
    return result;
}

// MARK: - AAX id <--> tiny

inline auto tiny_id_to_aax(uint32_t tiny_id) -> std::optional<std::string>
{
    // Prefix: "0x", Base: 16 (with leading 0s)
    return std::format("0x{:08x}", tiny_id);
}

inline auto aax_id_to_tiny(const char* aax_id) noexcept -> std::optional<uint32_t>
{
    static constexpr auto aax_id_prefix = "0x";
    static constexpr auto aax_id_base = 16;

    if (!aax_id) return std::nullopt;

    const auto size = std::strlen(aax_id);
    const auto poff = std::strlen(aax_id_prefix);

    auto tiny_id = uint32_t{};
    const auto [ptr, ec] = std::from_chars(
        aax_id + poff,
        aax_id + size,
        tiny_id,
        aax_id_base
    );

    if (ec != std::errc{} || ptr != aax_id + size) {
        return std::nullopt;
    }

    return tiny_id;
}

}