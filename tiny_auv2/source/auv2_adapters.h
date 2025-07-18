#pragma once

#include <cstring>
#include <unordered_map>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include "tinyplug/tinyplug.h"
#include "user_plug.h"

namespace tiny::auv2 {

inline auto cf_to_std(CFStringRef cfStr) -> std::string
{
    if (!cfStr) return {};

    CFIndex length = CFStringGetLength(cfStr);
    CFIndex maxSize = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;

    auto result = std::string(maxSize, '\0');

    if (CFStringGetCString(cfStr, result.data(), maxSize, kCFStringEncodingUTF8)) {
        result.resize(std::strlen(result.c_str())); // Trim to actual size
        return result;
    }

    return {};
}

#pragma mark - CFString and CString Utilities

static inline CFStringRef MakeCFString(const char* cStr)
{
  return CFStringCreateWithCString(0, cStr, kCFStringEncodingUTF8);
}

class CFStrLocal {
public:
  CFStrLocal(const char* cStr)
  {
    mCFStr = MakeCFString(cStr);
  }
    
  ~CFStrLocal()
  {
    CFRelease(mCFStr);
  }
    
  CFStrLocal(const CFStrLocal&) = delete;
  CFStrLocal& operator=(const CFStrLocal&) = delete;
    
  CFStringRef Get() { return mCFStr; }
    
private:
  CFStringRef mCFStr;
};

struct Clump {
    int32_t id{};
    std::string name{};
};

using Clump_map = std::unordered_map<uint32_t, Clump>; // Param id : Clump

template <typename Id>
inline auto build_clump_map(const Param_node<Id>& root) -> Clump_map
{
    auto result = Clump_map{};
    auto clump_ids = std::unordered_map<std::string, int32_t>{};
    auto next_id = int32_t{1}; // 0 reserved for system per AU docs.

    const auto visit = [&](const auto& node, const std::string& path, const auto& self) -> void {
        std::visit([&](const auto& item) {
            using T = std::decay_t<decltype(item)>;

            if constexpr (std::is_same_v<T, Param_spec<Id>>) {
                const auto clump_path = path;
                const auto clump_id = [&]() {
                    const auto [it, inserted] = clump_ids.try_emplace(clump_path, next_id);
                    if (inserted) ++next_id;
                    return it->second;
                }();
                result[utils::to_underlying(item.id)] = {clump_id, clump_path};
            }
            else if constexpr (std::is_same_v<T, Param_group<Id>>) {
                const auto new_path = path.empty() ? std::string{item.name} : path + "/" + item.name;
                for (const auto& child : item.nodes) {
                    self(child, new_path, self);
                }
            }
        }, node);
    };

    visit(root, "", visit);
    return result;
}

template <typename Id>
inline auto flatten_tree_ids(const Param_node<Id>& root) -> std::vector<std::underlying_type_t<Id>>
{
    using Underlying = std::underlying_type_t<Id>;
    auto result = std::vector<Underlying>{};

    const auto visit = [&](const auto& node, const auto& self) -> void {
        std::visit([&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, Param_spec<Id>>) {
                result.push_back(static_cast<Underlying>(item.id));
            } else if constexpr (std::is_same_v<T, Param_group<Id>>) {
                for (const auto& child : item.nodes) {
                    self(child, self);
                }
            }
        }, node);
    };

    visit(root, visit);
    return result;
}

inline auto find_clump(const Clump_map& map, int32_t clump_id) -> const Clump*
{
    for (const auto& [param_id, clump] : map) {
        if (clump.id == clump_id)
            return &clump;
    }
    return nullptr;
}

inline auto find_clump_for_parameter(const Clump_map& clump_map, uint32_t param_id) -> const Clump*
{
    if (const auto it = clump_map.find(param_id); it != clump_map.end()) {
        const auto& [clump_id, clump_name] = it->second;
        if (!clump_name.empty())
            return &it->second;
    }
    return nullptr;
}

} 