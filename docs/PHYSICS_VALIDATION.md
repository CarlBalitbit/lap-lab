# Stage 1 physics verification and grid refinement

This suite verifies the existing simplified model. It does not establish agreement
with measured motorsports performance. Production equations, circuit data, and
simulation behavior are unchanged. Proposed tolerances are acceptance targets,
not guaranteed properties of the solver. Local speed targets remain exceeded and
are reported as nonblocking diagnostics under the approved CI policy. Execution
errors, invalid outputs, analytical-reference failures, and broken fundamental
invariants still return nonzero. See the acceptance-policy section below.

## Reproducing the checks

From a Visual Studio Developer PowerShell at the repository root:

```powershell
cl /std:c++17 /EHsc /O2 /MT /utf-8 f1_track_sim.cpp /Fe:f1_track_sim.exe
cl /std:c++17 /EHsc /O2 /MT /utf-8 tools/validate_circuits.cpp /Fe:validate_circuits.exe
cl /std:c++17 /EHsc /O2 /MT /utf-8 tools/validate_races.cpp /Fe:validate_races.exe
cl /std:c++17 /EHsc /O2 /MT /utf-8 tools/validate_physics.cpp /Fe:validate_physics.exe
cl /std:c++17 /EHsc /O2 /MT /utf-8 tools/validate_convergence.cpp /Fe:validate_convergence.exe
.\validate_circuits.exe .\tracks
.\validate_races.exe .\tracks
.\validate_physics.exe
.\validate_convergence.exe .\tracks
```

GCC/MinGW can build the same sources using `-std=c++17 -O2 -Wall -Wextra`.
No additional libraries or test framework are required. Validators include the
application source with its interactive `main` renamed, following the existing
validator convention. Explicit checks remain enabled in optimized builds.

## Independent mathematical references

Expected values use fixture inputs and mathematical expressions, never production
force, timing, or interpolation helpers. Production helpers are called only as
the subject under test. References intentionally verify the equations of this
model; they are not independent evidence that those equations describe a real car.

Units are SI except wheel power supplied to `Car` in kW; references use watts.
Gravity is 9.81 m/s^2, matching the documented model constant.

- **Aerodynamics:** `F = rho * C * A * v^2 / 2` for drag and downforce.
  Test densities 1.0/1.225 kg/m^3 and speeds 0/20/60 m/s. The fixture uses
  mass 800 kg, area 1.5 m^2, Cd 0.8, and positive downforce Cl 2.3.
- **Normal load and tire capacity:** use `theta = atan(grade)` and
  `N = m*g*cos(theta) + downforce`. For axle fraction `q`, per-tire load
  `L = q*N/2`, then axle capacity `Cq = 2*mu*L*(F0/L)^lambda`.
  This independently rearranged form checks the production capacity expression.
  Fractions are 0.45/0.55, reference load 2000 N, sensitivity 0.10,
  and grades -0.1/0/0.1. Zero sensitivity is outside current solver input ranges.
- **Combined grip:** lateral demand `Yq = q*m*v^2*k`; available longitudinal
  force `sqrt(Cq^2 - Yq^2)` at feasible demand. Check each axle, rear-only drive,
  and summed braking capacity at 20 m/s and curvature 0.01 1/m.
- **Corner limit:** with zero downforce and flat ground, capacity is constant.
  For radii 25/60/150 m, compute each axle's limit
  `sqrt(Cq*R/(q*m))` and take the smaller value. No reference root solver is used.
- **Incline force resolution:** with equal endpoint speeds of 20 m/s and no
  downforce, independently compute drag, axle capacities, and gravity.
  Drive acceleration is `(min(Crear,P/v)-drag)/m - g*sin(theta)`;
  positive braking capacity is `(Cfront+Crear+drag)/m + g*sin(theta)`.
  Equal endpoints isolate force signs from the conservative cell-bound method.
- **Timing:** solve `v_end = sqrt(v_start^2+2*a*s)` and
  `t = (v_end-v_start)/a`, with `s/v_start` for constant speed. Fixtures cover
  acceleration, braking, constant speed, and a zero-speed launch.
- **Sectors and interpolation:** construct two 12 m cells from the known motion
  `v(t)=10+2t`, `s(t)=10t+t^2`. Independent inverse time is
  `(-10+sqrt(100+4*s))/2`; sector boundaries at 5 and 17 m lie inside cells.
  Check sector times, their sum, and trace time/speed at 0/5/12/17/24 m.
