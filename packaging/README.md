# Lap Lab for Windows

Lap Lab is an unofficial educational motorsports lap-time simulator. Results use
simplified physics and approximate circuit data; they are illustrative estimates,
not validated predictions for real vehicles. It is not affiliated with Formula 1
or the FIA.

## Start a session

1. Extract the entire ZIP into a writable folder on 64-bit Windows.
2. Keep `tracks` beside `f1_track_sim.exe` and run the executable.
3. Choose Basic for a quick setup or Advanced for more control.
4. Open the generated `LapData/.../session_report.html` in a modern browser.

No compiler or runtime network downloads are needed. Enter accepts defaults,
B returns to the previous answer, and Q quits. You can compare up to six preset,
custom, or seeded random cars. Qualifying uses flying laps; races support standing
or rolling starts and 1-20 continuous laps on closed circuits.

Reports include interactive maps, telemetry, playback, CSV exports, and JSON
settings. Earlier sessions are retained. In Race mode, individual car reports,
summary CSVs, and time-gap CSVs cover the first lap; the race exports and session
playback cover the full race. Cars run independently with no traffic/collisions,
tire wear, fuel consumption, or pit strategy.

The circuit pack contains 40 approximate profiles. See `tracks/README.md` and
`tracks/CATALOG.md` for format, provenance, elevation coverage, and limitations.
Most profiles use constant reference altitude. Banking is not modeled. Corner
labels and default equal-third sectors are not official numbering/timing lines.

Optional command-line arguments are output directory, then track directory:

```powershell
.\f1_track_sim.exe ".\LapData" ".\tracks"
```

The version is in `VERSION`. Keep `LICENSE` and `CIRCUIT_DATA_LICENSE.txt` with
redistributed copies. Developer source, AI instructions, validation tools, and
pre-generated example sessions are intentionally excluded from this Windows ZIP.
Source and development documentation are available in the project repository:
https://github.com/CarlBalitbit/lap-lab
