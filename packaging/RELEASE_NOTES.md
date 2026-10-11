# Windows distribution notes

The application version is recorded in `VERSION`. This distribution contains the
Windows x64 simulator, 40 local circuit profiles, user instructions, changelog,
and simulator/circuit licenses.

Features include solo and multi-car comparisons, flying qualifying laps, and
standing/rolling races of 1-20 continuous laps. Generated reports support offline
telemetry inspection, circuit maps, playback, and CSV/JSON exports.

This end-user package intentionally excludes developer source, validator tools,
AI configuration, CI configuration, and pre-generated examples. Generate your
own report by completing a session. Source-validation tools remain available in
the project repository.

The model and track geometry are approximate. No gearbox, dynamic load transfer,
banking, tire wear/temperature, fuel consumption, grid spacing, traffic/collisions,
or pit strategy is modeled. Individual race-car reports cover the first lap;
session playback and race exports cover the full race. Numerical verification
does not establish accuracy against measured real-car performance.

See `CHANGELOG.md` for application history and `tracks/CATALOG.md` for circuit
provenance. Preserve `LICENSE` and `CIRCUIT_DATA_LICENSE.txt` when redistributing.