- **Power-limited straight:** mass 800 kg, wheel power 80 kW, distance 1000 m,
  start speed 20 m/s, zero drag/downforce, flat terrain, mu 10.
  An independent rear-capacity calculation exceeds `P/v_start`; since speed
  increases and `P/v` decreases, traction remains nonbinding. The exact solution is
  `v(s) = cbrt(v_start^3 + 3*P*s/m)` and
  `t(s) = m*(v(s)^2-v_start^2)/(2*P)`.

## Acceptance targets

Absolute/relative checks use `error <= atol + rtol*abs(reference)` and reject
nonfinite values. Absolute terms protect comparisons near zero. These targets
were specified before execution and have not been relaxed after observing results.

| Quantity | Target | Rationale |
|---|---|---|
| Forces and axle capacity | 1e-8 N + 1e-10 relative | Floating-point rounding, not discretization |
| Analytic corner speed | 1e-8 m/s + 1e-9 relative | Root-search and rounding allowance |
| Synthetic timing/speed and incline acceleration | 1e-9 in SI units + 1e-10 relative | Exact fixtures within the stated model |
| Synthetic sector sum | 1e-6 s + 1e-10 relative | Accumulation allowance consistent with existing checks |
| Exact straight at 0.125 m | 0.1% time and final-speed error | Finite-step conservative solver target |
| Lap/sector/race time, 0.25 versus 0.125 m | 0.01 s + 0.1% relative | Engineering grid-stability target |
| Sampled speed, 0.25 versus 0.125 m | 0.02 m/s + 0.2% relative | Local interpolation/constraint-transition allowance |

Straight errors must not increase under refinement (1e-10 absolute rounding
allowance). Ratios/orders are omitted near a numerical floor rather than dividing
tiny errors. Circuit targets apply to the fine pair; the coarse pair is reported
but is not constrained by an unapproved additional target.

## Convergence versus numerical stability

All cases now run at maximum spacings 0.5, 0.25, 0.125, and 0.0625 m.
The original three grids and acceptance comparisons remain in place; the added
grid and dense sampling are diagnostics pending review. Actual cell lengths
can be smaller because segments are divided into an integer number of equal cells.
The validator prints cell counts and actual maximum spacing.

The straight has an exact reference: errors at all three grids, shrinking errors,
and error ratios provide demonstrated convergence toward that solution for this
fixture. `log2(error_h/error_h/2)` estimates observed order.

Circuits have no exact reference. The 0.125 m result is only a comparison baseline;
its reported self-difference of zero is not zero physical or numerical error.
The suite reports all three values, differences from that baseline, successive
refinement differences, their ratio, and an observed-order estimate. Ratios near
two are consistent with first-order behavior but three grids alone do not establish
an asymptotic limit or accuracy against real telemetry.

Speed comparisons use 201 fixed physical distances including endpoints over the
whole lap/race, interpolated separately on each grid. They do not compare array
indices. Reported maximum pairwise speed differences can occur at different
locations; their ratio is descriptive, not a pointwise order proof. Sparse samples
cannot certify every point in the continuous speed profile.

Cases are both fictional tracks in qualifying, Monza/Spa/COTA in qualifying,
and three-lap standing/rolling races on fictional track 2. All use the balanced
preset, mu 1, sea-level density 1.225, and enabled elevation. Rolling speed is
10 m/s. Race lap times and first-lap sectors are checked separately.

## Original Stage 1 measured results (before the acceptance-policy change)

Local optimized x64 MSVC run (Visual Studio 2026, toolchain environment v18.10.2).
Rounded values below; validator output retains 15-digit precision.

Analytical validator: **79 checks passed**, largest absolute discrepancy
9.09495e-13 N in an axle-capacity check. No analytical acceptance target failed.

Exact straight references: speed 67.5331341669382 m/s; time 20.8036210520484 s.

| Grid (m) | Final-speed error (m/s) | Speed error (%) | Time error (s) | Time error (%) |
|---|---:|---:|---:|---:|
| 0.5 | 0.006663187 | 0.009866545 | 0.004288438 | 0.020613903 |
| 0.25 | 0.003333411 | 0.004935963 | 0.002145444 | 0.010312839 |
| 0.125 | 0.001667160 | 0.002468656 | 0.001073028 | 0.005157892 |

