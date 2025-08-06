#pragma once

#include <algorithm> // std::clamp

#include "AAX_ITaperDelegate.h"

namespace tiny {

template<typename T>
class Float_semanticsTaperDelegate final : public AAX_ITaperDelegate<T> {
public:

    Float_semanticsTaperDelegate(Float_semantics semantics) : _semantics{semantics} {}
    ~Float_semanticsTaperDelegate() = default;

    AAX_ITaperDelegate<T>* Clone() const override
    {
        return new Float_semanticsTaperDelegate(_semantics);
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

}