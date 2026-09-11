// The colliding primitive is defined in another file. Its chain must keep only
// the steps from this file: the pin() call, not the translate/cube inside it.
use <lib/parts-lib.scad>
cube(10);  // part 1
pin();     // part 2: 2x2 peg through the cube, overlap volume 40
