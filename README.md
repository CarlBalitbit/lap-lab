# F1 Lap Time Simulator

A C++ console application for estimating lap times and comparing car setups on fictional, custom, and sampled real-world circuits.

## Features

- Solo runs or comparisons of up to six cars.
- High-downforce, balanced, low-downforce, custom, and random car setups.
- Monza, Spa-Francorchamps, and Circuit of the Americas track profiles.
- Fictional presets and a custom track designer.
- Flying qualifying laps and standing starts.
- Basic setup defaults and advanced grip, traction, elevation, and sector settings.
- Saved HTML reports with track maps and telemetry, CSV results, and JSON session data.

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
- `CIRCUIT_DATA_LICENSE.txt`: license for the circuit geometry data.

## Model and limitations

The model includes constant wheel power, aerodynamic drag and downforce, load-sensitive tire grip, shared grip for cornering and acceleration/braking, road incline, and altitude-dependent air density. It does not model a gearbox. Circuit profiles, elevations, and turn markers are approximate, so results should be treated as simplified estimates rather than validated real-car lap predictions.

## Circuit data attribution

Circuit geometry comes from [bacinger/f1-circuits](https://github.com/bacinger/f1-circuits) under the MIT license; the required notice is preserved in `CIRCUIT_DATA_LICENSE.txt` and the source. Elevations use smoothed [Open Topo Data / SRTM90m](https://www.opentopodata.org/datasets/srtm/) terrain estimates.

The circuit-data license applies to that data. This repository does not currently declare a separate license for the simulator code.
