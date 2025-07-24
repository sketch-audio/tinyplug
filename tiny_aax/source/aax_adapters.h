#pragma once

#include <algorithm>

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

}