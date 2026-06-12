#!/usr/bin/env python3
"""Generate an AAX *Pages.xml page table from a pagetable_manifest JSON file.

Usage:
    python generate_pages.py <manifest.json> <output.xml>
"""

import json
import sys
from xml.etree import ElementTree as ET
from xml.dom import minidom

CATS = [0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048]


def aax_id(address: int) -> str:
    return f"0x{address:08x}"

def fourcc_str(code: str) -> str:
    """Return the code as-is (already a 4-char string from the manifest)."""
    return code


def indent_xml(elem: ET.Element, level: int = 0) -> None:
    """Add pretty-print indentation in-place."""
    pad = "\n" + "\t" * level
    pad_child = "\n" + "\t" * (level + 1)
    if len(elem):
        if not elem.text or not elem.text.strip():
            elem.text = pad_child
        if not elem.tail or not elem.tail.strip():
            elem.tail = pad
        for child in elem:
            indent_xml(child, level + 1)
        # last child tail
        if not child.tail or not child.tail.strip():
            child.tail = pad
    else:
        if level and (not elem.tail or not elem.tail.strip()):
            elem.tail = pad
    if not level:
        elem.tail = "\n"


# ---------------------------------------------------------------------------
# Layout definitions
# ---------------------------------------------------------------------------

# Grid layouts: (type, pgsz, rows, cols, has_first_pg)
GRID_LAYOUTS = [
    ("Av18", 24,  1, 8,  True),
    ("Av1F", 48,  1, 16, False),
    ("Av41", 12,  4, 1,  False),
    ("Av46", 72,  4, 6,  False),
    ("Av48", 96,  4, 8,  True),
    ("Av81", 24,  8, 1,  True),
]

# Linear layouts: (type, pgsz, has_first_pg, first_pg_value)
# first_pg_value: 0 for grid-style controls, 1 for timeline-style
LINEAR_LAYOUTS = [
    ("BkCS", 12, False, None),
    ("BkSF", 16, False, None),
    ("FrTL", 24, True,  1),
    ("HgTL", 8,  True,  1),
    ("MkTL", 8,  True,  1),
    ("PcTL", 16, True,  1),
]


def make_first_pg_elements(parent: ET.Element, value: int) -> None:
    for cat in CATS:
        fp = ET.SubElement(parent, "FirstPg", cat=str(cat))
        fp.text = str(value)


def make_grid_page_table(parent: ET.Element, layout_type: str, pgsz: int,
                         rows: int, cols: int, has_first_pg: bool,
                         params: list[dict]) -> None:
    """Emit a grid PageTable, splitting params across pages as needed."""
    per_page = rows * cols
    pt = ET.SubElement(parent, "PageTable", type=layout_type, pgsz=str(pgsz))

    # Chunk params into pages; always emit at least one page.
    chunks = [params[i:i + per_page] for i in range(0, max(len(params), 1), per_page)]
    if not chunks:
        chunks = [[]]

    for page_num, chunk in enumerate(chunks, start=1):
        page = ET.SubElement(pt, "Page", num=str(page_num))
        param_iter = iter(chunk)
        for row in range(1, rows + 1):
            for col in range(1, cols + 1):
                p = next(param_iter, None)
                knob = aax_id(p["address"]) if p else ""
                ET.SubElement(page, "Cell",
                               row=str(row), col=str(col),
                               knobID=knob,
                               inOutButtonID="",
                               selectButtonID="")

    if has_first_pg:
        make_first_pg_elements(pt, 0)

    # Trailing comment
    pt.append(ET.Comment(f"type='{layout_type}' pgsz='{pgsz}'"))


def make_linear_page_table(parent: ET.Element, layout_type: str, pgsz: int,
                            has_first_pg: bool, first_pg_value: int | None,
                            params: list[dict]) -> None:
    """Emit a linear PageTable (BkCS, BkSF, FrTL, HgTL, MkTL, PcTL)."""
    per_page = pgsz
    pt = ET.SubElement(parent, "PageTable", type=layout_type, pgsz=str(pgsz))

    chunks = [params[i:i + per_page] for i in range(0, max(len(params), 1), per_page)]
    if not chunks:
        chunks = [[]]

    for page_num, chunk in enumerate(chunks, start=1):
        page = ET.SubElement(pt, "Page", num=str(page_num))
        for p in chunk:
            id_el = ET.SubElement(page, "ID")
            id_el.text = aax_id(p["address"])
        # Pad remaining slots with empty IDs to fill the page
        for _ in range(per_page - len(chunk)):
            id_el = ET.SubElement(page, "ID")
            id_el.text = " "

    if has_first_pg and first_pg_value is not None:
        make_first_pg_elements(pt, first_pg_value)

    pt.append(ET.Comment(f"type='{layout_type}' pgsz='{pgsz}'"))


def make_pgtl_page_table(parent: ET.Element, params: list[dict]) -> None:
    """Emit the PgTL (1 param per page) table. Page 1 = MasterBypassID.

    The AAX spec requires PgTL to list parameters in registration order (DFS
    address order), matching the order params are added to the plug-in.
    """
    pt = ET.SubElement(parent, "PageTable", type="PgTL", pgsz="1")

    bypass_page = ET.SubElement(pt, "Page", num="1")
    ET.SubElement(bypass_page, "ID").text = "MasterBypassID"

    for i, p in enumerate(sorted(params, key=lambda p: p["address"]), start=2):
        page = ET.SubElement(pt, "Page", num=str(i))
        ET.SubElement(page, "ID").text = aax_id(p["address"])

    pt.append(ET.Comment("type='PgTL' pgsz='1'"))


