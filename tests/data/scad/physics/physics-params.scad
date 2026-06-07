// Covers parameter parsing and toString() serialization (the cache key):
// all parameters must round-trip into the csg dump.
physics(density = 2.5, friction = 0.9, restitution = 0.25, gravity = 5000,
        max_time = 3, nudge = true, convexity = 2) cube(10);
