#!/usr/bin/env python3
"""
Turn a Heltec RF-survey log (+ optional iPhone GPX track) into maps and plots.

Usage:
    python survey_map.py rfsurvey_boot7.csv --gpx track.gpx --out results
    python survey_map.py log.csv                 # if a GPS module was wired to the Heltec

Options:
    --antenna-gain 3      antenna gain dBi to subtract (Laird 3 dBi default = 0, i.e. not applied)
    --max-gap 30          max seconds between GPX points to interpolate across
    --time-shift 0        seconds to add to device times if the phone/device clocks disagree

Outputs (in --out):
    map.html          Leaflet map, points coloured by noise floor (open in any browser)
    survey.kml        same points for Google Earth
    points.csv        one row per logged window: time, lat, lon, floor, median, peak, busy
    spectrum.png      average / max-hold / min spectrum over the whole drive   (needs matplotlib)
    waterfall.png     channel-vs-time heatmap                                   (needs matplotlib)
    timeline.png      floor / median / peak over time with MARKs                (needs matplotlib)
    stations.csv      one row per GO station (dwell baseline) with per-channel mean spectrum
    stations.png      mean spectrum of each GO station overlaid                  (needs matplotlib)

Only the Python standard library is required; matplotlib adds the PNG plots.
"""
import argparse
import bisect
import csv
import json
import math
import os
import sys
import xml.etree.ElementTree as ET
from datetime import datetime, timezone


