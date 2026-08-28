// Scratch validation of meters::Publisher semantics. Not a shipped test.
#include <tinyplug/meter_publisher.hpp>
#include <cstdio>
#include <vector>
#include <string>

using namespace tiny;
using namespace tiny::meters;

struct Test_meters {
    enum class Address : uint32_t { level, peak, trig, Num_meters };
    static auto make_spec(Address a) -> Spec {
        switch (a) {
            case Address::level: return {.range={0,100}, .policy=Policy::Stream};
            case Address::peak:  return {.range={0,1},   .policy=Policy::Peak};
            case Address::trig:  return {.range={0,1},   .policy=Policy::Trig};
            default: return {};
        }
    }
};
static_assert(meters::Model<Test_meters>);

using Pub = Publisher<Infos<Test_meters>>;

static int failures = 0;
static void check(bool ok, const std::string& what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

struct Wire {
    std::vector<std::pair<uint32_t,float>> sent;
    bool accept = true;
    auto fn() { return [this](uint32_t a, float v) { if (accept) sent.push_back({a,v}); return accept; }; }
    auto count(uint32_t a) const { int n=0; for (auto& [x,_]:sent) if (x==a) ++n; return n; }
    auto last(uint32_t a) const { float v=-1; for (auto& [x,y]:sent) if (x==a) v=y; return v; }
    void clear() { sent.clear(); }
};

constexpr auto LEVEL = 0u, PEAK = 1u, TRIG = 2u;

int main() {
    // --- 1. A Stream meter survives a block that does not write it. -----------
    {
        Pub p; Wire w;
        p.scratch()[LEVEL] = 48000.f;
        p.publish(false, w.fn());
        check(w.last(LEVEL) == 48000.f, "stream: first value transmitted");
        w.clear();
        // Block where process() did not run: nothing writes the scratch.
        p.publish(false, w.fn());
        check(w.count(LEVEL) == 0, "stream: unchanged value not retransmitted");
        check(p.scratch()[LEVEL] == 48000.f, "stream: value PERSISTS across a skipped block");
    }
    // --- 2. Peak resets each block. -------------------------------------------
    {
        Pub p; Wire w;
        p.scratch()[PEAK] = 0.9f;
        p.publish(false, w.fn());
        check(w.last(PEAK) == 0.9f, "peak: transmitted");
        check(p.scratch()[PEAK] == 0.f, "peak: scratch reset for next block");
        w.clear();
        p.publish(false, w.fn()); // silent block
        check(w.last(PEAK) == 0.f, "peak: silent block reports 0");
    }
    // --- 3. Peak maxima held across a full transport. --------------------------
    {
        Pub p; Wire w;
        p.scratch()[PEAK] = 0.2f; p.publish(false, w.fn()); w.clear();
        w.accept = false;                        // transport full (editor stalled)
        p.scratch()[PEAK] = 0.95f; p.publish(false, w.fn());
        p.scratch()[PEAK] = 0.30f; p.publish(false, w.fn());
        w.accept = true;
        p.scratch()[PEAK] = 0.10f; p.publish(false, w.fn());
        check(w.last(PEAK) == 0.95f, "peak: max held across failed sends, not lost");
    }
    // --- 4. Shadow does not advance on a failed send. --------------------------
    {
        Pub p; Wire w;
        w.accept = false;
        p.scratch()[LEVEL] = 96000.f; p.publish(false, w.fn());
        w.accept = true;
        p.scratch()[LEVEL] = 96000.f; p.publish(false, w.fn());
        check(w.last(LEVEL) == 96000.f, "stream: dropped send is retried, not lost forever");
    }
    // --- 6. Trig: one event per trigger, no phantom from the reset. ------------
    {
        Pub p; Wire w;
        p.scratch()[TRIG] = 1.f;
        p.publish(false, w.fn());
        check(w.count(TRIG) == 1, "trig: fires once");
        w.clear();
        p.publish(false, w.fn());   // reset left it at 0
        check(w.count(TRIG) == 0, "trig: trailing zero is NOT a second trigger");
    }
    // --- 7. Trig: consecutive identical triggers both fire. --------------------
    {
        Pub p; Wire w;
        p.scratch()[TRIG] = 1.f; p.publish(false, w.fn());
        p.scratch()[TRIG] = 1.f; p.publish(false, w.fn());
        check(w.count(TRIG) == 2, "trig: repeat trigger not swallowed by dedup");
    }
    // --- 9. Offline suppresses sends but still resets. -------------------------
    {
        Pub p; Wire w;
        p.scratch()[PEAK] = 0.5f; p.publish(true, w.fn());
        check(w.sent.empty(), "offline: nothing transmitted");
        check(p.scratch()[PEAK] == 0.f, "offline: peak still reset (no spike hoarding)");
        p.scratch()[PEAK] = 0.4f; p.publish(true, w.fn());
        p.scratch()[PEAK] = 0.1f; p.publish(false, w.fn());
        check(w.last(PEAK) == 0.1f, "offline: first realtime block reports current, not hoarded");
    }
    // --- 10. Peak can fall back to a lower value. ------------------------------
    {
        Pub p; Wire w;
        p.scratch()[PEAK] = 0.8f; p.publish(false, w.fn());
        p.scratch()[PEAK] = 0.2f; p.publish(false, w.fn());
        check(w.last(PEAK) == 0.2f, "peak: decays, does not stick at the maximum");
    }

    // --- 11. Editor closed a long time: peaks must not be lost. --------------
    {
        Pub p; Wire w;
        p.scratch()[PEAK] = 0.1f; p.publish(false, w.fn()); w.clear();
        w.accept = false;                          // editor closed, queue full
        for (int i = 0; i < 500; ++i) {
            p.scratch()[PEAK] = (i == 250) ? 0.97f : 0.05f;
            p.publish(false, w.fn());
        }
        w.accept = true;                           // editor reopens, queue drained
        p.scratch()[PEAK] = 0.02f; p.publish(false, w.fn());
        check(w.last(PEAK) == 0.97f, "closed-editor: peak over the whole interval survives");
    }
    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS", failures, failures==1?"":"s");
    return failures ? 1 : 0;
}
