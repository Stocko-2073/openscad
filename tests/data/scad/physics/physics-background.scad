// Preview-only behavior: background (%) and highlight (#) ghosts in the
// child hierarchy must follow the simulated transform. The % sphere carries
// no mass/collision but rides the settled pose; the # cylinder is part of
// the body and its ghost must overlay the settled solid exactly.
physics() {
  translate([0, 0, 30]) cube(10, center = true);
  % translate([0, 0, 41]) sphere(5, $fn = 32);
  # translate([0, 0, 30]) rotate([0, 90, 0]) cylinder(h = 16, r = 2, center = true, $fn = 24);
}
