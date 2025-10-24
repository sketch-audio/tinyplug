#pragma once

#include <cstring>
#include <unordered_map>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

#include <AudioUnitSDK/AUScopeElement.h>

#include "tinyplug/tinyplug.h"

#include "models/param_model.h"

namespace tiny {

enum class Message_type : uint32_t { latency_changed = 0 };
struct Private_message {
    Message_type type{};
};
struct Main_executor {
    using On_main = std::function<void(void)>;
    On_main on_main = [](){};
};

inline auto cf_to_std(CFStringRef cfStr) -> std::string
{
    if (!cfStr) return {};

    CFIndex length = CFStringGetLength(cfStr);
    CFIndex maxSize = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;

    auto result = std::string(static_cast<size_t>(maxSize), '\0');

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

inline auto tree_to_clump_map(const Param_node& root) -> Clump_map
{
    auto result = Clump_map{};
    auto clump_ids = std::unordered_map<std::string, int32_t>{};
    auto next_id = int32_t{1}; // 0 reserved for system per AU docs.

    const auto visit = [&](const Param_node& node, const std::string& path, const auto& self) -> void {
        std::visit(
            Inline_visitor{
                [&](const Param_spec& spec) {
                    const auto clump_id = [&]() {
                        const auto [it, inserted] = clump_ids.try_emplace(path, next_id);
                        if (inserted) ++next_id;
                        return it->second;
                    }();
                    result[spec.address] = {clump_id, path};
                },
                [&](const Param_group& group) {
                    const auto new_path = path.empty() ? std::string{group.name} : path + "/" + group.name;

                    for (const auto& child : group.nodes) {
                        self(child, new_path, self);
                    }
                }
            }
        , node);
    };

    visit(root, "", visit);
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