// Cube dropped from z=50 must land flat on the floor with its base at z=0,
// rotation staying (numerically) identity.
physics() translate([0, 0, 50]) cube(10, center = true);
