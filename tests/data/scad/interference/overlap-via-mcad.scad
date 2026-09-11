// The colliding primitive comes from a library on OPENSCADPATH (MCAD). Library
// steps are dropped from the chain, leaving only the translate from this file.
use <MCAD/shapes.scad>
cube(10);                          // part 1
translate([5, 5, 5]) box(4, 4, 4); // part 2: centered 4mm cube fully inside part 1, overlap 64
