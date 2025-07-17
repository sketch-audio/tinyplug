#pragma once

#include <algorithm>

#include "AAX_ITaperDelegate.h"
#include "user_plug.h"

namespace tiny::aax {

template<typename T, typename Id>
class ControlAdapterTaperDelegate final : public AAX_ITaperDelegate<T> {
public:

    ControlAdapterTaperDelegate(Param_spec<Id> param) : mParam(std::move(param)) {}
    ~ControlAdapterTaperDelegate() = default;

    AAX_ITaperDelegate<T>* Clone() const override
    {
        return new ControlAdapterTaperDelegate(mParam);
    }

    T GetMaximumValue() const override
    {
        return mParam.max_val;
    }

    T GetMinimumValue() const override
    {
        return mParam.min_val;
    }

    T ConstrainRealValue(T value) const override
    {
        return std::clamp(value, mParam.min_val, mParam.max_val);
    }

    T NormalizedToReal(double normalizedValue) const override
    {
        const auto& adapter = mParam.knob_adapter;
        return adapter.norm_to_plain(mParam, static_cast<T>(normalizedValue));
    }

    double RealToNormalized(T realValue) const override
    {
        const auto& adapter = mParam.knob_adapter;
        return static_cast<double>(adapter.plain_to_norm(mParam, realValue));
    }

private:

    Param_spec<Id> mParam;
    
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