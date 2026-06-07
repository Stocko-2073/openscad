// A box already resting on the floor must stay (numerically) in place.
physics() translate([0, 0, 5]) cube(10, center = true);
