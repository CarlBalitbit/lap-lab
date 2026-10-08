"""Reproduce the circuit pack with Python 3 (standard library only).

Run: python tools/import_circuits.py [path/to/f1-circuits.geojson]
Existing Monza, Spa and COTA terrain profiles are always preserved.
"""
import bisect
import hashlib
import json
import math
import sys
import unicodedata
import urllib.request
from pathlib import Path

REVISION = '394d8fbe70ef2c0b0c8d23ff7bee61fa09606055'
SOURCE = f'https://raw.githubusercontent.com/bacinger/f1-circuits/{REVISION}/f1-circuits.geojson'
SOURCE_SHA256 = 'a0c8dfb3109a9181d096985eaa30bd692595eae9125b5b8686744600b24621b5'
ROOT = Path(__file__).resolve().parents[1]
TRACKS = ROOT / 'tracks'
PRESERVED = {'it-1922': 'monza', 'be-1925': 'spa', 'us-2012': 'cota'}
# Originally announced 2026 calendar. The catalog records circuit coverage,
# not a claim that each originally scheduled race was held.
SEASON = {'au-1953', 'cn-2004', 'jp-1962', 'bh-2002', 'sa-2021',
          'us-2022', 'ca-1978', 'mc-1929', 'es-1991', 'at-1969',
          'gb-1948', 'be-1925', 'hu-1986', 'nl-1948', 'it-1922',
          'es-2026', 'az-2016', 'sg-2008', 'us-2012', 'mx-1962',
          'br-1940', 'us-2023', 'qa-2004', 'ae-2009'}


def ascii_text(text):
    return unicodedata.normalize('NFKD', text).encode('ascii', 'ignore').decode()


def cumulative(points):
    result = [0.0]
    for a, b in zip(points, points[1:]):
        result.append(result[-1] + math.hypot(b[0] - a[0], b[1] - a[1]))
    return result


def resample(points, distances, count):
    result = []
    for i in range(count):
        distance = i * distances[-1] / count
        j = min(bisect.bisect_right(distances, distance) - 1, len(points) - 2)
        f = (distance - distances[j]) / (distances[j + 1] - distances[j])
        result.append(tuple(a + f * (b - a) for a, b in zip(points[j], points[j + 1])))
    return result


def convert(feature):
    props = feature['properties']
    assert feature['geometry']['type'] == 'LineString'
    coordinates = feature['geometry']['coordinates']
    lon0, lat0 = coordinates[0][:2]
    factor = math.pi / 180 * 6371000
    points = []
    for lon, lat, *_ in coordinates:
        point = ((lon - lon0) * factor * math.cos(math.radians(lat0)), (lat - lat0) * factor)
        if not points or math.dist(point, points[-1]) > 1e-6:
            points.append(point)
    closure_gap = math.dist(points[0], points[-1])
    if closure_gap > 100:
        raise ValueError(f"Open source trace for {props['Name']}: {closure_gap:.1f} m")
    if closure_gap > 1e-6:
        points.append(points[0])
    else:
        points[-1] = points[0]
    length = float(props['length'])
    count = math.ceil(length / 4)
    distances = cumulative(points)
    points = resample(points, distances, count)
    # Periodic Gaussian smoothing (about 8 m) removes polyline vertex spikes.
    # This is an approximate centerline, not a surveyed racing line.
    weights = [math.exp(-0.5 * (j / 2) ** 2) for j in range(-6, 7)]
    weight_sum = sum(weights)
    points = [tuple(sum(weights[j + 6] * points[(i + j) % count][axis]
                        for j in range(-6, 7)) / weight_sum for axis in (0, 1))
              for i in range(count)]
    points.append(points[0])
    # Normalize to upstream's declared lap length, then resample uniformly.
    scale = length / cumulative(points)[-1]
    points = [(x * scale, y * scale) for x, y in points]
    points = resample(points, cumulative(points), count)
    origin = points[0]
    points = [(x - origin[0], y - origin[1]) for x, y in points]
    curvature = []
    for i, b in enumerate(points):
        a, c = points[(i - 1) % count], points[(i + 1) % count]
        u = (b[0] - a[0], b[1] - a[1])
        v = (c[0] - b[0], c[1] - b[1])
        denominator = math.dist(a, b) * math.dist(b, c) * math.dist(a, c)
        curvature.append(2 * (u[0] * v[1] - u[1] * v[0]) / denominator if denominator > 1e-9 else 0)
    # Geometry-derived peak labels are deliberately not official turn numbers.
    candidates = [i for i in range(count) if abs(curvature[i]) >= 0.002
                  and abs(curvature[i]) >= abs(curvature[(i - 1) % count])
                  and abs(curvature[i]) > abs(curvature[(i + 1) % count])]
    selected = []
    for i in sorted(candidates, key=lambda i: abs(curvature[i]), reverse=True):
        if all(min(abs(i - j), count - abs(i - j)) * length / count >= 60 for j in selected):
            selected.append(i)
    if not selected:
        selected = [max(range(count), key=lambda i: abs(curvature[i]))]
    markers = [i * length / count for i in sorted(selected)]
    altitude = float(props.get('altitude', 0))
    note = ('Approximate bacinger/f1-circuits centerline, source layout may differ from current configuration. '
            'Constant upstream reference altitude; no terrain elevation profile or banking. '
            'Turn labels are automatic curvature peaks, not official turn numbers. '
            'Start position follows source; sectors are equal thirds, not official timing lines.')
    rows = ['F1TRACK 1', 'NAME ' + json.dumps(ascii_text(props['Name'])),
            'NOTE ' + json.dumps(note), f'LENGTH {length:.8f}', 'SECTORS 0 0',
            f'TURNS {len(markers)}', ' '.join(f'{d:.8f}' for d in markers), f'PROFILE {count + 1}']
    for i in range(count + 1):
        x, y = points[i % count]
        rows.append(f'{i * length / count:.8f} {x:.8f} {y:.8f} {altitude:.8f} {curvature[i % count]:.8f}')
    return '\n'.join(rows) + '\n', len(markers), closure_gap


