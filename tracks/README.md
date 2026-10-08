# Local circuit files

The pack contains 40 circuits. `monza.track`, `spa.track`, and `cota.track` retain their original sampled geometry, terrain elevations, and approximate turn markers. The other 37 use imported MIT-licensed centerlines and constant reference altitude. See [CATALOG.md](CATALOG.md) for the complete list and source revision.

The menu discovers lowercase `.track` files in this folder. Original choices remain first, followed by other circuits sorted by name. Header metadata is read for the menu; the complete profile is loaded and validated only on selection. Invalid headers are skipped with a message; invalid selected profiles produce an error.

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

Add or edit a valid `.track` file to extend the menu without recompiling. Restart the circuit selection to pick up changes. Missing/empty circuit folders produce a clear console error.

Geometry source: [bacinger/f1-circuits](https://github.com/bacinger/f1-circuits), MIT license (included in the parent folder). Original three terrain profiles: [Open Topo Data / SRTM90m](https://www.opentopodata.org/datasets/srtm/). Imported profiles use upstream reference altitudes and do not model hills or banking. Curvature-peak labels are not official turn numbering. Profiles, source configurations, starts, and sectors remain approximate.
