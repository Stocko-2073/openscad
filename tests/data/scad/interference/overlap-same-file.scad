// Two overlapping cubes, a flush pair, and a far-away part.
// Expect one collision (parts 1 and 2, volume 400) with both cubes attributed.
cube(10);                       // part 1
translate([5, 2, 0]) cube(10);  // part 2: overlaps part 1 (overlap volume 400)
translate([20, 0, 0]) cube(10); // part 3: shares a flush face with part 4
translate([30, 0, 0]) cube(10); // part 4: flush with part 3 (overlap volume 0)
translate([0, 50, 0]) cube(10); // part 5: far away, bounding-box culled
