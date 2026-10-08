# F1 Lap Time Simulator

A C++ console application for estimating lap times and comparing car setups on fictional, custom, and sampled real-world circuits.

## Features

- Solo runs or comparisons of up to six cars.
- High-downforce, balanced, low-downforce, custom, and random car setups.
- 40 circuit profiles, covering all venues in the originally announced 2026 season plus historic circuits.
- Automatic circuit discovery: add a valid `.track` file without recompiling.
- Fictional presets and a custom track designer.
- Qualifying mode with flying laps, or continuous Race mode with standing/rolling starts.
- Races of 1 to 20 laps, with a shared clock, live standings, lap counters, and independent car progress.
- Basic setup defaults and advanced grip, traction, elevation, and sector settings.
- Saved HTML reports with a clean dark dashboard, track maps and telemetry, CSV results, and JSON session data.
- Playback-speed slider, telemetry chart zoom, and a manual position lock. Hidden comparison cars cannot be selected for map follow.

## Build on Windows

Use a C++17-capable compiler. VS Code is the editor; a C++ compiler must be installed separately. The source uses the C++ standard library and requires no third-party C++ libraries.

Open this folder in VS Code, then open **Terminal > New Terminal**.

With GCC/MinGW available in your terminal:

```powershell
g++ -std=c++17 -O2 -Wall -Wextra f1_track_sim.cpp -o f1_track_sim.exe
```

Alternatively, in a Visual Studio Developer PowerShell with MSVC available:

```powershell
cl /std:c++17 /EHsc /O2 /utf-8 f1_track_sim.cpp /Fe:f1_track_sim.exe
```

## Run

From the project folder:

```powershell
.\f1_track_sim.exe
```

Follow the console menus to choose settings, cars, a circuit, and lap mode. Press Enter to accept a default, B to return to the previous answer, or Q to quit.

Keep the `tracks` folder beside the executable. The program reads circuit data locally; no runtime downloads are needed.

Optional arguments set the output folder first and the track folder second:

```powershell
.\f1_track_sim.exe ".\LapData" ".\tracks"
```

Each completed run saves a new session folder under `LapData` by default. Open its `session_report.html` in a browser to inspect the results. Earlier sessions are retained. Generated reports and compiled files are excluded from Git.

## Project files

- `f1_track_sim.cpp`: simulation, interactive setup, and report generation.
- `tracks/*.track`: local circuit geometry and elevation profiles.
- `tracks/README.md`: circuit file format and validation details.
- `tracks/CATALOG.md`: complete circuit list, provenance, and approximation details.
- `tools/import_circuits.py`: reproducible importer (Python 3 standard library only).
- `tools/validate_circuits.cpp`: validates every circuit in qualifying and standing-start modes.
- `CIRCUIT_DATA_LICENSE.txt`: license for the circuit geometry data.

## Model and limitations

The model includes constant wheel power, aerodynamic drag and downforce, load-sensitive tire grip, shared grip for cornering and acceleration/braking, road incline, and altitude-dependent air density. It does not model a gearbox. Circuit profiles, elevations, and turn markers are approximate, so results should be treated as simplified estimates rather than validated real-car lap predictions.

## Circuit data attribution

Circuit geometry comes from [bacinger/f1-circuits](https://github.com/bacinger/f1-circuits) under the MIT license; the required notice is preserved in `CIRCUIT_DATA_LICENSE.txt` and the source. The original Monza, Spa and COTA profiles use smoothed [Open Topo Data / SRTM90m](https://www.opentopodata.org/datasets/srtm/) terrain estimates. The 37 added profiles use constant upstream reference altitudes, so they do not model hills. Their turn labels are automatic curvature peaks, sectors are equal thirds, and source layouts may differ from current configurations. See [the circuit catalog](tracks/CATALOG.md) for provenance and limitations.

The circuit-data license applies to that data. This repository does not currently declare a separate license for the simulator code.

## Validate the circuit pack

In a Visual Studio Developer PowerShell, from the project root:

```powershell
cl /std:c++17 /EHsc /O2 /utf-8 tools/validate_circuits.cpp /Fe:validate_circuits.exe
.\validate_circuits.exe .\tracks
```

The validator loads every discoverable circuit and checks both lap modes, finite speeds, closure, and sector totals at the normal 0.25 m simulation spacing. It does not establish agreement with real F1 lap times.

## Race mode

Choose **Race** after choosing cars and a track. Pick a standing start (zero speed) or rolling start (a common configurable speed, default 80 km/h). Choose 1 to 20 laps, default 5. A rolling speed must be feasible at the selected circuit's start for every car; if the simulator rejects it, use a lower speed.

Race physics run over consecutive copies of the closed circuit, carrying speed continuously across lap boundaries. Each car starts its next lap immediately. Cars run independently, starting together at the same position; there is no starting grid spacing, collision/traffic model, tire wear, fuel consumption, or pit strategy. Open custom tracks cannot be used for races.

Open `session_report.html` for continuous race playback, live position/lap counts and gaps to the leader. The time slider controls the shared race clock. Zoomed charts follow the selected reference car's current lap. The static results table shows final race totals; the live table shows progress at the current playback time. Cars stop only after completing the configured race distance. Loop replays the complete race and is off by default.

Race exports are `race_results.csv`, `race_laps.csv`, and `race_telemetry.csv` (all laps). Each individual car HTML/telemetry report, plus the existing `summary.csv` and `time_gaps.csv`, describes the first lap only. Saved settings record the race mode, lap count, and starting speed.

To validate race physics in a Visual Studio Developer PowerShell:

```powershell
cl /std:c++17 /EHsc /O2 /utf-8 tools/validate_races.cpp /Fe:validate_races.exe
.\validate_races.exe .\tracks
```
