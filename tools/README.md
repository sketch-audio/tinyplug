# tools

Author-facing helper utilities. Each is opt-in — the demo plug-ins don't depend
on any of them.

- **[new_plugin.py](new_plugin.py)** — scaffold a new plug-in (a simple gain
  effect, no worker) from [../template/](../template):

  ```bash
  python3 tools/new_plugin.py "My Plug" --manu Acme --id plg1
  ```

  Generates `examples/<snake_name>/` and appends it to
  [../examples/CMakeLists.txt](../examples/CMakeLists.txt).

- **[presets/](presets/)** — build a per-plugin executable that writes factory
  preset files from the plug-in's parameter defaults. `make_tfx_exporter`
  (AAX `.tfx`) and `make_vstpreset_exporter` (VST3 `.vstpreset`) in
  [presets/make_preset_exporters.cmake](presets/make_preset_exporters.cmake).

- **[pagetables/](pagetables/)** — generate AAX `*Pages.xml` page tables for
  control-surface mapping. A C++ manifest tool (`make_pagetable_manifest` in
  [pagetables/make_pagetable.cmake](pagetables/make_pagetable.cmake)) dumps the
  param list as JSON, then [pagetables/generate_pages.py](pagetables/generate_pages.py)
  turns it into XML. Full pipeline in
  [pagetables/README.md](pagetables/README.md).
