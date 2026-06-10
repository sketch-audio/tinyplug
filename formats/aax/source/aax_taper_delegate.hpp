#pragma once

#include <algorithm> // std::clamp

#include "AAX_ITaperDelegate.h"

namespace tiny {

// MARK: - Fixed

template<typename T>
class Fixed_semanticsTaperDelegate final : public AAX_ITaperDelegate<T> {
public:

    Fixed_semanticsTaperDelegate(Fixed_semantics semantics) : _semantics{semantics} {}
    ~Fixed_semanticsTaperDelegate() override = default;
    
    AAX_ITaperDelegate<T>* Clone() const override
    {
        return new Fixed_semanticsTaperDelegate(_semantics);
    }

    T GetMaximumValue() const override
    {
        return static_cast<T>(_semantics.max_val);
    }

    T GetMinimumValue() const override
    {
        return static_cast<T>(_semantics.min_val);
    }

    T ConstrainRealValue(T value) const override
    {
        const auto tmin = static_cast<T>(_semantics.min_val);
        const auto tmax = static_cast<T>(_semantics.max_val);
        return std::clamp(value, tmin, tmax);
    }

    T NormalizedToReal(double normalizedValue) const override
    {
        return static_cast<T>(norm_to_plain(normalizedValue, _semantics));
    }

    double RealToNormalized(T realValue) const override
    {
        return plain_to_norm(static_cast<double>(realValue), _semantics);
    }

private:
    
    Fixed_semantics _semantics;
    
};

// MARK: - Real

template<typename T>
class Real_semanticsTaperDelegate final : public AAX_ITaperDelegate<T> {
public:

    Real_semanticsTaperDelegate(Real_semantics semantics) : _semantics{semantics} {}
    ~Real_semanticsTaperDelegate() override = default;

    AAX_ITaperDelegate<T>* Clone() const override
    {
        return new Real_semanticsTaperDelegate(_semantics);
    }

    T GetMaximumValue() const override
    {
        return static_cast<T>(_semantics.max_val);
    }

    T GetMinimumValue() const override
    {
        return static_cast<T>(_semantics.min_val);
    }

    T ConstrainRealValue(T value) const override
    {
        const auto tmin = static_cast<T>(_semantics.min_val);
        const auto tmax = static_cast<T>(_semantics.max_val);
        return std::clamp(value, tmin, tmax);
    }

    T NormalizedToReal(double normalizedValue) const override
    {
        return static_cast<T>(norm_to_plain(normalizedValue, _semantics));
    }

    double RealToNormalized(T realValue) const override
    {
        return plain_to_norm(static_cast<double>(realValue), _semantics);
    }

private:

    Real_semantics _semantics;
    
};

}