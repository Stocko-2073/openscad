// Without nudge, a box dropped exactly onto its edge stays knife-edge
// balanced: the simulation is deterministic and honest about unstable
// equilibria (see physics-nudge-tip.scad for the tie-broken variant).
physics() translate([0, 0, 8]) rotate([45, 0, 0]) cube([4, 10, 10], center = true);
