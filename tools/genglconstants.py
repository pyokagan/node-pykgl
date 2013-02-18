#! /usr/bin/python3
"""
Generates binding for OpenGL constants.
Usage (from base working directory):
    ./tools/genglconstants.py > src/glconstants.cpp

"""
from argparse import ArgumentParser
from subprocess import check_output
import re
from itertools import chain
import sys

def get_constants(f):
    out = []
    x = check_output(("gcc", "-E", "-dM", f)).decode("utf-8").split("\n")
    for y in x:
        match = re.match(r"#define GL_([A-Za-z0-9_]*)", y)
        if match:
            out.append(match.group(1))
    return out

def main():
    p = ArgumentParser()
    p.add_argument("headers", nargs="*", default=["/usr/include/GL/glew.h"])
    args = p.parse_args()
    print("""#include <node.h>
#include <GL/glew.h>
#include <GL/gl.h>

void InitGLConstants(v8::Handle<v8::ObjectTemplate> target) {
v8::HandleScope scope;""")
    z = set(chain.from_iterable(get_constants(f) for f in args.headers))
    for x in z:
        print('target->Set(v8::String::NewSymbol("', x, '"), v8::Integer::New(GL_', x, '));', sep='')
    print("}")
    print(len(z), "constants", file=sys.stderr)

if __name__ == "__main__":
    main()
