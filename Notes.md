# Notes

Ryan's notes for the next rename/refactor pass. Done and superseded items are in git history (the 0.5 renames:
MIGRATION.md, "Edit vocabulary").

## Ongoing
- Switch to std-qualified int types instead of C-style typedefs (ex. std::uint32_t), as files are touched.

## Deferred to 1.0: one review of the whole save and load system
- Parameter values in format-native types (AAX, CLAP, VST3: double; AUv2, AUv3: float). Needs the old paths kept for
  sessions saved as floats.
- Something better than `State_rules::no_value`, with the same backward-compatibility constraint.
- Format state load/save as a "v2", the buffer system ([plans/buffer-system.md](plans/buffer-system.md)) included.
