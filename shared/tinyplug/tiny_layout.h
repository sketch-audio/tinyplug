#pragma once

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <variant>
#include <vector>

#include "tiny_utils.h"

namespace tiny::layout {

struct Fill {};
struct Fixed { double length{}; };
struct Relative { double amount{}; };
struct Defer {};

enum class Axis { x, y };

using Size_rule = std::variant<Fill, Fixed, Relative, Defer>;
enum class Alignment_rule { start, center, end };

struct Size { Size_rule w{}; Size_rule h{}; };
struct Alignment { Alignment_rule x{}; Alignment_rule y{}; };

struct Column; struct Row; struct Frame;
using View = std::variant<Column, Row, Frame>;

struct Column {
    Size size{};
    Alignment align{};
    std::vector<View> content{};
};

struct Row {
    Size size{};
    Alignment align{};
    std::vector<View> content{};
};

struct Frame {
    Size size{};
    Alignment align{};
};

struct Rect { double x{};  double y{}; double w{};  double h{}; size_t z{}; };

auto do_layout(const View& view, const Rect& in_rect, std::vector<Rect>& out_list) -> void;

namespace impl {

inline auto get_size_rule(const View& view, Axis axis) -> const Size_rule&
{
    return std::visit([=](auto&& v) -> const Size_rule& {
        return axis == Axis::x ? v.size.w : v.size.h;
    }, view);
}

inline auto count_fill_views(const std::vector<View>& views, Axis axis) -> size_t
{
    auto count = size_t{};
    for (const auto& view : views) {
        const auto& size_rule = get_size_rule(view, axis);

        std::visit(Inline_visitor{
            [&](const Fill&) { ++count; },
            [](auto&&) {}
        }, size_rule);
    }
    return count;
}

inline auto space_to_fill(const std::vector<View>& views, Axis axis, double available) -> double
{
    auto remaining = available;

    for (const auto& view : views) {
        const auto& size_rule = get_size_rule(view, axis);

        std::visit(Inline_visitor{
            [&](const Fixed& f) {
                remaining -= std::clamp(f.length, {}, available);
            },
            [&](const Relative& r) {
                remaining -= std::clamp(r.amount * available, {}, available);
            },
            [](auto&&) {}
        }, size_rule);
    }

    return std::max({}, remaining);
}

inline auto calc_dimension(const Size_rule& size_rule, double available) -> double
{
    return std::visit(Inline_visitor{
        [&](const Fill&) {
            return available;
        },
        [&](const Fixed& f) {
            return std::clamp(f.length, {}, available);
        },
        [&](const Relative& r) {
            return std::clamp(r.amount * available, {}, available);
        },
        [](auto&&) { return double{}; },
    }, size_rule);
}

inline auto calc_rect(const Size& size, const Alignment& align, const Rect& in_rect) -> Rect
{
    const auto w = calc_dimension(size.w, in_rect.w);
    const auto h = calc_dimension(size.h, in_rect.h);

    auto calc_off = [](Alignment_rule rule, double parent, double child) {
        using enum Alignment_rule;
        switch (rule) {
            case start: return double{};
            case center: return (parent - child) / 2;
            case end: return (parent - child);
        }
    };
    const auto x_off = calc_off(align.x, in_rect.w, w);
    const auto y_off = calc_off(align.y, in_rect.h, h);
    return {
        .x = in_rect.x + x_off,
        .y = in_rect.y + y_off,
        .w = w,
        .h = h,
        .z = in_rect.z
    };
}

inline auto layout_content_along_axis(const std::vector<View>& content, Axis axis, const Rect& own_rect, std::vector<Rect>& out_list) -> void
{
    const auto l_parent = axis == Axis::x ? own_rect.w : own_rect.h;
    const auto n_to_fill = count_fill_views(content, axis);
    const auto l_to_fill = space_to_fill(content, axis, l_parent);

    auto remaining = double{l_parent};
    auto advance = double{};
    auto working_rect = Rect{
        .x = own_rect.x,
        .y = own_rect.y,
        .w = axis == Axis::y ? own_rect.w : 0,
        .h = axis == Axis::x ? own_rect.h : 0,
        .z = own_rect.z + 1
    };
    auto* const working_d = axis == Axis::x ? &working_rect.x : &working_rect.y;
    auto* const working_l = axis == Axis::x ? &working_rect.w : &working_rect.h;
    for (const auto& child : content) {
        const auto size_rule = get_size_rule(child, axis);
        std::visit(Inline_visitor{
            [&](const Fill&) {
                advance = l_to_fill / n_to_fill;
                *working_l = advance;
            },
            [&](const Fixed& f) {
                advance = std::clamp(f.length, {}, remaining);
                *working_l = advance;
            },
            [&](const Relative& r) {
                advance = std::clamp(r.amount * l_parent, {}, remaining);
                *working_l = l_parent; // Calculate rect relative to parent.
            },
            [](auto&&) {},
        }, size_rule);
        do_layout(child, working_rect, out_list);
        remaining -= advance;
        *working_d += advance;
    }
}

} // namespace impl

inline auto do_layout(const View& view, const Rect& in_rect, std::vector<Rect>& out_list) -> void
{
    using namespace impl;
    std::visit(Inline_visitor{
        [&](const Column& c) {
            const auto own_rect = calc_rect(c.size, c.align, in_rect);
            out_list.push_back(own_rect);
            layout_content_along_axis(c.content, Axis::y, own_rect, out_list);
        },
        [&](const Row& r) {
            const auto own_rect = calc_rect(r.size, r.align, in_rect);
            out_list.push_back(own_rect);
            layout_content_along_axis(r.content, Axis::x, own_rect, out_list);
        },
        [&](const Frame& f) {
            const auto own_rect = calc_rect(f.size, f.align, in_rect);
            out_list.push_back(own_rect);
        },
    }, view);
}

inline auto print_rects(const std::vector<Rect>& rects) -> void
{
    std::cout << std::fixed << std::setprecision(1);

    std::cout << std::setw(5) << "Idx"
              << std::setw(8) << "X"
              << std::setw(8) << "Y"
              << std::setw(8) << "W"
              << std::setw(8) << "H"
              << std::setw(5) << "Z"
              << '\n';

    std::cout << std::string(42, '-') << '\n';

    for (size_t i = 0; i < rects.size(); ++i) {
        const auto& r = rects[i];
        std::cout << std::setw(5) << i
                  << std::setw(8) << r.x
                  << std::setw(8) << r.y
                  << std::setw(8) << r.w
                  << std::setw(8) << r.h
                  << std::setw(5) << r.z
                  << '\n';
    }
}

} // namespace tiny