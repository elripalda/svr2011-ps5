#!/usr/bin/env python3
"""[ps5 port] Make generated/rexglue.cmake usable in a PS5 cross build.

rexglue codegen writes generated/rexglue.cmake with a build step that runs the
codegen tool. In a cross build that tool would be built for the PS5 and can't
run on the build machine, so the step is skipped when CMAKE_CROSSCOMPILING:
run codegen on the host first (it generates the same C++ for every platform).
Idempotent: run it after every codegen.
"""
import pathlib, sys

path = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "generated/rexglue.cmake")
s = path.read_text()
if "[ps5 port]" in s:
    print(f"{path}: already patched")
    sys.exit(0)

def rep(old, new):
    global s
    if old not in s:
        sys.exit(f"{path}: expected text not found: {old[:60]!r}")
    s = s.replace(old, new)

rep("        add_dependencies(${target_name}_recomp svr2011_codegen)\n",
    "        if(NOT CMAKE_CROSSCOMPILING)  # [ps5 port]\n"
    "            add_dependencies(${target_name}_recomp svr2011_codegen)\n"
    "        endif()\n")
rep("    add_dependencies(${target_name} svr2011_codegen)\n",
    "    if(NOT CMAKE_CROSSCOMPILING)  # [ps5 port]\n"
    "        add_dependencies(${target_name} svr2011_codegen)\n"
    "    endif()\n")
rep("add_custom_command(\n    OUTPUT \"${CMAKE_CURRENT_SOURCE_DIR}/generated/default/codegen.build.stamp\"",
    "if(CMAKE_CROSSCOMPILING)\n"
    "    # [ps5 port] the codegen tool would be a PS5 program: run codegen on the host.\n"
    "    add_custom_target(svr2011_codegen)\n"
    "else()\n"
    "add_custom_command(\n    OUTPUT \"${CMAKE_CURRENT_SOURCE_DIR}/generated/default/codegen.build.stamp\"")
rep("add_custom_target(svr2011_codegen\n    DEPENDS \"${CMAKE_CURRENT_SOURCE_DIR}/generated/default/codegen.build.stamp\")\n",
    "add_custom_target(svr2011_codegen\n    DEPENDS \"${CMAKE_CURRENT_SOURCE_DIR}/generated/default/codegen.build.stamp\")\n"
    "endif()\n")
path.write_text(s)
print(f"{path}: patched for cross builds")
