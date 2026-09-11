#!/usr/bin/env python3

# --interference-check JSON test
#
# Usage: <script> <inputfile.scad> [--openscad=<executable-path>] [<openscad args>] out.json
#
# Runs OpenSCAD with --interference-check, writing the JSON report to the given
# output path (the CTest harness compares it structurally against the expected
# file). Before handing the file back, the machine-specific parts of the report
# are normalized so the expectation is portable:
#   - every "path" (absolute source file path) becomes its basename
#   - "input.directory" becomes "<testdir>"
#   - "generator.version" becomes "<version>"
#
# The echo export written alongside is discarded; the interference-echo test
# covers the logged warnings.
#
# This script returns 0 on success, non-0 on error.

import argparse
import json
import os
import subprocess
import sys


def failquit(*args):
    if args:
        print(*args, file=sys.stderr)
    print("interference_json_test args:", str(sys.argv), file=sys.stderr)
    print("exiting interference_json_test.py with failure", file=sys.stderr)
    sys.exit(1)


def normalize(obj):
    if isinstance(obj, dict):
        for key, value in obj.items():
            if key == "path" and isinstance(value, str):
                obj[key] = os.path.basename(value)
            elif key == "directory" and isinstance(value, str):
                obj[key] = "<testdir>"
            elif key == "version" and isinstance(value, str):
                obj[key] = "<version>"
            else:
                normalize(value)
    elif isinstance(obj, list):
        for item in obj:
            normalize(item)


parser = argparse.ArgumentParser()
parser.add_argument(
    "--openscad",
    required=False,
    default=os.environ.get("OPENSCAD_BINARY"),
    help='Specify OpenSCAD executable, default to env["OPENSCAD_BINARY"] if absent.',
)
args, remaining_args = parser.parse_known_args()

if len(remaining_args) < 2:
    failquit("expected <inputfile> [openscad args] <out.json>")
inputfile = remaining_args[0]
jsonfile = remaining_args[-1]
openscad_args = remaining_args[1:-1]

if not os.path.exists(inputfile):
    failquit("cant find input file named: " + inputfile)
if not args.openscad or not os.path.exists(args.openscad):
    failquit("cant find openscad executable named: " + str(args.openscad))

echofile = jsonfile + ".echo"
cmd = [
    args.openscad,
    inputfile,
    "--interference-check",
    "--interference-file",
    jsonfile,
    "--export-format",
    "echo",
    "-o",
    echofile,
] + openscad_args
print("Running OpenSCAD:", " ".join(cmd), file=sys.stderr)
sys.stderr.flush()
result = subprocess.call(cmd)
if result != 0:
    failquit("OpenSCAD failed with return code " + str(result))

try:
    with open(jsonfile, "r", encoding="utf-8") as f:
        report = json.load(f)
except Exception as e:  # noqa: BLE001
    failquit("could not read JSON report " + jsonfile + ": " + str(e))

normalize(report)

with open(jsonfile, "w", encoding="utf-8") as f:
    json.dump(report, f, indent=2)
    f.write("\n")

try:
    os.remove(echofile)
except OSError:
    pass
