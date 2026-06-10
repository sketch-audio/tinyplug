#pragma once

#include <utility>

namespace tiny {

template<typename Eff>
struct Stereo {

    Stereo() = default;

    template<typename... Args>
    Stereo(Args&&... args) : _l(std::forward<Args>(args)...), _r(std::forward<Args>(args)...) {}

    auto reset(double sr) -> void
    {
        _l.reset(sr);
        _r.reset(sr);
    }

    auto process(float x, bool left) -> float
    {
        return left ? _l.process(x) : _r.process(x);
    }

    auto latency_samps() -> uint32_t
    {
        assert(_l.latency_samps() == _r.latency_samps());
        return _l.latency_samps();
    }


private:
    
    Eff _l{};
    Eff _r{};
    
};

} // namespace tiny