Successive exact-error ratios: speed 1.998910/1.999454; time 1.998858/1.999429.
Observed orders are 0.9992-0.9996, approximately first order. All exact-straight
targets pass. The computed speed is below, and travel time above, the exact result.

| Case | Time at 0.5 m (s) | At 0.25 m (s) | At 0.125 m (s) | Coarse vs fine (s) | Medium vs fine (s) | Refinement ratio |
|---|---:|---:|---:|---:|---:|---:|
| Fictional 1 | 48.530448 | 48.526214 | 48.524100 | 0.006348 | 0.002113 | 2.003745 |
| Fictional 2 | 30.604342 | 30.601697 | 30.600388 | 0.003954 | 0.001308 | 2.022141 |
| Monza | 121.116243 | 120.972374 | 120.903475 | 0.212768 | 0.068899 | 2.088121 |
| Spa | 166.269552 | 166.084977 | 165.993089 | 0.276463 | 0.091888 | 2.008685 |
| COTA | 167.090154 | 166.821811 | 166.687303 | 0.402851 | 0.134508 | 1.994996 |
| Standing race | 94.216606 | 94.208491 | 94.204472 | 0.012134 | 0.004019 | 2.019058 |
| Rolling race | 92.589677 | 92.581707 | 92.577764 | 0.011913 | 0.003943 | 2.021541 |

All fine-pair total, sector, and individual race-lap time targets pass. Fine-grid
self-differences are zero by definition. Largest fine-pair total-time percentage
is COTA's 0.080695%. Per-sector/per-lap values and ratios are printed by the tool.

| Case | Max speed difference: 0.5 vs 0.125 (m/s) | 0.25 vs 0.125 (m/s) | Pairwise-max refinement ratio |
|---|---:|---:|---:|
| Fictional 1 | 0.075551 | 0.025219 | 1.995759 |
| Fictional 2 | 0.032624 | 0.010889 | 1.996105 |
| Monza | 0.245201 | 0.082129 | 1.985541 |
| Spa | 0.203844 | 0.068014 | 1.997085 |
| COTA | 0.232085 | 0.077542 | 1.993010 |
| Standing/rolling race | 0.032624 | 0.010889 | 1.996105 |

Targets are applied at each sample using that sample's speed, not by comparing
the table's maximum error to a tolerance at a different location.

### Unresolved Monza speed target

At distance 608.265 m, sampled speeds are:

| Grid (m) | Speed (m/s) | Cell start (m) | Cell curvature (1/m) |
|---|---:|---:|---:|
| 0.5 | 16.4560664404 | 608.000 | 0.0136758696944 |
| 0.25 | 16.5685597826 | 608.250 | 0.0136758696944 |
| 0.125 | 16.6257477574 | 608.250 | 0.0130714108891 |

Medium/fine difference **0.0571879748 m/s** exceeds the unchanged
**0.0532514955 m/s** target (approximately 7.39% over the target).
The local grid-dependent curvature cap changes while grade is approximately
0.014726175 on all grids and density differences are negligible. The solver takes
the maximum sampled curvature magnitude within each cell; a finer cell can impose
a different cap. These diagnostics support a curvature-discretization explanation,
but do not isolate all upstream braking-envelope contributions or prove the exact
root cause. Full diagnosis or remediation is a separate approved task.

This is one failed original sampled-speed check, not a failed total lap-time check.
No tolerance was weakened, no case removed, and no production equation changed.
At that stage, this accuracy target blocked CI. The approved policy below retains
the exceedance as a diagnostic; it does not claim the discrepancy has been fixed.

## Original runtime and CI limitations

Initial five-executable build: 14.06 s. Existing circuit validator: 20.82 s
(40 circuits, qualifying/standing modes, exit 0). Existing race validator:
69.74 s (40 circuits, three-lap standing/rolling races and invalid-input checks,
exit 0). The main application and all four validators compiled successfully.
Analytical checks: approximately 0.0006 s inside the executable (process startup
and redirected output add overhead). Final convergence run: 4.54 s, including
diagnostics; per-grid timings are printed. Local build plus validation totals
approximately 109 s, excluding diagnostic recompilation and process overhead.

Windows Actions retains its 15-minute timeout and existing validation steps. New
steps report wall-clock durations and preserve nonzero exit codes. Hosted Actions
has not been run: local timings do not guarantee hosted performance. Only local
MSVC was exercised; GCC and other platforms remain unverified. CI intentionally
had one known failing accuracy target under the original policy. No real-car calibration, dashboard
interaction testing, or schema testing is claimed by this Stage 1 suite.

