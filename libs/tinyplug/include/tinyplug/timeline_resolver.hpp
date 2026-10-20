#pragma once

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "tiny_processor.hpp"

namespace tiny::process {

// Turns what a wrapper read from the host into a `Timeline`: invalid values become empty, and `sample_pos` is guaranteed.
struct Timeline_resolver {
    // Once per host buffer, at its first frame. `t.sample_pos` is ignored; the host's goes in `host_pos`.
    auto resolve(Timeline t, std::optional<std::int64_t> host_pos, std::size_t num_frames, double sr) -> Timeline
    {
        const auto finite = [](const std::optional<double>& x) { return x && std::isfinite(*x); };

        if (!finite(t.beat_pos)) t.beat_pos.reset();
        if (!finite(t.tempo) || *t.tempo <= 0) t.tempo.reset();
        if (t.signature && (t.signature->numer <= 0 || t.signature->denom <= 0)) t.signature.reset();
        if (t.cycle && !(std::isfinite(t.cycle->start) && std::isfinite(t.cycle->end))) t.cycle.reset();
        if (!t.cycle && t.state) t.state->cycling = false;

        // Position: the host's, else from its beats and tempo, else free-running (and not moving).
        if (host_pos) {
            t.sample_pos = *host_pos;
        }
        else if (t.beat_pos && t.tempo) {
            t.sample_pos = std::llround(*t.beat_pos * 60 * sr / *t.tempo);
        }
        else {
            t.sample_pos = _free_pos;
            if (t.state) t.state->moving = false;
        }
        _free_pos = t.sample_pos + static_cast<std::int64_t>(num_frames);
        return t;
    }

    // A host position reported in samples as a double; non-finite is missing.
    static auto samples(double x) -> std::optional<std::int64_t>
    {
        if (!std::isfinite(x)) return std::nullopt;
        return static_cast<std::int64_t>(std::llround(x));
    }

    // The same buffer's timeline `offset` frames in, for a sub-block. Beats without a tempo can't advance, so they drop.
    static auto at_offset(Timeline t, std::size_t offset, double sr) -> Timeline
    {
        assert(sr > 0 && "Sample rate must be greater than zero.");
        t.sample_pos += static_cast<std::int64_t>(offset);
        if (t.beat_pos && t.tempo) *t.beat_pos += static_cast<double>(offset) * *t.tempo / (60 * sr);
        else if (offset > 0) t.beat_pos.reset();
        return t;
    }

    auto clear() -> void { _free_pos = 0; }

private:
    std::int64_t _free_pos{};
};

} // namespace tiny::process
