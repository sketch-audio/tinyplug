#pragma once

#include <algorithm> // std::clamp
#include <cmath>     // std::lround
#include <string>
#include <type_traits>

#include "AAX_CString.h"
#include "AAX_IDisplayDelegate.h"

#include <tiny_core/host_formatter.hpp>
#include <tiny_core/value_helper.hpp>

namespace tiny {

// Parameter text through Host_formatter, so AAX reads and writes the same strings as every other format.
// `T` is the AAX parameter's value type; its values are plain space.
template<typename T>
class Semantics_display_delegate final : public AAX_IDisplayDelegate<T> {
public:

    Semantics_display_delegate(params::Semantics::Any semantics) : _semantics{std::move(semantics)} {}
    ~Semantics_display_delegate() override = default;

    AAX_IDisplayDelegate<T>* Clone() const override
    {
        return new Semantics_display_delegate(_semantics);
    }

    bool ValueToString(T value, AAX_CString* valueString) const override
    {
        *valueString = AAX_CString(_to_string(value).c_str());
        return true;
    }

    // Control surfaces: drop the units before truncating.
    bool ValueToString(T value, int32_t maxNumChars, AAX_CString* valueString) const override
    {
        auto str = _to_string(value);
        const auto max_chars = static_cast<size_t>(std::max(maxNumChars, int32_t{}));
        if (str.size() > max_chars) {
            if (const auto space = str.rfind(' '); space != std::string::npos && space <= max_chars) str.resize(space);
            else str.resize(max_chars);
        }
        *valueString = AAX_CString(str.c_str());
        return true;
    }

    bool StringToValue(const AAX_CString& valueString, T* value) const override
    {
        using namespace params;
        const auto plain = Host_formatter::to_value(std::string{valueString.CString()}, _semantics);
        if (!plain) return false;
        const auto clamped = Value_helper::clamp(*plain, _semantics);
        if constexpr (std::is_same_v<T, bool>) *value = clamped >= 0.5;
        else if constexpr (std::is_integral_v<T>) *value = static_cast<T>(std::lround(clamped));
        else *value = static_cast<T>(clamped);
        return true;
    }

private:

    auto _to_string(T value) const -> std::string
    {
        using namespace params;
        return Host_formatter::to_string(Value_helper::plain_to_host(static_cast<double>(value), _semantics), _semantics);
    }

    params::Semantics::Any _semantics;

};

}
