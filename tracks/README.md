# Local circuit files

`monza.track`, `spa.track`, and `cota.track` contain the same sampled geometry, elevations, and turn markers previously embedded in the source. Editing track data no longer requires recompiling the simulator. Files are read only when their circuit is selected.

The plain-text format has fixed field order:

```text
F1TRACK 1
NAME "Circuit name"
NOTE "Source and approximation notes"
LENGTH 5793
SECTORS 0 0
TURNS 11
624 656 ...
PROFILE 1450
0 x y elevation curvature
next_distance x y elevation curvature
...
```

This is a format illustration, not a complete loadable circuit.

- All lengths, map coordinates, and elevations use metres; curvature uses inverse metres and its sign indicates turn direction.
- `SECTORS 0 0` selects equal thirds. Otherwise provide two increasing distances inside the lap.
- `TURNS` specifies the number of turn marker distances on the following line.
- `PROFILE` specifies the number of five-column samples: distance, x, y, altitude above sea level, signed curvature.
- Distances must increase from 0 to exactly the declared lap length. Final position and elevation must close back to the first sample.
- Numeric data uses a decimal point. Names/notes use quoted strings. Comments and extra trailing fields are not supported.

The loader checks versions, counts, finite values, ordering, sector boundaries, full lap coverage, and closure. Missing or malformed files produce a console error instead of running invalid data.

The current menu is mapped to these three filenames. Adding another circuit to the menu still requires a small C++ menu change, but updating an existing circuit's data does not.

Geometry source: [bacinger/f1-circuits](https://github.com/bacinger/f1-circuits), MIT license (included in the parent folder). Elevation source: [Open Topo Data / SRTM90m](https://www.opentopodata.org/datasets/srtm/). Profiles and markers remain approximate.
