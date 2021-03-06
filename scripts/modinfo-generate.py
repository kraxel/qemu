#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import sys

def print_array(name, values):
    if len(values) == 0:
        return
    list = ", ".join(values)
    print("    .%s = ((const char*[]){ %s, NULL })," % (name, list))

def parse_line(line):
    kind = ""
    data = ""
    get_kind = False
    get_data = False
    for item in line.split():
        if item == "MODINFO_START":
            get_kind = True
            continue
        if item.startswith("MODINFO_END"):
            get_data = False
            continue
        if get_kind:
            kind = item
            get_kind = False
            get_data = True
            continue
        if get_data:
            data += " " + item
            continue
    return (kind, data)

def generate(name, lines):
    arch = ""
    objs = []
    deps = []
    opts = []
    for line in lines:
        if line.find("MODINFO_START") != -1:
            (kind, data) = parse_line(line)
            if kind == 'obj':
                objs.append(data)
            elif kind == 'dep':
                deps.append(data)
            elif kind == 'opts':
                opts.append(data)
            elif kind == 'arch':
                arch = data;
            else:
                print("unknown:", kind)
                exit(1)

    print("    .name = \"%s\"," % name)
    if arch != "":
        print("    .arch = %s," % arch)
    print_array("objs", objs)
    print_array("deps", deps)
    print_array("opts", opts)
    print("},{");
    return deps

def print_pre():
    print("/* generated by scripts/modinfo-generate.py */")
    print("#include \"qemu/osdep.h\"")
    print("#include \"qemu/module.h\"")
    print("const QemuModinfo qemu_modinfo[] = {{")

def print_post():
    print("    /* end of list */")
    print("}};")

def main(args):
    deps = {}
    print_pre()
    for modinfo in args:
        with open(modinfo) as f:
            lines = f.readlines()
        print("    /* %s */" % modinfo)
        (basename, ext) = os.path.splitext(modinfo)
        deps[basename] = generate(basename, lines)
    print_post()

    flattened_deps = {flat.strip('" ') for dep in deps.values() for flat in dep}
    error = False
    for dep in flattened_deps:
        if dep not in deps.keys():
            print("Dependency {} cannot be satisfied".format(dep),
                  file=sys.stderr)
            error = True

    if error:
        exit(1)

if __name__ == "__main__":
    main(sys.argv[1:])
