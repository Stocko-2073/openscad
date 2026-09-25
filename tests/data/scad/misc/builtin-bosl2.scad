// BOSL2 ships with OpenSCAD, so this resolves without installing anything.
// Echoes only version-independent facts so a BOSL2 update does not change them.
include <BOSL2/std.scad>

echo(version_parts = len(BOSL_VERSION), major = BOSL_VERSION[0]);
echo(lerp = lerp(0, 10, 0.25));
echo(cube_bounds = pointlist_bounds(cube([10, 20, 30], anchor = BOT)[0]));
