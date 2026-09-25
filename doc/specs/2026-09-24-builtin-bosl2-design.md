# Built-in BOSL2, kept at the latest release

Status: implemented, 2026-09-24.

[BOSL2](https://github.com/BelfrySCAD/BOSL2) ships inside OpenSCAD. Models
use it as they would an installed library:

```
include <BOSL2/std.scad>
```

Nothing has to be installed, and each GUI launch fetches a newer BOSL2
release when GitHub has one.

## Where the copies live

| Copy | Location | Written by |
|---|---|---|
| Built-in | `builtin-libraries/BOSL2` submodule, pinned to a release tag; copied to `<resources>/builtin-libraries/BOSL2` at build time | the build |
| Updated | macOS `~/Library/Application Support/OpenSCAD/builtin-libraries/BOSL2`, Windows `%LOCALAPPDATA%/OpenSCAD/builtin-libraries/BOSL2`, Linux `~/.local/share/OpenSCAD/builtin-libraries/BOSL2` | the GUI at launch |

A copy's version is `BOSL_VERSION` in its `version.scad`. The updated copy
is used only while it is strictly newer than the built-in one, so rebuilding
the app with a newer submodule is never shadowed by an older download.

## Search order

1. `OPENSCADPATH`, the explicit override;
2. the active BOSL2 folder (built-in or updated, whichever is newer);
3. the user's libraries folder (`~/Documents/OpenSCAD/libraries` on macOS);
4. the bundled `libraries` folder (MCAD).

BOSL2 ranks above the user's libraries folder on purpose: a BOSL2 installed
there by hand before it was built in would otherwise shadow the maintained
copy. That is also why BOSL2 has its own `builtin-libraries` folder rather
than sitting next to MCAD in `libraries`: moving the whole `libraries`
folder up would change MCAD's precedence too. Other libraries in the user's
folder (BOSL, NopSCADlib, …) resolve as before.

## Launch banner

Every launch prints, to stderr, the line after the existing banner saying
which BOSL2 `include <BOSL2/std.scad>` resolves to:

```
OpenSCAD for AI Agents, by Stocko.  See --help for additional AI friendly tools
BOSL2 v2.0.757 built in: include <BOSL2/std.scad>
```

Other forms: `built in (updated from GitHub)`; `from <dir>, overriding the
built-in v2.0.757` when `OPENSCADPATH` supplies its own BOSL2; `not found:
the built-in copy is missing from <dir>` when the submodule was not checked
out. `--info` and the Library Info dialog print the same line, plus the
update folder.

## Update at launch (GUI only)

`gui/BOSL2Updater` runs once per GUI process, after the main window exists:

1. Take `<update folder>/.update.lock` (`QLockFile`); if another OpenSCAD
   holds it, skip. Remove `.BOSL2-*` folders left by an interrupted update.
2. `GET https://api.github.com/repos/BelfrySCAD/BOSL2/releases/latest`.
   Stop if `tag_name` is not newer than the active copy.
3. Download `zipball_url`, then unpack it off the main thread (libzip) into
   `.BOSL2-XXXXXX`, dropping the zipball's top folder and `.git*` entries
   (the same entries the build leaves out of the built-in copy). Paths that
   would escape the folder are rejected.
4. On the main thread, where compiles run: move the old `BOSL2` aside,
   rename the staging folder to `BOSL2`, delete the old one, rebuild the
   library path, and log `BOSL2 updated from vA to vB; the next preview or
   render uses it.`

Being offline, being rate-limited by GitHub or any other network failure is
silent (debug output only): the copy on disk keeps working. A download that
cannot be unpacked or installed logs a warning in the console. GUI test runs
(`--run-all-gui-tests`) skip the update.

The command line never touches the network: scripted and agent runs stay
fast, deterministic and offline-safe. They use the newest copy on disk,
including one the GUI downloaded.

## Bumping the built-in copy

```
git -C builtin-libraries/BOSL2 fetch --tags
git -C builtin-libraries/BOSL2 checkout v2.0.NNN
git add builtin-libraries/BOSL2
```

Re-run CMake afterwards: the bundle copy is made at configure time.

## Tests

* `OpenSCADUnitTests "[BOSL2]"`: tag parsing, numeric version ordering and
  reading `BOSL_VERSION` from `version.scad`.
* `echo_builtin-bosl2`: `include <BOSL2/std.scad>` resolves under ctest
  (whose `OPENSCADPATH` holds only MCAD) and BOSL2 functions evaluate. It
  echoes only version-independent values so an update does not break it.
