// Part 1 is a cube with a slab (x 10..15) subtracted. Part 2 straddles the
// slab and the remaining material (x 12..18), so the collision is x 15..18
// (volume 48). The subtracted slab's bounding box overlaps part 2 too, but it
// removes material, so it must not appear among the colliding primitives.
difference() {
  cube(20);
  translate([10, -1, -1]) cube([5, 22, 22]);
}
translate([12, 5, 5]) cube([6, 4, 4]);
