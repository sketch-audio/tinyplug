from argparse import ArgumentParser
import os
import shutil
import sys

# Convert a name to pascal case. (Ex. "My Plug-in" -> "MyPlugin")
def to_pascal_case(name: str) -> str:
    name = name.replace("-", "")
    name = name.title().replace(" ", "")
    return name

# Convert a name to snake case. (Ex. "My Plug-in" -> "my_plugin")
def to_snake_case(name: str) -> str:
    name = name.replace("-", "")
    name = name.lower().replace(" ", "_")
    return name

# Generate a four-character string from a name.
def make_short_name(name: str) -> str:
    name = to_pascal_case(name)
    return name[:4]

def main():
    # arguments
    parser = ArgumentParser(
        prog="new_plugin.py",
        description="Create a new plug-in."
    )
    parser.add_argument("name", help="Display name of the plug-in (spaces OK).")
    parser.add_argument("--manu", help="Four-character manufacturer code.", default="Tiny")
    parser.add_argument("--id", help="Four-character plug-in identifier.", default="demo")
    parser.add_argument("--dest", help="Destination directory for the new plugin.", default=None)

    args = parser.parse_args()
    print("-- Parsing args.")

    # check that args.manu, args.id are four-character codes
    if len(args.manu) != 4:
        print("Error: Manufacturer code must be four characters.")
        sys.exit(1)
    if len(args.id) != 4:
        print("Error: Plug-in identifier must be four characters.")
        sys.exit(1)

    # generate derived names
    display_name = args.name
    dir_name = to_snake_case(display_name)
    file_name = to_pascal_case(display_name)
    short_name = make_short_name(display_name)

    manufacturer_code = args.manu
    plugin_code = args.id

    # get current directory
    here = os.path.dirname(__file__)

    # try to create a new directory
    if args.dest:
        dest_dir = os.path.join(args.dest, dir_name)
    else:
        dest_dir = os.path.join(here, "plugins", dir_name)

    print(f"-- Creating directory '{dir_name}' in '{dest_dir}'.")
    try:
        os.makedirs(dest_dir)
    except FileExistsError:
        print(f"Error: Directory '{dir_name}' already exists.")
        sys.exit(1)

    # format the cmake
    print("-- Formatting CMakeLists.txt.")
    cmake_template = os.path.join(here, "template", "CMakeLists.txt.in")
    with open(cmake_template, "r") as f:
        content = f.read()

    content = content.replace("{display_name}", display_name)\
                     .replace("{file_name}", file_name)\
                     .replace("{short_name}", short_name)\
                     .replace("{manu}", manufacturer_code)\
                     .replace("{id}", plugin_code)

    cmake_target = os.path.join(dest_dir, "CMakeLists.txt")
    with open(cmake_target, "w") as f:
        f.write(content)

    # create the source directory
    source_dir = os.path.join(dest_dir, "source")
    os.makedirs(source_dir)

    # copy files from ./template/source to `source_dir`
    print("-- Copying source files.")
    template_sources = os.path.join(here, "template", "source")
    for item in os.listdir(template_sources):
        s = os.path.join(template_sources, item)
        d = os.path.join(source_dir, item)
        if os.path.isdir(s):
            shutil.copytree(s, d)
        else:
            shutil.copy2(s, d)

    # If no destination, add to test plugins build.
    if args.dest == None:
        print("-- Adding to build.")
        build_file = os.path.join(here, "plugins", "CMakeLists.txt")
        # append 'add_subdirectory'
        with open(build_file, "a") as f:
            f.write(f"add_subdirectory({dir_name})\n")

        # sort lines alphabetically
        with open(build_file, "r") as f:
            lines = f.readlines()
        lines.sort()
        with open(build_file, "w") as f:
            f.writelines(lines)

    print("-- Success!")

if __name__ == "__main__":
    main()