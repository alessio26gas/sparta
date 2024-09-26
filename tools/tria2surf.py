#!/bin/bin/env python

# Script:  tria2surf.py
# Purpose: convert a TRIA file (ASCII text) into a SPARTA surface file
# Syntax:  tria2surf.py triafile surffile
#          triafile = read this TRIA file in ASCII format (not binary)
#          surffile = write this SPARTA surface file

import sys

if len(sys.argv) != 3:
    print("Syntax: " + sys.argv[0] + " triafile surffile")
    sys.exit(0)

triafile = sys.argv[1]
surffile = sys.argv[2]

with open(triafile, "r") as i, open(surffile, "w") as o:
    o.write("# SPARTA surface file generated from " + sys.argv[0] + "\n\n")
    for n, line in enumerate(i):
        if n == 0:
            nodes = int(line.strip())
            o.write(f"{nodes} points\n")
        if n == nodes + 1:
            elements = int(line.strip())
            o.write(f"{elements} triangles\n\nPoints\n\n")
            break
    i.seek(0)
    for n, line in enumerate(i):
        if n > 0 and n < nodes + 1:
            o.write(f"{n} " + line)
        if n == nodes + 1:
            o.write("\nTriangles\n\n")
        if n > nodes + 1:
            o.write(f"{n - nodes - 1} " + line[:-3] + "\n")

# stats to screen

print("# of points in SPARTA file:", nodes)
print("# of triangles in SPARTA file:", elements)

