// Nested user modules: the chain for part 2 reads a -> translate -> b -> cube,
// with the "module " prefix stripped from the module steps.
module a() {
  translate([5, 0, 0]) b();
}
module b() {
  cube(10);
}
cube(10);  // part 1
a();       // part 2: overlaps part 1 by 500
