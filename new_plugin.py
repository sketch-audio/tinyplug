import sys
import os
import shutil

def main():
    if len(sys.argv) != 2:
        print("Usage: python new_plugin.py <plugin_name>")
        sys.exit(1)

    plugin_name = sys.argv[1]
    root_dir = os.path.dirname(__file__)
    plugins_dir = os.path.join(root_dir, "plugins")
    template_dir = os.path.join(root_dir, "template")

    cmake_template = os.path.join(template_dir, "CMakeLists.txt.in")

    target_dir = os.path.join(plugins_dir, plugin_name)
    target_cmake = os.path.join(target_dir, "CMakeLists.txt")

    cmake_file = os.path.join(plugins_dir, "CMakeLists.txt")

    # try create target directory
    try:
        os.makedirs(target_dir)
    except FileExistsError:
        print(f"Error: Directory '{target_dir}' already exists.")
        sys.exit(1)

    # copy files at template/source to plugins/<plugin_name>/source
    source_dir = os.path.join(template_dir, "source")
    try:
        shutil.copytree(source_dir, os.path.join(target_dir, "source"))
        print(f"Copied source files from {source_dir} to {os.path.join(target_dir, 'source')}")
    except FileExistsError:
        print(f"Error: Directory '{os.path.join(target_dir, 'source')}' already exists.")
        sys.exit(1)

    # format CMakeLists.txt.in and write to target CMakeLists.txt
    with open(cmake_template, "r") as f:
        content = f.read()
    content = content.replace("{plugin_name}", plugin_name)
    with open(target_cmake, "w") as f:
        f.write(content)

    # Read existing lines, add new line, sort, and write back
    new_line = f"add_subdirectory({plugin_name})\n"
    lines = []
    if os.path.exists(cmake_file):
        with open(cmake_file, "r") as f:
            lines = f.readlines()
    if new_line not in lines:
        lines.append(new_line)
    # Remove duplicates and sort
    lines = sorted(set(line.strip() for line in lines if line.strip()))
    with open(cmake_file, "w") as f:
        for line in lines:
            f.write(line + "\n")
    print(f"Updated {cmake_file} with add_subdirectory({plugin_name})")

if __name__ == "__main__":
    main()