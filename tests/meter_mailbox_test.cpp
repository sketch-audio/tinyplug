// Scratch validation of Publisher + Mailbox end to end. Not a shipped test.
#include <tiny_core/meter_publisher.hpp>
#include <tiny_core/meter_mailbox.hpp>
#include <cstdio>
#include <string>
#include <vector>
#include <thread>

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

using Infos_t = Infos<Test_meters>;
using Pub = Publisher<Infos_t>;
using Box = Mailbox<Infos_t>;

constexpr auto LEVEL = 0u, PEAK = 1u, TRIG = 2u;

static int failures = 0;
static void check(bool ok, const std::string& what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

// One process block: DSP writes the scratch, publisher posts into the mailbox.
//
// `blocks_per_draw` is load-bearing, not a knob. Most checks here drive one block per
// draw, which is the one ratio at which a post can never share a read interval with
// another — and so the one ratio at which coalescing bugs are invisible. Every host
// runs at 5-20, and AAX forwards a whole 30 ms burst at once.
struct Rig {
    Pub pub; Box box;
    std::array<Sample, 3> out{};
    bool wire = true;   // false = transport refuses (VST3 null queue / full AAX ring)
    int blocks_per_draw = 1;
    void frame(float level, float peak, float trig) {
        for (auto b = 0; b < blocks_per_draw; ++b) block(level, peak, trig);
        draw();
    }
    void block(float level, float peak, float trig) {
        if (level >= 0) pub.scratch()[LEVEL] = level;
        if (peak  >= 0) pub.scratch()[PEAK]  = peak;
        if (trig  >  0) pub.scratch()[TRIG]  = trig;
        pub.publish(false, [this](uint32_t a, float v) {
            if (!wire) return false;
            box.post(a, v); return true;
        });
    }
    void draw() { box.read(out); }
};

int main() {
    // 1. A level survives blocks that never write it.
    {
        Rig r;
        r.block(48000.f, -1, 0); r.draw();
        check(r.out[LEVEL].value == 48000.f, "level: delivered");
        for (int i = 0; i < 100; ++i) r.block(-1, -1, 0);   // process skipped
        r.draw();
        check(r.out[LEVEL].value == 48000.f, "level: held across 100 skipped blocks");
    }
    // 2. A steady peak keeps reading, frame after frame. (Dedup here would go dark.)
    {
        Rig r;
        for (int frame = 0; frame < 5; ++frame) {
            r.block(-1, 0.5f, 0);
            r.draw();
            if (r.out[PEAK].value != 0.5f) { check(false, "peak: steady signal keeps reading"); return 1; }
        }
        check(true, "peak: steady signal keeps reading (not deduped into silence)");
    }
    // 3. Peak is the max since the last look, over any interval.
    {
        Rig r;
        for (int i = 0; i < 2000; ++i) r.block(-1, (i == 900) ? 0.97f : 0.02f, 0);
        r.draw();
        check(r.out[PEAK].value == 0.97f, "peak: max over 2000 unread blocks survives");
        r.block(-1, 0.f, 0); r.draw();
        check(r.out[PEAK].value == 0.f, "peak: cleared by the read, then decays");
    }
    // 4. No editor for a long time, then one attaches: the level is simply there.
    {
        Rig r;
        r.block(44100.f, -1, 0);
        for (int i = 0; i < 5000; ++i) r.block(-1, 0.3f, 0);  // nobody reading
        r.draw();  // first frame of a freshly attached window
        check(r.out[LEVEL].value == 44100.f, "attach: constant present with no resync at all");
    }
    // 5. Triggers are counted, never coalesced, never phantom.
    {
        Rig r;
        r.block(-1, -1, 3.f);
        r.draw();
        check(r.out[TRIG].triggers == 1 && r.out[TRIG].value == 3.f, "trig: one event, magnitude carried");
        r.block(-1, -1, 0);          // trailing zero from the reset
        r.draw();
        check(r.out[TRIG].triggers == 0, "trig: trailing zero is not a phantom trigger");
        r.block(-1, -1, 4.f); r.block(-1, -1, 4.f); r.block(-1, -1, 4.f);
        r.draw();
        check(r.out[TRIG].triggers == 3, "trig: repeats counted, not deduped or coalesced");
    }
    // 6. Triggers accumulate while unread rather than being dropped.
    {
        Rig r;
        for (int i = 0; i < 40; ++i) { r.block(-1, -1, 1.f); r.block(-1, -1, 0); }
        r.draw();
        check(r.out[TRIG].triggers == 40, "trig: 40 events across an unread stretch all counted");
    }
    // 7. discard() drops the backlog but keeps levels.
    {
        Rig r;
        r.block(96000.f, 0.8f, 2.f);
        r.box.discard();
        r.draw();
        check(r.out[LEVEL].value == 96000.f, "discard: level kept");
        check(r.out[PEAK].value == 0.f,      "discard: stale peak dropped");
        check(r.out[TRIG].triggers == 0,     "discard: trigger backlog dropped");
    }
    // 8. A refused send is retried, not lost.
    {
        Rig r;
        r.wire = false;
        r.block(22050.f, 0.9f, 0);
        r.wire = true;
        r.block(22050.f, 0.f, 0);
        r.draw();
        check(r.out[LEVEL].value == 22050.f, "refused: level retried on the next block");
        check(r.out[PEAK].value == 0.9f,     "refused: peak held and delivered late");
    }

    // --- 9. Reader faster than delivery (VST3: host forwards at its own rate). ---
    {
        Rig r;
        auto zeros = 0;
        for (int frame = 0; frame < 20; ++frame) {
            // Steady signal, but the host only forwards on every other frame.
            if (frame % 2 == 0) r.block(-1, 0.6f, 0);
            r.draw();
            if (r.out[PEAK].value == 0.f) ++zeros;
        }
        check(zeros == 0, "peak: steady signal does not flicker when delivery is slower than the draw");
    }
    // --- 10. Genuine silence still falls to zero. ---
    {
        Rig r;
        r.block(-1, 0.6f, 0); r.draw();
        for (int frame = 0; frame < 5; ++frame) { r.block(-1, 0.f, 0); r.draw(); }
        check(r.out[PEAK].value == 0.f, "peak: real silence still decays to zero");
    }

    // --- 11. Concurrent: a posted maximum is never lost. --------------------
    // The reason `Slot` is one atomic rather than two. With a separate value and
    // count, a reader could exchange the value out, then load a count the producer
    // had not yet bumped, conclude nothing arrived, and discard the peak it had
    // just removed from the slot. Single-threaded tests cannot reach that window.
    {
        constexpr auto trials = 50;
        constexpr auto posts = 20000;
        auto lost = 0;

        for (auto t = 0; t < trials; ++t) {
            Box box;
            std::atomic<bool> done{false};
            auto highest_seen = 0.f;

            std::thread producer([&] {
                for (auto i = 0; i < posts; ++i) {
                    // One spike in the middle of a long quiet run.
                    box.post(PEAK, (i == posts / 2) ? 0.9f : 0.1f);
                }
                done.store(true, std::memory_order_release);
            });

            std::array<Sample, 3> out{};
            while (!done.load(std::memory_order_acquire)) {
                box.read(out);
                highest_seen = std::max(highest_seen, out[PEAK].value);
            }
            producer.join();
            box.read(out); // Final drain.
            highest_seen = std::max(highest_seen, out[PEAK].value);

            if (highest_seen < 0.9f) ++lost;
        }
        check(lost == 0, "concurrent: a posted peak is never dropped by the reader ("
                         + std::to_string(trials) + " trials)");
    }

    // --- 12. The falling edge survives coalescing. --------------------------
    // The publisher announces silence exactly once. At more than one block per draw
    // that announcement shares a read interval with the audio before it, and `max`
    // absorbs it — after which nothing is ever posted again and the meter holds a
    // level the signal no longer has, for as long as the session stays quiet.
    {
        for (const auto per_draw : {1, 2, 5, 20, 64}) {
            Rig r;
            r.blocks_per_draw = per_draw;
            for (auto f = 0; f < 3; ++f) r.frame(-1, 0.6f, 0);

            // Audio stops part-way through a frame, which is the general case.
            r.block(-1, 0.6f, 0);
            for (auto b = 1; b < per_draw; ++b) r.block(-1, 0.f, 0);
            r.draw();

            auto settled = -1;
            for (auto f = 0; f < 200 && settled < 0; ++f) {
                r.frame(-1, 0.f, 0);
                if (r.out[PEAK].value == 0.f) settled = f;
            }
            check(settled == 0, "falling edge: zero on the next draw at "
                  + std::to_string(per_draw) + " blocks/draw (settled=" + std::to_string(settled) + ")");
        }
    }

    // --- 13. A signal that restarts inside one interval is not silence. -----
    // `ended` describes the most recent post, so a stop-start that never reaches the
    // reader must not read as a stop.
    {
        Rig r;
        r.block(-1, 0.5f, 0); r.block(-1, 0.f, 0); r.block(-1, 0.8f, 0);
        r.draw();
        check(r.out[PEAK].value == 0.8f, "restart: a stop-start within one interval is not a falling edge");
        r.draw();
        check(r.out[PEAK].value == 0.8f, "restart: ... and the next draw still holds");
    }

    // --- 14. KNOWN LIMITATION: a producer that stops posting holds forever. -
    // Not a bug to fix here — a record of where the in-band mechanism ends, so that
    // changing it is deliberate. `ended` needs the producer to get one more block in
    // which to say zero; a host that stops calling `process` mid-signal never gives it
    // one, and absence is not observable from this side. If a host ever demonstrates
    // this, the fix is a hold timeout in `read` and this check inverts.
    // See "Falling edges" in plans/meter-pipeline.md.
    {
        Rig r;
        r.block(-1, 0.7f, 0); r.draw();
        for (auto f = 0; f < 100; ++f) r.draw();   // Producer gone. No posts at all.
        check(r.out[PEAK].value == 0.7f, "KNOWN LIMITATION: a producer that stops without "
                                         "reaching its falling edge holds its last peak");
    }
    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS", failures, failures==1?"":"s");
    return failures ? 1 : 0;
}
