# Lap Lab 0.1.0

F1 Lap Time Simulator

First development release of an unofficial C++ lap-time and car-comparison project by Carl Balitbit.

## Windows download

Download `lap-lab-0.1.0-windows-x64.zip`, extract the entire folder, and run `f1_track_sim.exe`. Keep `tracks` beside the executable. The release contains a statically linked MSVC runtime; no compiler is needed to run it. It targets 64-bit Windows and was checked on the author's Windows machine.

Choose Basic settings for a quick start. Qualifying simulates a flying lap; Race offers standing or rolling starts and 1-20 continuous laps. Open the newly saved `LapData/.../session_report.html` in a modern browser. Enter accepts defaults, B goes back and Q quits.

## Included

- 40 circuit profiles, including the originally announced 2026 venues and historic circuits.
- Preset, custom and random cars; solo sessions or comparisons of up to six cars.
- Offline HTML telemetry dashboard, map follow, speed slider, chart zoom and position lock.
- Continuous race playback with lap counters, standings and leader gaps.
- CSV/JSON exports and source-validation tools.

The example report in `example/` shows a three-car, three-lap standing-start race on a fictional circuit. Open its `session_report.html`. It is a demonstration, not a measured F1 result.

## Known limitations

- Unofficial educational simulator, not affiliated with or endorsed by Formula 1 or the FIA.
- Circuit geometry and start alignment are approximate and can describe older configurations.
- Automatic corner markers are curvature peaks, not official turn numbering. Crowded labels may require zooming.
- Sectors default to equal distance thirds, not official timing boundaries.
- Monza, Spa and COTA have approximate terrain profiles; other circuits use constant reference altitude.
- No gearbox, tire temperature/wear, dynamic load transfer, banking, fuel model, pit stops, starting-grid spacing or traffic/collisions.
- Individual race-car HTML/CSV reports cover the first lap; the session report and race exports cover all laps.
- Browser interactions were checked with control-logic fixtures and manually reviewed by the author; there is no automated visual-browser coverage.

## License and credit

Simulator license: MIT, copyright 2026 Carl Balitbit. Copies and substantial portions must retain the copyright/license notice. Circuit geometry has a separate MIT notice in `CIRCUIT_DATA_LICENSE.txt`, crediting Tomislav Bacinger. The README and circuit catalog describe data provenance and approximations.