## Native C++ follow-up: 0.25, 0.125, and 0.0625 m

The follow-up extended the executable to call the unchanged production C++ solver on all
four grids. It retains the 0.5 m baseline in addition to the requested grids.
Every existing convergence case is retained: the exact straight, both fictional
qualifying circuits, Monza/Spa/COTA qualifying, and three-lap standing/rolling
races. No production spacing, curvature sampling, physics, or tolerance changes
were made. These results supersede the earlier in-memory diagnostic reproduction.

Native MSVC build succeeded. Before the acceptance-policy change, the executable
returned **1**, preserving the original Monza failure and original acceptance rules.
Total, first-lap sector, race-lap, and fixed-distance comparisons are
printed at 15-digit precision, including ratios and observed-order estimates where
differences exceed a numerical floor. Grid-difference ratios remain evidence about
refinement behavior, not exact circuit error estimates.

### Total-time differences

| Case | 0.25 m time (s) | 0.125 m (s) | 0.0625 m (s) | First difference (s) | Second difference (s) | Ratio |
|---|---:|---:|---:|---:|---:|---:|
| Fictional 1 | 48.526214 | 48.524100 | 48.523044 | 0.002113 | 0.001057 | 2.000089 |
| Fictional 2 | 30.601697 | 30.600388 | 30.599736 | 0.001308 | 0.000653 | 2.004775 |
| Monza | 120.972374 | 120.903475 | 120.867916 | 0.068899 | 0.035559 | 1.937590 |
| Spa | 166.084977 | 165.993089 | 165.947266 | 0.091888 | 0.045823 | 2.005280 |
| COTA | 166.821811 | 166.687303 | 166.621478 | 0.134508 | 0.065825 | 2.043421 |
| Standing race | 94.208491 | 94.204472 | 94.202465 | 0.004019 | 0.002007 | 2.002937 |
| Rolling race | 92.581707 | 92.577764 | 92.575798 | 0.003943 | 0.001966 | 2.005322 |

First difference means absolute 0.25/0.125 difference; second means 0.125/0.0625.
Each ratio is first divided by second. All time differences are within the
previously proposed time targets when evaluated with the finer reference.

### Sector-time differences

Monza sectors illustrate nonuniform refinement even within one lap:

| Sector | 0.25 m (s) | 0.125 m (s) | 0.0625 m (s) | First difference (s) | Second difference (s) | Ratio |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 40.958790 | 40.924518 | 40.907454 | 0.034272 | 0.017065 | 2.008349 |
| 2 | 46.107142 | 46.082880 | 46.070275 | 0.024262 | 0.012604 | 1.924900 |
| 3 | 33.906442 | 33.896077 | 33.890187 | 0.010365 | 0.005890 | 1.759736 |

All other sector comparisons are emitted by `EXTRA_REFINEMENT` rows. Their
0.125/0.0625 differences in seconds are:

| Case | Sector 1 | Sector 2 | Sector 3 | Sector ratio range |
|---|---:|---:|---:|---:|
| Fictional 1 | 0.000335 | 0.000259 | 0.000462 | 1.999078-2.000715 |
| Fictional 2 | 0.000215 | 0.000152 | 0.000285 | 2.002694-2.005743 |
| Spa | 0.013808 | 0.015378 | 0.016637 | 2.003844-2.006694 |
| COTA | 0.020540 | 0.019525 | 0.025760 | 2.003032-2.093661 |
| Standing race, first lap | 0.000264 | 0.000152 | 0.000285 | 1.990985-2.005743 |
| Rolling race, first lap | 0.000224 | 0.000152 | 0.000285 | 2.002694-2.009760 |

### Fixed-distance speeds and dense Monza sampling

The original 201 distances per case are unchanged; each row now prints all three
requested speeds, both adjacent-grid differences, the extra-grid target status,
and a pointwise refinement ratio where meaningful. Additional diagnostics scan
Monza from 580 through 650 m at 0.05 m spacing, plus the exact original distance
608.265 m: **1,402 points**. Every point and every exceedance is printed; none
is excluded to obtain a pass. Bands describe sampled exceedances, not proven
continuous interval endpoints.

