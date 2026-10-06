// Dump the view variables for comparison.
// Create some output to look at, and for CSG tree comparison.

echo($vpt=$vpt);
echo($vpr=$vpr);
echo($vpd=$vpd);
echo($vpf=$vpf);

// No matter where you move the camera, this text will
// be facing it and be displayed at the same size.
// (Extruded: a render keeps 2D shapes flat in the XY plane.)
translate($vpt)
  rotate($vpr)
  scale($vpd/200)
  linear_extrude(1) text("Hello, world!", halign="center");