# ---------------------------------------------------------------- parsing
def parse_log(path):
    header = None
    rows, syncs, marks, stations = [], {}, [], {}
    with open(path, newline="", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            if line.startswith("#STATION,"):
                p = line.split(",", 15)
                if len(p) < 15:
                    continue
                key = (int(p[1]), int(p[2]))
                stations.setdefault(key, {}).update({
                    "boot": key[0], "id": key[1], "start_ms": int(p[3]), "end_ms": int(p[4]),
                    "start_utc": int(p[5]) if p[5] else 0, "end_utc": int(p[6]) if p[6] else 0,
                    "sweeps": int(p[7]), "floor": float(p[8]), "median": float(p[9]),
                    "peak": float(p[10]), "peak_mhz": float(p[11]), "busy": int(p[12]),
                    "early": p[13] == "1", "note": p[14] if len(p) > 14 else ""})
                continue
            if line.startswith(("#STATION_MEAN,", "#STATION_MIN,", "#STATION_MAX,")):
                p = line.split(",")
                kind = p[0][len("#STATION_"):].lower()
                stations.setdefault((int(p[1]), int(p[2])), {})[kind] = [float(x) for x in p[3:]]
                continue
            if line.startswith("#SYNC,"):
                _, boot, ms, epoch, *_ = line.split(",")
                syncs.setdefault(int(boot), []).append((int(ms), int(epoch)))
                continue
            if line.startswith("#MARK,"):
                parts = line.split(",", 4)
                marks.append({"boot": int(parts[1]), "millis": int(parts[2]),
                              "utc_ms": int(parts[3] or 0), "note": parts[4] if len(parts) > 4 else ""})
                continue
            if line.startswith("#"):
                continue
            if line.startswith("boot,"):
                header = line.split(",")
                continue
            if header is None:
                continue
            vals = line.split(",")
            if len(vals) != len(header):
                continue  # partial row (power cut mid-write)
            rows.append(dict(zip(header, vals)))
    if header is None:
        sys.exit("No header row found — is this a RF-Survey log?")
    freqs = [float(h[1:]) for h in header if h.startswith("f") and h[1:].replace(".", "").isdigit()]
    stations = [v for k, v in sorted(stations.items()) if "floor" in v]
    return header, freqs, rows, syncs, marks, stations


def resolve_times(rows, syncs, marks, shift_s, stations=()):
    """Device time for every row/mark: use the row's own utc_ms, otherwise back-fill from
    any #SYNC in the same boot (rows logged before the phone connected still get a time)."""
    def from_sync(boot, ms):
        lst = syncs.get(boot)
        if not lst:
            return None
        sms, sep = lst[-1]          # latest sync in that boot is the best
        return sep + (ms - sms)

    for r in rows:
        t = int(r["utc_ms"]) if r.get("utc_ms") else from_sync(int(r["boot"]), int(r["millis"]))
        r["_t"] = None if t is None else t / 1000.0 + shift_s
    for m in marks:
        t = m["utc_ms"] or from_sync(m["boot"], m["millis"])
        m["_t"] = None if not t else t / 1000.0 + shift_s
    for s in stations:
        a = s["start_utc"] or from_sync(s["boot"], s["start_ms"])
        b = s["end_utc"] or from_sync(s["boot"], s["end_ms"])
        s["_t0"] = a / 1000.0 + shift_s if a else None
        s["_t1"] = b / 1000.0 + shift_s if b else None


def parse_gpx(path):
    tree = ET.parse(path)
    pts = []
    for el in tree.iter():
        tag = el.tag.split("}")[-1]
        if tag not in ("trkpt", "rtept", "wpt"):
            continue
        t = None
        for ch in el:
            if ch.tag.split("}")[-1] == "time" and ch.text:
                s = ch.text.strip().replace("Z", "+00:00")
                t = datetime.fromisoformat(s)
                if t.tzinfo is None:
                    t = t.replace(tzinfo=timezone.utc)
        if t is None:
            continue
        pts.append((t.timestamp(), float(el.get("lat")), float(el.get("lon"))))
    pts.sort()
    return pts


def interp(track, t, max_gap):
    if not track or t is None:
        return None
    times = [p[0] for p in track]
    i = bisect.bisect_left(times, t)
    if i < len(times) and abs(times[i] - t) < 1e-6:
        return track[i][1], track[i][2]
    if i == 0 or i == len(times):
        return None
    (t0, a0, o0), (t1, a1, o1) = track[i - 1], track[i]
    if t1 - t0 > max_gap:
        return None
    k = (t - t0) / (t1 - t0)
    return a0 + k * (a1 - a0), o0 + k * (o1 - o0)


# ---------------------------------------------------------------- outputs
def colour(v, lo, hi):
    """green (quiet) -> yellow -> red (noisy)"""
    k = 0.0 if hi <= lo else max(0.0, min(1.0, (v - lo) / (hi - lo)))
    r = int(255 * min(1, 2 * k)); g = int(255 * min(1, 2 * (1 - k)))
    return r, g, 40


def write_points_csv(path, pts):
    with open(path, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["utc", "lat", "lon", "floor_p20_dbm", "median_dbm", "peak_dbm", "peak_mhz", "busy_channels", "pos_source"])
        for p in pts:
            w.writerow([p["iso"], f"{p['lat']:.7f}", f"{p['lon']:.7f}", p["floor"], p["median"],
                        p["peak"], p["peak_mhz"], p["busy"], p["src"]])


def write_stations_csv(path, stations, freqs):
    with open(path, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["station", "label", "utc_start", "seconds", "sweeps", "lat", "lon", "floor_p20_dbm", "median_dbm",
                    "peak_dbm", "peak_mhz", "busy_channels", "stopped_early"] + [f"mean_{f:.1f}" for f in freqs])
        for s in stations:
            w.writerow([s["id"], s["note"], s["iso"], f"{s['dur']:.0f}", s["sweeps"],
                        f"{s['lat']:.7f}" if s["lat"] is not None else "", f"{s['lon']:.7f}" if s["lon"] is not None else "",
                        f"{s['floor']:.1f}", f"{s['median']:.1f}", f"{s['peak']:.1f}", s["peak_mhz"], s["busy"],
                        int(s["early"])] + [f"{v:.1f}" for v in s.get("mean", [])])


def write_kml(path, pts, marks, lo, hi, stations=()):
    out = ['<?xml version="1.0" encoding="UTF-8"?>',
           '<kml xmlns="http://www.opengis.net/kml/2.2"><Document><name>RF survey 902-928 MHz</name>']
    for p in pts:
        r, g, b = colour(p["floor"], lo, hi)
        kcol = f"ff{b:02x}{g:02x}{r:02x}"
        out.append(f'<Placemark><name>{p["floor"]:.1f}</name>'
                   f'<description>{p["iso"]}  floor {p["floor"]} dBm, median {p["median"]}, '
                   f'peak {p["peak"]} @ {p["peak_mhz"]} MHz, busy {p["busy"]}</description>'
                   f'<Style><IconStyle><color>{kcol}</color><scale>0.6</scale>'
                   f'<Icon><href>http://maps.google.com/mapfiles/kml/shapes/shaded_dot.png</href></Icon>'
                   f'</IconStyle><LabelStyle><scale>0</scale></LabelStyle></Style>'
                   f'<Point><coordinates>{p["lon"]:.7f},{p["lat"]:.7f},0</coordinates></Point></Placemark>')
    for m in marks:
        if m.get("lat") is None:
            continue
        out.append(f'<Placemark><name>MARK {m["note"]}</name><Point>'
                   f'<coordinates>{m["lon"]:.7f},{m["lat"]:.7f},0</coordinates></Point></Placemark>')
    for st in stations:
        if st["lat"] is None:
            continue
        r, g, b = colour(st["floor"], lo, hi)
        out.append(f'<Placemark><name>STN {st["id"]} {st["note"]} ({st["floor"]:.1f})</name>'
                   f'<description>{st["iso"]}  {st["dur"]:.0f}s baseline: floor {st["floor"]:.1f} dBm, median {st["median"]:.1f}, '
                   f'peak {st["peak"]:.1f} @ {st["peak_mhz"]} MHz, busy {st["busy"]}</description>'
                   f'<Style><IconStyle><color>ff{b:02x}{g:02x}{r:02x}</color><scale>1.4</scale>'
                   f'<Icon><href>http://maps.google.com/mapfiles/kml/shapes/square.png</href></Icon></IconStyle></Style>'
                   f'<Point><coordinates>{st["lon"]:.7f},{st["lat"]:.7f},0</coordinates></Point></Placemark>')
    out.append("</Document></kml>")
    with open(path, "w") as fh:
        fh.write("\n".join(out))


def write_map(path, pts, marks, lo, hi, freqs, stations=()):
    data = [{"lat": p["lat"], "lon": p["lon"], "f": p["floor"], "m": p["median"], "p": p["peak"],
             "pf": p["peak_mhz"], "b": p["busy"], "t": p["iso"], "c": "#%02x%02x%02x" % colour(p["floor"], lo, hi)}
            for p in pts]
    mk = [{"lat": m["lat"], "lon": m["lon"], "n": m["note"], "t": m.get("iso", "")} for m in marks if m.get("lat") is not None]
    sn = [{"lat": s["lat"], "lon": s["lon"], "id": s["id"], "n": s["note"], "t": s["iso"], "d": round(s["dur"]),
           "f": round(s["floor"], 1), "m": round(s["median"], 1), "p": round(s["peak"], 1), "pf": s["peak_mhz"],
           "b": s["busy"], "c": "#%02x%02x%02x" % colour(s["floor"], lo, hi)} for s in stations if s["lat"] is not None]
    html = f"""<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>RF survey 902-928 MHz</title>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css">
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<style>html,body,#map{{height:100%;margin:0}}
.legend{{background:#fff;padding:8px;font:12px sans-serif;border-radius:6px;line-height:1.5}}</style></head>
<body><div id="map"></div><script>
const pts={json.dumps(data)}, marks={json.dumps(mk)}, stns={json.dumps(sn)};
const map=L.map('map');
L.tileLayer('https://{{s}}.tile.openstreetmap.org/{{z}}/{{x}}/{{y}}.png',{{maxZoom:19,attribution:'&copy; OpenStreetMap'}}).addTo(map);
const g=L.featureGroup().addTo(map);
pts.forEach(p=>L.circleMarker([p.lat,p.lon],{{radius:6,color:p.c,fillColor:p.c,fillOpacity:.85,weight:1}})
 .bindPopup(`<b>${{p.t}}</b><br>Floor (P20): ${{p.f}} dBm<br>Median: ${{p.m}} dBm<br>Peak: ${{p.p}} dBm @ ${{p.pf}} MHz<br>Busy ch: ${{p.b}}`).addTo(g));
marks.forEach(m=>L.marker([m.lat,m.lon]).bindPopup(`<b>MARK</b> ${{m.n}}<br>${{m.t}}`).addTo(g));
stns.forEach(s=>L.marker([s.lat,s.lon],{{icon:L.divIcon({{className:'',iconSize:[38,22],iconAnchor:[19,11],
 html:`<div style="background:${{s.c}};border:2px solid #111;border-radius:5px;font:bold 11px sans-serif;text-align:center;line-height:18px">${{s.f}}</div>`}})}})
 .bindPopup(`<b>Station ${{s.id}}</b> ${{s.n}}<br>${{s.t}} · ${{s.d}} s baseline<br>Floor (P20): ${{s.f}} dBm<br>Median: ${{s.m}} dBm<br>Peak: ${{s.p}} dBm @ ${{s.pf}} MHz<br>Busy ch: ${{s.b}}`).addTo(g));
if(pts.length||stns.length) map.fitBounds(g.getBounds().pad(0.1)); else map.setView([39.8,-98.6],4);
const lg=L.control({{position:'bottomright'}});
lg.onAdd=()=>{{const d=L.DomUtil.create('div','legend');
 d.innerHTML='<b>Noise floor (P20)</b><br><span style="color:#28ff28">&#9679;</span> {lo:.1f} dBm (quiet)<br>'+
 '<span style="color:#ffff28">&#9679;</span> {(lo+hi)/2:.1f}<br><span style="color:#ff0028">&#9679;</span> {hi:.1f} dBm (noisy)<br>'+
 '<small>{freqs[0]:.1f}–{freqs[-1]:.1f} MHz, {len(freqs)} ch. Relative, uncalibrated.</small>';return d}};
lg.addTo(map);
</script></body></html>"""
    with open(path, "w") as fh:
        fh.write(html)


def make_plots(outdir, rows, freqs, marks, stations=()):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import matplotlib.dates as mdates
    except ImportError:
        print("matplotlib not installed — skipping PNG plots (pip install matplotlib)")
        return
    st_ok = [s for s in stations if "mean" in s and len(s["mean"]) == len(freqs)]
    if st_ok:
        fig, ax = plt.subplots(figsize=(11, 4.5))
        for s in st_ok:
            ax.plot(freqs, s["mean"], lw=1.2, label=f"STN {s['id']} {s['note'][:18]} ({s['floor']:.1f})")
        ax.set_xlabel("MHz"); ax.set_ylabel("dBm (uncalibrated)"); ax.grid(alpha=.3)
        ax.set_title("GO station baselines — mean spectrum per stop"); ax.legend(fontsize=7, ncol=2)
        fig.tight_layout(); fig.savefig(os.path.join(outdir, "stations.png"), dpi=130); plt.close(fig)
    timed = [r for r in rows if r["_t"] is not None]
    use = timed if len(timed) >= 2 else rows
    keys = [f"f{f:.1f}" for f in freqs]
    spec = [[float(r[k]) for k in keys] for r in use]
    if not spec:
        return

    avg = [10 * math.log10(sum(10 ** (s[i] / 10) for s in spec) / len(spec)) for i in range(len(freqs))]
    mx = [max(s[i] for s in spec) for i in range(len(freqs))]
    mn = [min(s[i] for s in spec) for i in range(len(freqs))]
    fig, ax = plt.subplots(figsize=(11, 4.5))
    ax.plot(freqs, mx, color="#d1495b", lw=1, label="max hold (window avg)")
    ax.plot(freqs, avg, color="#2e86ab", lw=1.6, label="power average")
    ax.plot(freqs, mn, color="#6c757d", lw=1, label="minimum")
    ax.set_xlabel("MHz"); ax.set_ylabel("dBm (uncalibrated)")
    ax.set_title("902–928 MHz spectrum over the survey"); ax.grid(alpha=.3); ax.legend()
    fig.tight_layout(); fig.savefig(os.path.join(outdir, "spectrum.png"), dpi=130); plt.close(fig)

    if use is timed:
        ts = [datetime.fromtimestamp(r["_t"], timezone.utc) for r in use]
        x0, x1 = mdates.date2num(ts[0]), mdates.date2num(ts[-1])
    else:
        ts = list(range(len(use))); x0, x1 = 0, len(use)
    fig, ax = plt.subplots(figsize=(11, 5))
    im = ax.imshow(list(zip(*spec)), aspect="auto", origin="lower", cmap="viridis",
                   extent=[x0, x1, freqs[0], freqs[-1]])
    if use is timed:
        ax.xaxis_date(); ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
        ax.set_xlabel("time (UTC)")
    else:
        ax.set_xlabel("row #")
    ax.set_ylabel("MHz"); ax.set_title("Waterfall")
    fig.colorbar(im, ax=ax, label="dBm")
    fig.tight_layout(); fig.savefig(os.path.join(outdir, "waterfall.png"), dpi=130); plt.close(fig)

    fig, ax = plt.subplots(figsize=(11, 4))
    ax.plot(ts, [float(r["peak_dbm"]) for r in use], color="#d1495b", lw=.8, label="peak (raw)")
    ax.plot(ts, [float(r["median_dbm"]) for r in use], color="#edae49", lw=1.2, label="median")
    ax.plot(ts, [float(r["floor_p20_dbm"]) for r in use], color="#2e86ab", lw=1.6, label="floor P20")
    if use is timed:
        for m in marks:
            if m["_t"]:
                mt = datetime.fromtimestamp(m["_t"], timezone.utc)
                ax.axvline(mt, color="k", ls=":", lw=.8)
                ax.text(mt, ax.get_ylim()[1], m["note"] or "mark", rotation=90, va="top", fontsize=7)
        ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M")); ax.set_xlabel("time (UTC)")
    ax.set_ylabel("dBm"); ax.grid(alpha=.3); ax.legend(loc="lower right")
    ax.set_title("Noise floor over time")
    fig.tight_layout(); fig.savefig(os.path.join(outdir, "timeline.png"), dpi=130); plt.close(fig)


# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--gpx", help="GPX track recorded on the iPhone during the survey")
    ap.add_argument("--out", default="results")
    ap.add_argument("--max-gap", type=float, default=30.0)
    ap.add_argument("--time-shift", type=float, default=0.0)
    ap.add_argument("--antenna-gain", type=float, default=0.0,
                    help="dBi to subtract from readings (default 0 = raw values)")
    a = ap.parse_args()

    header, freqs, rows, syncs, marks, stations = parse_log(a.log)
    resolve_times(rows, syncs, marks, a.time_shift, stations)
    if a.antenna_gain:
        for r in rows:
            for k in ["floor_p20_dbm", "median_dbm", "peak_dbm"] + [f"f{f:.1f}" for f in freqs]:
                r[k] = f"{float(r[k]) - a.antenna_gain:.1f}"
        for st in stations:
            for k in ("floor", "median", "peak"):
                st[k] -= a.antenna_gain
            for k in ("mean", "min", "max"):
                if k in st:
                    st[k] = [v - a.antenna_gain for v in st[k]]
    track = parse_gpx(a.gpx) if a.gpx else []

    print(f"log rows: {len(rows)}   channels: {len(freqs)} ({freqs[0]}-{freqs[-1]} MHz)")
    print(f"rows with time: {sum(r['_t'] is not None for r in rows)}   marks: {len(marks)}")
    if a.gpx:
        print(f"GPX points: {len(track)}" + (
            f"  ({datetime.fromtimestamp(track[0][0], timezone.utc):%Y-%m-%d %H:%M:%S} → "
            f"{datetime.fromtimestamp(track[-1][0], timezone.utc):%H:%M:%S} UTC)" if track else ""))

    pts = []
    for r in rows:
        pos, src = None, ""
        if r.get("lat"):
            pos, src = (float(r["lat"]), float(r["lon"])), "gps-module"
        elif track:
            pos = interp(track, r["_t"], a.max_gap)
            src = "gpx" if pos else ""
        if not pos:
            continue
        iso = datetime.fromtimestamp(r["_t"], timezone.utc).strftime("%Y-%m-%d %H:%M:%SZ") if r["_t"] else ""
        pts.append({"lat": pos[0], "lon": pos[1], "iso": iso, "src": src,
                    "floor": float(r["floor_p20_dbm"]), "median": float(r["median_dbm"]),
                    "peak": float(r["peak_dbm"]), "peak_mhz": float(r["peak_mhz"]), "busy": int(r["busy"])})
    for m in marks:
        pos = interp(track, m["_t"], a.max_gap) if track else None
        if pos is None and m["_t"]:   # fall back to nearest geotagged row
            near = [r for r in rows if r.get("lat") and r["_t"]]
            if near:
                rr = min(near, key=lambda r: abs(r["_t"] - m["_t"]))
                pos = (float(rr["lat"]), float(rr["lon"]))
        m["lat"], m["lon"] = (pos if pos else (None, None))
        m["iso"] = datetime.fromtimestamp(m["_t"], timezone.utc).strftime("%H:%M:%SZ") if m["_t"] else ""

    for st in stations:
        st["lat"] = st["lon"] = None
        t0, t1 = st.get("_t0"), st.get("_t1")
        cand = []
        if track and t0 is not None and t1 is not None:
            cand = [(la, lo) for (t, la, lo) in track if t0 - 5 <= t <= t1 + 5]
            if not cand:
                p = interp(track, (t0 + t1) / 2, a.max_gap)
                cand = [p] if p else []
        if not cand and t0 is not None:
            cand = [(float(r["lat"]), float(r["lon"])) for r in rows
                    if r.get("lat") and r["_t"] is not None and t0 - 5 <= r["_t"] <= t1 + 5]
        if cand:   # median position over the dwell
            st["lat"] = sorted(c[0] for c in cand)[len(cand) // 2]
            st["lon"] = sorted(c[1] for c in cand)[len(cand) // 2]
        st["iso"] = datetime.fromtimestamp(t0, timezone.utc).strftime("%Y-%m-%d %H:%M:%SZ") if t0 else ""
        st["dur"] = (st["end_ms"] - st["start_ms"]) / 1000.0

    print(f"geotagged points: {len(pts)}")
    if stations:
        print(f"GO stations: {len(stations)} ({sum(s['lat'] is not None for s in stations)} placed)")
        for st in stations:
            where = f"{st['lat']:.5f},{st['lon']:.5f}" if st["lat"] is not None else "no position"
            print(f"   STN {st['id']:>3} {st['note'][:24]:<24} floor {st['floor']:6.1f}  median {st['median']:6.1f}  "
                  f"peak {st['peak']:6.1f} @ {st['peak_mhz']} MHz  busy {st['busy']:>3}  {st['dur']:.0f}s  {where}")
    if not pts:
        print("\nNo rows could be placed on the map. Check that:\n"
              "  - the phone page was opened at least once (clock sync), or a GPS module was attached\n"
              "  - the GPX track covers the same time span (try --time-shift if clocks differ)")
    os.makedirs(a.out, exist_ok=True)
    if stations:
        write_stations_csv(os.path.join(a.out, "stations.csv"), stations, freqs)

    if pts or any(s["lat"] is not None for s in stations):
        floors = sorted([p["floor"] for p in pts] + [s["floor"] for s in stations if s["lat"] is not None])
        lo = floors[int(0.05 * (len(floors) - 1))]
        hi = floors[int(0.95 * (len(floors) - 1))]
        if hi - lo < 3:
            hi = lo + 3
        write_points_csv(os.path.join(a.out, "points.csv"), pts)
        write_kml(os.path.join(a.out, "survey.kml"), pts, marks, lo, hi, stations)
        write_map(os.path.join(a.out, "map.html"), pts, marks, lo, hi, freqs, stations)
        print(f"floor range: {floors[0]:.1f} .. {floors[-1]:.1f} dBm  (colour scale {lo:.1f} .. {hi:.1f})")
        worst = sorted(pts, key=lambda p: p["floor"], reverse=True)[:5]
        print("noisiest drive spots:" if worst else "")
        for p in worst:
            print(f"   {p['floor']:6.1f} dBm  {p['lat']:.5f},{p['lon']:.5f}  {p['iso']}  peak {p['peak']} @ {p['peak_mhz']} MHz")
    make_plots(a.out, rows, freqs, marks, stations)
    print(f"\nwrote results to {os.path.abspath(a.out)}/")


if __name__ == "__main__":
    main()