| Comparison | Target exceedances in dense window | Maximum difference (m/s) | Location (m) |
|---|---:|---:|---:|
| 0.25 / 0.125 m | 236 | 0.085170272 | 643.9 |
| 0.125 / 0.0625 m | 0 | 0.029976068 | 643.95 |

The original pair exceeds its target over sampled bands 606.45-615.05 m,
643.8-646.8 m, and at 647 m. The count includes the original 608.265 m sample
in addition to the uniform scan, explaining the difference from the earlier
235-point exceedance count. The maximum ratio of errors at potentially different
locations is not a pointwise convergence-order estimate.

At the original failing point, native C++ speeds are 16.5685597826315,
16.6257477574426, and 16.6539445053010 m/s. Differences are
**0.0571879748112** and **0.0281967478583 m/s**, ratio **2.02817626694**.
The original target **0.0532514955149 m/s** still fails. The extra-fine target
**0.0533078890106 m/s**, using the same unchanged absolute/relative formula,
passes. This confirms refinement behavior without establishing an exact local speed.

For the unchanged 201-point comparisons:

| Case | Max 0.25/0.125 difference (m/s) | Max 0.125/0.0625 difference (m/s) | Max-difference ratio | Extra-pair exceedances |
|---|---:|---:|---:|---:|
| Fictional 1 | 0.025219173 | 0.012622991 | 1.997876 | 0 |
| Fictional 2 | 0.010888658 | 0.005452148 | 1.997132 | 0 |
| Monza | 0.082129450 | 0.037105428 | 2.213408 | 0 |
| Spa | 0.068013995 | 0.034020639 | 1.999198 | 0 |
| COTA | 0.077542423 | 0.038739233 | 2.001651 | 0 |
| Standing/rolling race | 0.010888658 | 0.005452148 | 1.997132 | 0 |

The exact straight at 0.0625 m has final-speed error 0.000833694196416 m/s
(0.001234497%) and time error 0.000536590781024 s (0.002579314%). Exact-error
ratios from 0.125 to 0.0625 m are 1.999726640 for speed and 1.999714411 for time,
providing continued approximately first-order convergence toward the mathematical
reference. Circuit results show consistent refinement/stability but have no exact
reference; Monza sector 3's ratio of 1.76 illustrates why an exact factor of two
should not be imposed indiscriminately.

### Measured follow-up runtime

Final follow-up compilation: 4.88 s. Native validation: 9.70 s internally,
9.78 s wall clock with redirected output. Dense sampling/output took 0.013 s.
The initial follow-up run took 9.61 s internally / 9.70 s wall clock and produced
the same numerical results. Individual 0.0625 m runs in that initial run took
approximately 0.38 s (fictional 1), 0.16 s (fictional 2), 0.95 s (Monza),
1.36 s (Spa), 1.13 s (COTA), and 0.45/0.52 s (standing/rolling races).
Compiler/process/output overhead and host load affect these measurements.
The original validator runtime was approximately 4.54 s. Hosted Actions has not
been run; the 15-minute job timeout and workflow remain unchanged.

## Approved CI acceptance policy

**Mandatory checks establish correctness and enforce supported contracts:**
independent analytical references, finite/feasible simulation results, sector/lap
consistency, qualifying periodicity, race continuity, and documented accuracy
targets for any explicitly supported numerical resolution. A numerical invariant
is not made optional because a convergence diagnostic finds an expected bias.

**Diagnostic checks characterize discretization:** circuit grid differences,
observed orders, dense speed profiles, and provisional pointwise targets that
have not yet been established as supported accuracy guarantees. Report all
exceedances and unexpected trends without asserting an exact circuit solution.
Analytical-reference convergence remains mandatory because it has independent
ground truth; circuit grid agreement cannot substitute for physical validation.

This is an explicit, user-approved acceptance-policy change, not a physics fix.
All cases, four spacings, 201 fixed samples per case, and 1,402 dense Monza samples
remain. Numeric thresholds are unchanged; there are no distance whitelists or
removed samples. Both adjacent-grid time/sector/lap targets are evaluated, along
with all existing fixed/dense speed targets. Every diagnostic exceedance is counted
and printed; the original 608.265 m comparison remains visible.

### Blocking checks and exit codes

- Existing circuit and race validators and the analytical physics validator remain
  mandatory independent CI steps. Every nonzero exit fails the job.
