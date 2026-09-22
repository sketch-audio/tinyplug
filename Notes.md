# Notes

Ryan's notes for the next rename/refactor pass.

## General
- Switch to using std-qualified int types instead of C-style typedefs (ex. std::uint32_t).

## Weigh and report
- Add `Bytes` to edit::State::Tag. For AAX encode to base64 and store as string in chunk. Confirm AAX SDK does not enforce a maximum string size for chunk.
- Feasibility of storing parameter values in 'format native' types. Likely needs old code paths for backwards compatibility.
    - AAX, CLAP, VST3: double
    - AUv2, AUv3: float
- Benefit/feasibility of changing State_rules::no_value to something better. Also likely needs old code paths for backwards compatibility.
- Review of format state load/save and recommendations for "v2" state system. Include buffer-system plan.

## Headers
- tiny_processor.hpp
    - Rename to tiny_process.hpp (matching namespace)
    - Event::Set, value first, fix call sites.
    - Event::Ramp, rename target to value, make first, fix call sites.
    - Event::Ramp, rename dur_samples to samples, fix call sites.
    - Rename Tagged_event to Event::Ordered
    ```c++
    struct Event {
        // ...
        struct Ordered {
            Any event{};
            std::int32_t offset{/*std::numeric_limits...*/};
        };
    }
    ```
    - Possible to move `frames_to_beats` elsewhere?
    - Consolidate Musical_context and related types. Use std::optional correctly. Fix call sites.
    ```c++
    struct Timeline {
        template<typename T>
        using Optional = std::optional<T>;

        struct State {
            Optional<bool> moving{};
            Optional<bool> cycling{};
            Optional<bool> recording{};
        };

        std::int64_t sample_pos{}; // Framework wrappers must guarantee!
        Optional<double> beat_pos{};
        Optional<double> cycle_start{};
        Optional<double> cycle_end{};
        Optional<double> tempo_ideal{120.};
        Optional<double> tempo_real{120.};
        Optional<std::int32_t> sig_numer{4};
        Optional<std::int32_t> sig_denom{4};
        State state{};
    };
    ```
    - Rename Render_mode to Mode, use std::uint32_t as underlying type. Fix call sites.
    - Rename Dsp_context to Context, note new field names, update call sites.
    ```c++
    struct Context {
        Timeline timeline{};
        Mode mode{};
        std::span<const float*> inputs{};
        std::span<const float*> sidechain{};
        std::span<float*> outputs{};
        std::size_t num_frames{};
        std::span<float> meters{};
        std::optional<std::uint32_t> propose_latency{};
    };
    ```
    - Rename Some_plug_processor to Interface, update call sites.
    ```c++
    template<typename T>
    concept Interface = requires(T t) {
        requires std::default_initializable<T>;
        { t.configure(std::declval<const Config&>()) } -> std::same_as<void>;
        { t.reset(std::declval<const Reset::Any&>()) } -> std::same_as<void>;
        { t.handle(std::declval<const Event::Any&>()) } -> std::same_as<void>;
        { t.process(std::declval<Context&>()) } -> std::same_as<void>;
        { t.latency() } -> std::same_as<std::uint32_t>;
        { t.tail() } -> std::same_as<std::uint32_t>;
    };
    ```
- tiny_events.hpp
    - Move all types into tiny_edit.hpp, delete header, update call sites.
    - Should be no longer necessary to include in change_list.hpp, tiny_processor.hpp.
- state_adapter.hpp
    - Move `State_tag`, etc. into tiny_edit.hpp
- tiny_edit.hpp
    - Introduce namespace tiny::edit, all header types in this namespace, no using shims, fix call sites, get building.
    - Introduce edit::Action, update call sites.
    ```c++
    struct Action {
        struct Start {} // Was Action_start
        struct Set {} // Was Set_param
        struct End {} // Was Action_end
        struct Resize {} // Was Request_resize
        using Any = std::variant<...>
    };
    ```
    - Renames:
        - Ui_receiver to Connection
            - Remove nested usings, std::function<...> at member sites.
        - Edit_context to Context
    - Introduce nested structure for `State_x` types. Update call sites.
    ```c++
    struct State {
        enum class Tag : std::uint32_t {...}
        using Item = std::variant<...>
        using Map = ...
    }
    ```
    - Introduce Interface for client editor type.





