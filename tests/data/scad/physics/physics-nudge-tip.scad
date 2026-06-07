// With nudge=true the deterministic tie-breaker spin tips the edge-balanced
// box onto one of its faces.
physics(nudge = true) translate([0, 0, 8]) rotate([45, 0, 0]) cube([4, 10, 10], center = true);
