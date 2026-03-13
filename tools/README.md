# AAX Page Table Generator

Generates `*Pages.xml` page table files for AAX plugins, required for control surface mapping (S6, S3, EUCON, etc.).

## Overview

Two steps:

1. **Build the manifest tool** — a small C++ executable (per plugin) that dumps the plugin's parameter list as JSON.
2. **Run the Python generator** — reads the JSON and writes the `*Pages.xml`.

---

## Step 1 — Add the manifest tool to your plugin's CMakeLists.txt

```cmake
include(${TINYPLUG_DIR}/presets/make_preset_exporter.cmake)
make_pagetable_manifest(${PLUGIN_TARGET})
```

This creates a build target named `<BaseFilename>_pagetable_manifest`. It requires no AAX or VST3 SDK — it just links against your plugin lib.

Also set the page table path so it gets copied into the AAX bundle:

```cmake
add_property(${PLUGIN_TARGET} TINY_AAX_PAGE_TABLE_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/MyPluginPages.xml")
```

---

## Step 2 — Generate the manifest JSON

Build and run the manifest tool:

```bash
cmake --build build-release --target MyPlugin_pagetable_manifest
./build-release/plugins/my_plugin/MyPlugin_pagetable_manifest > /tmp/MyPlugin_manifest.json
```

---

## Step 3 — Generate the XML

```bash
python3 tools/generate_pages.py /tmp/MyPlugin_manifest.json \
    plugins/my_plugin/assets/MyPluginPages.xml
```

The XML is committed to source control and only needs regenerating when params change.

---

## Notes

- Only `automation` policy params appear in the page tables. `control`, `hidden`, and `interface` params are excluded.
- Parameter IDs in the XML use hex addresses (e.g. `0x00000000`) matching the IDs registered in the AAX host.
- The `PgTL` table (one param per page) must list all automatable params — this is what the AAX validator checks with `test.page_table.automation_list`.