- The convergence validator also blocks on exceptions, invalid arguments, missing
  inputs, nonfinite/negative speeds or times, invalid cells or vector dimensions,
  invalid sector ordering/coverage, nonmonotonic traces, inconsistent distance/time
  totals, sector/lap sums, qualifying closure, infeasible cell accelerations/lateral
  demands, invalid race starts/counts, and discontinuous race crossings.
- Feasibility is checked against the existing model's cell curvature and force
  bounds; this is not a claim of true continuous-profile curvature maxima or
  agreement with real vehicle data. Existing production equations are unchanged.
- Exact-straight accuracy and decreasing exact-reference errors remain mandatory,
  now including the extra-fine grid. Nonfinite diagnostic calculations or invalid
  interpolated outputs always throw; they cannot become nonblocking exceedances.
- Provisional circuit grid-accuracy target exceedances do not increment mandatory
  failure counts. Exit 0 means mandatory checks passed, not all targets passed.
  Exit 1 indicates mandatory failure; exit 2 indicates invalid invocation.

`MANDATORY_PASS` / `MANDATORY_FAIL` identify blocking checks.
`DIAGNOSTIC_TARGET_EXCEEDED` identifies nonblocking accuracy exceedances;
`TARGET_EXCEEDED` in each dense/fixed row has the same diagnostic meaning.
The final summary prints `mandatory_failures` and `diagnostic_target_exceedances`
separately. Counts are comparison occurrences: the original point appears once
in the fixed set and again in the dense set, so they are not unique locations.

The workflow preserves normal exit-code checks; it does **not** use
`continue-on-error` or ignore failures. It runs `validate_convergence --self-test`
before the full convergence suite. Self-tests deliberately construct invalid
outputs and require their rejection (nonfinite comparison/speed, broken dimensions,
invalid cells/sectors, time totals, trace monotonicity/endpoints). They verify
the blocking validation path without changing simulation inputs in normal runs.

Production equations, default 0.25 m spacing, and curvature sampling are untouched.
No real-world prediction accuracy is claimed, and sampled diagnostic passes do
not guarantee unsampled positions. Measurements below record the full local run;
hosted GitHub Actions has not been executed.

### Acceptance-policy verification results

Local MSVC rebuild of the application and all four validators succeeded.
The full local suite completed with the following wall-clock measurements:

| Check | Outcome | Runtime (s) |
|---|---|---:|
| Build application and all validators | Exit 0 | 13.98 |
| Circuit validator, 40 circuits and both modes | Exit 0 | 21.15 |
| Race validator, all circuits and invalid-input cases | Exit 0 | 70.00 |
| Analytical physics, 79 checks | Exit 0 | 0.13 |
| Convergence rejection self-tests, 8 cases | Exit 0 | 0.10 |
| All convergence grids/cases plus dense sampling | Exit 0 | 9.80 |
| Deliberately missing circuit directory | Expected exit 1 | 2.95 |
| Deliberately omitted argument | Expected exit 2 | 0.02 |

Full-suite wall time including process/output overhead was 101.35 s. One build,
the full suite, and one set of negative invocation probes totaled 118.30 s.
These totals exclude investigation/repeated probes and are not hosted CI timing
guarantees. The convergence executable itself reported 9.7795252 s internally.

Final convergence summary: **0 mandatory failures, 237 diagnostic target
exceedances**. Of these, one is the original fixed-distance Monza result and
236 are dense-window comparisons. The fine/extra-fine pair still has zero dense
exceedances. Original point difference 0.0571879748112 m/s exceeds the unchanged
0.0532514955149 m/s target; maximum dense original-pair difference remains
0.0851702721635 m/s at 643.9 m. These numerical issues remain unresolved, visible,
and nonblocking under the approved provisional-accuracy policy.

The missing-directory probe reports three mandatory exceptions (Monza/Spa/COTA)
and exits 1; it demonstrates execution failures are not mistaken for diagnostics.
The self-tests confirm malformed/nonfinite outputs are rejected. Production source
hash matches its Git version; default spacing and curvature sampling are unchanged.

Workflow verification checked all five PowerShell `run` blocks with the native
PowerShell parser, confirmed the four mandatory validator commands and their
exit-code guards, and confirmed no `continue-on-error` setting. Build commands and
validation calls were exercised locally with the same MSVC options as CI. A full
YAML/Actions schema linter was unavailable locally, and hosted GitHub Actions was
not executed. No dependencies were installed to work around that limitation.