def generate(manifest_path: str, output_path: str) -> None:
    with open(manifest_path, "r") as f:
        manifest = json.load(f)

    man_code = fourcc_str(manifest["manufacturer_code"])
    prod_code = fourcc_str(manifest["product_code"])
    plug_id_int = manifest["plugin_id"]
    # plugin_id = 0 → null FOURCC → empty string in XML
    plug_id_str = "" if plug_id_int == 0 else chr((plug_id_int >> 24) & 0xFF) + \
                                               chr((plug_id_int >> 16) & 0xFF) + \
                                               chr((plug_id_int >>  8) & 0xFF) + \
                                               chr( plug_id_int        & 0xFF)
    plugin_name = manifest["plugin_name"]
    company_name = manifest["company_name"]
    base_file_name = manifest["base_file_name"]

    all_params = manifest["params"]
    # Only automation params appear on control surfaces.
    visible_params = [p for p in all_params if p["policy"] == "automation"]

    root = ET.Element("PageTables", vers="6.4.0.95")

    # --- PageTableLayouts ---
    layouts_el = ET.SubElement(root, "PageTableLayouts")
    layout_name = "StandardLayout"

    plugin_el = ET.SubElement(layouts_el, "Plugin",
                               manID=man_code, prodID=prod_code, plugID=plug_id_str)
    desc = ET.SubElement(plugin_el, "Desc")
    desc.text = f"{plugin_name} {company_name}"
    ET.SubElement(plugin_el, "Layout").text = layout_name
    plugin_el.append(ET.Comment(f"manID='{man_code}' prodID='{prod_code}' plugID='{plug_id_str}'"))

    pt_layout = ET.SubElement(layouts_el, "PTLayout", name=layout_name)

    for layout_type, pgsz, rows, cols, has_fp in GRID_LAYOUTS:
        make_grid_page_table(pt_layout, layout_type, pgsz, rows, cols, has_fp, visible_params)

    for layout_type, pgsz, has_fp, fp_val in LINEAR_LAYOUTS:
        make_linear_page_table(pt_layout, layout_type, pgsz, has_fp, fp_val, visible_params)

    make_pgtl_page_table(pt_layout, visible_params)

    pt_layout.append(ET.Comment(f"name='{layout_name}'"))

    # --- ControlNamesVariations ---
    ctrl_names = ET.SubElement(root, "ControlNamesVariations")

    # MasterBypassID entry
    bypass_ctrl = ET.SubElement(ctrl_names, "Ctrl", ID="MasterBypassID")
    ET.SubElement(bypass_ctrl, "name", typ="PgTL", sz="3").text = "Byp"
    ET.SubElement(bypass_ctrl, "name", typ="PgTL", sz="6").text = "Bypass"
    bypass_ctrl.append(ET.Comment("ID='MasterBypassID'"))

    # One entry per visible param using hex address and short_name as label.
    for p in visible_params:
        param_id = aax_id(p["address"])
        short = p.get("short_name") or p["name"]
        ctrl = ET.SubElement(ctrl_names, "Ctrl", ID=param_id)
        label = short[:4] if len(short) > 4 else short
        ET.SubElement(ctrl, "name", typ="PgTL", sz=str(len(label))).text = label
        ctrl.append(ET.Comment(f"ID='{param_id}'"))

    # --- Editor ---
    editor = ET.SubElement(root, "Editor", vers="1.3.7.1")
    plugin_list = ET.SubElement(editor, "PluginList")
    rtas = ET.SubElement(plugin_list, "RTAS")
    plugin_id_el = ET.SubElement(rtas, "PluginID",
                                  manID=man_code, prodID=prod_code, plugID=plug_id_str)
    menu_str = ET.SubElement(plugin_id_el, "MenuStr")
    menu_str.text = f"AAX Native: {plugin_name}"
    plugin_id_el.append(ET.Comment(f"manID='{man_code}' prodID='{prod_code}' plugID='{plug_id_str}'"))

    disc_ctrls = ET.SubElement(editor, "DiscCtrls")
    ET.SubElement(disc_ctrls, "CtrlID").text = "MasterBypassID"

    editor.append(ET.Comment("vers='1.3.7.1'"))

    # --- Serialize ---
    indent_xml(root)
    tree = ET.ElementTree(root)
    ET.indent(tree, space="\t")  # Python 3.9+

    xml_str = ET.tostring(root, encoding="unicode", xml_declaration=False)
    output = f"<?xml version='1.0' encoding='US-ASCII' standalone='yes'?>\n{xml_str}\n"

    with open(output_path, "w", encoding="ascii", errors="replace") as f:
        f.write(output)

    print(f"Wrote {output_path} ({len(visible_params)} visible params)")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <manifest.json> <output.xml>", file=sys.stderr)
        sys.exit(1)
    generate(sys.argv[1], sys.argv[2])
