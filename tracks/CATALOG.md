# Circuit catalog

40 circuits from [bacinger/f1-circuits](https://github.com/bacinger/f1-circuits), including all 24 venues in the originally announced 2026 calendar and additional historic circuits. This is a venue pack, not a race schedule; calendar changes do not remove circuits. Sepang is also included.

Source revision: `394d8fbe70ef2c0b0c8d23ff7bee61fa09606055`. Source GeoJSON SHA-256: `a0c8dfb3109a9181d096985eaa30bd692595eae9125b5b8686744600b24621b5`.

The geometry is unofficial. New profiles use a local map projection, approximately 4 m sampling and 8 m Gaussian smoothing, then normalization to the upstream declared length. Layout dates are not verified against 2026: upstream geometry or lengths may describe older configurations. Madrid is an approximate source trace. This pack does not claim exact current-season layouts or official lap predictions.

Monza, Spa and COTA retain their existing terrain profiles and approximate manual turn markers. Other circuits use a **constant reference altitude** from upstream: air density reflects that altitude, but hills are not modeled. Automatic curvature peaks supply approximate corner labels; they are not official corner numbering. New sectors are equal thirds and start positions follow the source trace. Banking is not modeled.

The upstream MIT notice is preserved in `../CIRCUIT_DATA_LICENSE.txt`. Regenerate the added files with `python tools/import_circuits.py` from the project root (Python standard library only). Original three profiles are never overwritten. Add any valid `.track` file to this folder to make it appear in the menu without recompiling.

| Circuit | File | Source length (m) | Original 2026 calendar | Profile |
|---|---|---:|---|---|
| Albert Park Circuit | `au-1953.track` | 5278 | Yes | Constant reference altitude; 34 automatic corner labels |
| Autodromo Enzo e Dino Ferrari | `it-1953.track` | 4909 | Extra | Constant reference altitude; 24 automatic corner labels |
| Autodromo Hermanos Rodriguez | `mx-1962.track` | 4304 | Yes | Constant reference altitude; 21 automatic corner labels |
| Autodromo Internacional Nelson Piquet | `br-1977.track` | 5031 | Extra | Constant reference altitude; 25 automatic corner labels |
| Autodromo Internacional do Algarve | `pt-2008.track` | 4653 | Extra | Constant reference altitude; 20 automatic corner labels |
| Autodromo Internazionale del Mugello | `it-1914.track` | 5245 | Extra | Constant reference altitude; 28 automatic corner labels |
| Autodromo Jose Carlos Pace - Interlagos | `br-1940.track` | 4309 | Yes | Constant reference altitude; 26 automatic corner labels |
| Autodromo Nazionale Monza | `monza.track` | 5793 | Yes | Existing SRTM terrain profile |
| Autodromo Oscar y Juan Galvez | `ar-1952.track` | 4322 | Extra | Constant reference altitude; 23 automatic corner labels |
| Autodromo do Estoril | `pt-1972.track` | 4349 | Extra | Constant reference altitude; 23 automatic corner labels |
| Bahrain International Circuit | `bh-2002.track` | 5412 | Yes | Constant reference altitude; 23 automatic corner labels |
| Baku City Circuit | `az-2016.track` | 6003 | Yes | Constant reference altitude; 28 automatic corner labels |
| Circuit Gilles-Villeneuve | `ca-1978.track` | 4361 | Yes | Constant reference altitude; 26 automatic corner labels |
| Circuit Paul Ricard | `fr-1969.track` | 5842 | Extra | Constant reference altitude; 31 automatic corner labels |
| Circuit Zandvoort | `nl-1948.track` | 4259 | Yes | Constant reference altitude; 34 automatic corner labels |
| Circuit de Barcelona-Catalunya | `es-1991.track` | 4655 | Yes | Constant reference altitude; 24 automatic corner labels |
| Circuit de Monaco | `mc-1929.track` | 3337 | Yes | Constant reference altitude; 30 automatic corner labels |
| Circuit de Nevers Magny-Cours | `fr-1960.track` | 4412 | Extra | Constant reference altitude; 31 automatic corner labels |
| Circuit de Spa-Francorchamps | `spa.track` | 7004 | Yes | Existing SRTM terrain profile |
| Circuit of the Americas | `cota.track` | 5514 | Yes | Existing SRTM terrain profile |
| Circuito de Madring | `es-2026.track` | 5474 | Yes | Constant reference altitude; 39 automatic corner labels |
| Hockenheimring | `de-1932.track` | 4574 | Extra | Constant reference altitude; 25 automatic corner labels |
| Hungaroring | `hu-1986.track` | 4381 | Yes | Constant reference altitude; 21 automatic corner labels |
| Indianapolis Motor Speedway | `us-1909.track` | 4192 | Extra | Constant reference altitude; 26 automatic corner labels |
| Intercity Istanbul Park | `tr-2005.track` | 5338 | Extra | Constant reference altitude; 29 automatic corner labels |
| Jeddah Corniche Circuit | `sa-2021.track` | 6175 | Yes | Constant reference altitude; 46 automatic corner labels |
| Kyalami Grand Prix Circuit | `za-1961.track` | 4529 | Extra | Constant reference altitude; 23 automatic corner labels |
| Las Vegas Street Circuit | `us-2023.track` | 6201 | Yes | Constant reference altitude; 30 automatic corner labels |
| Losail International Circuit | `qa-2004.track` | 5380 | Yes | Constant reference altitude; 23 automatic corner labels |
| Marina Bay Street Circuit | `sg-2008.track` | 4928 | Yes | Constant reference altitude; 26 automatic corner labels |
| Miami International Autodrome | `us-2022.track` | 5412 | Yes | Constant reference altitude; 33 automatic corner labels |
| Nurburgring | `de-1927.track` | 5148 | Extra | Constant reference altitude; 29 automatic corner labels |
| Red Bull Ring | `at-1969.track` | 4318 | Yes | Constant reference altitude; 24 automatic corner labels |
| Sepang International Circuit | `my-1999.track` | 5543 | Extra | Constant reference altitude; 25 automatic corner labels |
| Shanghai International Circuit | `cn-2004.track` | 5451 | Yes | Constant reference altitude; 31 automatic corner labels |
| Silverstone Circuit | `gb-1948.track` | 5891 | Yes | Constant reference altitude; 36 automatic corner labels |
| Sochi Autodrom | `ru-2014.track` | 5848 | Extra | Constant reference altitude; 31 automatic corner labels |
| Suzuka International Racing Course | `jp-1962.track` | 5807 | Yes | Constant reference altitude; 43 automatic corner labels |
| Watkins Glen International | `us-1956.track` | 5430 | Extra | Constant reference altitude; 26 automatic corner labels |
| Yas Marina Circuit | `ae-2009.track` | 5281 | Yes | Constant reference altitude; 30 automatic corner labels |