def main():
    raw = Path(sys.argv[1]).read_bytes() if len(sys.argv) > 1 else urllib.request.urlopen(SOURCE, timeout=60).read()
    if hashlib.sha256(raw).hexdigest() != SOURCE_SHA256:
        raise ValueError('Input does not match the pinned source revision; update provenance before importing.')
    data = json.loads(raw)
    rows = []
    TRACKS.mkdir(exist_ok=True)
    for feature in data['features']:
        props = feature['properties']
        circuit_id = props['id']
        slug = PRESERVED.get(circuit_id, circuit_id)
        filename = slug + '.track'
        if circuit_id in PRESERVED:
            if not (TRACKS / filename).is_file():
                raise FileNotFoundError(f'Missing preserved profile: {filename}')
            profile = 'Existing SRTM terrain profile'
        else:
            content, turns, gap = convert(feature)
            (TRACKS / filename).write_text(content, encoding='utf-8')
            profile = f'Constant reference altitude; {turns} automatic corner labels'
        rows.append((ascii_text(props['Name']), filename, props['length'], circuit_id in SEASON, profile))
    assert SEASON <= {f['properties']['id'] for f in data['features']}
    header = f'''# Circuit catalog

{len(rows)} circuits from [bacinger/f1-circuits](https://github.com/bacinger/f1-circuits), including all 24 venues in the originally announced 2026 calendar and additional historic circuits. This is a venue pack, not a race schedule; calendar changes do not remove circuits. Sepang is also included.

Source revision: `{REVISION}`. Source GeoJSON SHA-256: `{hashlib.sha256(raw).hexdigest()}`.

The geometry is unofficial. New profiles use a local map projection, approximately 4 m sampling and 8 m Gaussian smoothing, then normalization to the upstream declared length. Layout dates are not verified against 2026: upstream geometry or lengths may describe older configurations. Madrid is an approximate source trace. This pack does not claim exact current-season layouts or official lap predictions.

Monza, Spa and COTA retain their existing terrain profiles and approximate manual turn markers. Other circuits use a **constant reference altitude** from upstream: air density reflects that altitude, but hills are not modeled. Automatic curvature peaks supply approximate corner labels; they are not official corner numbering. New sectors are equal thirds and start positions follow the source trace. Banking is not modeled.

The upstream MIT notice is preserved in `../CIRCUIT_DATA_LICENSE.txt`. Regenerate the added files with `python tools/import_circuits.py` from the project root (Python standard library only). Original three profiles are never overwritten. Add any valid `.track` file to this folder to make it appear in the menu without recompiling.

| Circuit | File | Source length (m) | Original 2026 calendar | Profile |
|---|---|---:|---|---|
'''
    for name, filename, length, season, profile in sorted(rows):
        header += f'| {name} | `{filename}` | {length} | {"Yes" if season else "Extra"} | {profile} |\n'
    (TRACKS / 'CATALOG.md').write_text(header, encoding='utf-8')
    print(f'Prepared {len(rows)} circuits; preserved {len(PRESERVED)} original profiles.')


if __name__ == '__main__':
    main()
