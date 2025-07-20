#pragma once

#include <algorithm>

#include "AAX_ITaperDelegate.h"
#include "user_plug.h"

namespace tiny::aax {

template<typename T>
class FloatSemanticsTaperDelegate final : public AAX_ITaperDelegate<T> {
public:

    using Float_semantics = tiny::params::Float_semantics;

    FloatSemanticsTaperDelegate(Float_semantics semantics) : _semantics{std::move(semantics)} {}
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

template <typename Id>
auto flatten_tree_to_ids(const Param_node<Id>& root) -> std::vector<std::string>
{
    auto result = std::vector<std::string>{};

    const auto visit = [&](const auto& node, const auto& self) -> void {
        std::visit([&](const auto& item) {
            using T = std::decay_t<decltype(item)>;

            if constexpr (std::is_same_v<T, Param_spec<Id>>) {
                const auto raw = utils::to_underlying(item.id);
                const auto hex = std::format("0x{:08X}", raw);
                result.push_back(hex);
            }
            else if constexpr (std::is_same_v<T, Param_group<Id>>) {
                for (const auto& child : item.nodes) {
                    self(child, self);
                }
            }
        }, node);
    };

    visit(root, visit);
    return result;
}

}