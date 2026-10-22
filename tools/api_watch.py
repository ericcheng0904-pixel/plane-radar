#!/usr/bin/env python3
"""
Check, once, that every service the radar depends on still answers the way the
radar's own code expects. Run daily by .github/workflows/api-watch.yml, which
opens an issue when something stops being "ok"; run it by hand with

    python3 tools/api_watch.py [--report FILE] [--json FILE] [SERVICE ...]

Each service is asked the way the radar asks (src/services/*.cpp), with the
User-Agent below and at most two requests:

    adsb.fi               GET  opendata.adsb.fi/api/v3/lat/../lon/../dist/10
    adsb.lol              GET  api.adsb.lol/v2/point/../../10
    adsb.im routes        POST adsb.im/api/0/routeset (plain HTTP, then HTTPS
                          if it redirects: routes.cpp does the same)
    Open-Meteo geocoding  GET  geocoding-api.open-meteo.com/v1/search?name=..
    GitHub Pages          GET  ota/manifest.json, HEAD ota/firmware-<version>.bin
    Certificates          scripts/check_hosts_against_bundle.py (exit code)

and sorted into

    ok           the answer has everything the radar reads
    changed      it answered, but with another status or another shape
    unreachable  no answer (DNS, connect, timeout, TLS) or a 5xx
    unknown      only on a GitHub runner: the service refused or rate-limited
                 the runner (403, 429, 451, a Cloudflare challenge). Those
                 addresses are shared datacenter addresses a home radar is not
                 behind, so this says nothing about the radar. It is shown with
                 its evidence and never opens an issue.

A table is printed, and a Markdown report written to --report (default
api-watch-report.md in the current directory; it is appended to the job summary
on GitHub too). --json writes the same results for the workflow. The exit
status is 1 if any service is "changed" or "unreachable".

Standard library only, except that the certificate check needs the
'cryptography' package and openssl (scripts/requirements-release.txt).
Everything a server sends back is untrusted text: it is shortened and stripped
of control characters, "::" (so it cannot become a workflow command), "@" (so it
cannot mention anyone) and code fences.
"""

from __future__ import annotations

import argparse
import base64
import binascii
import datetime
import json
import math
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.dont_write_bytecode = True

REPO_ROOT = Path(__file__).resolve().parent.parent

USER_AGENT = "PlaneRadar/watch (+https://github.com/ericcheng0904-pixel/plane-radar)"
TIMEOUT_S = 20
MAX_BODY_BYTES = 4 * 1024 * 1024
BODY_SNIPPET_CHARS = 1500

# The radar's centre of the world for this check: O'Hare, always busy.
LAT, LON = 41.9786, -87.9048
# The radius asked of the feeds below, and how far out the radar still takes
# an aircraft as part of the answer (util::feed_answer::kReachFactor).
FEED_RADIUS_NM = 10
REACH_FACTOR = 1.5

PAGES_OTA = "https://ericcheng0904-pixel.github.io/plane-radar/ota/"
ROUTES_HTTP = "http://adsb.im/api/0/routeset"
ROUTES_HTTPS = "https://adsb.im/api/0/routeset"
# Byte for byte what routes.cpp builds for one flight (%.4f, no spaces).
ROUTES_BODY = '{"planes":[{"callsign":"UAL1","lat":%.4f,"lng":%.4f}]}' % (LAT, LON)

# docs/OTA.md: the radar reads at most 1024 bytes of manifest, and an image must
# fit an app slot (0x1C0000).
MANIFEST_MAX_BYTES = 1024
APP_SLOT_BYTES = 0x1C0000
RELEASE_RE = re.compile(r"v(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})")
SHA256_RE = re.compile(r"[0-9a-f]{64}")

OK, CHANGED, UNREACHABLE, UNKNOWN = "ok", "changed", "unreachable", "unknown"
LABEL = {OK: "ok", CHANGED: "changed", UNREACHABLE: "unreachable", UNKNOWN: "unknown from runner"}
REDIRECTS = (301, 302, 307, 308)

ON_RUNNER = os.environ.get("GITHUB_ACTIONS") == "true"

# Headers worth keeping as evidence of who answered and why.
EVIDENCE_HEADERS = ("server", "content-type", "content-length", "content-encoding", "location",
                    "retry-after", "via", "cf-ray", "cf-mitigated", "x-cache", "x-github-request-id")
EVIDENCE_HEADER_PREFIXES = ("x-ratelimit", "ratelimit")


# --- Untrusted text -----------------------------------------------------------------

def clean(text, limit=200):
    """Text from a server or an exception, made safe to print and quote."""
    text = "".join(c if c >= " " and c != "\x7f" else " " for c in str(text))
    text = " ".join(text.split()).replace("::", ": :").replace("```", "'''").replace("@", "(at)")
    return text if len(text) <= limit else text[:limit - 1] + "..."


# --- One request ----------------------------------------------------------------------

class Exchange:
    """A request and what came back, kept as evidence."""

    def __init__(self, method, url, body=None):
        self.method = method
        self.url = url
        self.request_body = body
        self.status = None
        self.headers = {}
        self.body = b""
        self.error = None
        self.elapsed = 0.0

    def header(self, name):
        return self.headers.get(name.lower())

    def json(self):
        """(document, problem)."""
        try:
            return json.loads(self.body.decode("utf-8")), None
        except (ValueError, UnicodeDecodeError) as err:
            return None, "the body is not JSON (%s)" % clean(err, 80)


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """Report a redirect instead of following it: the radar decides what to do with one."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


_OPENER = urllib.request.build_opener(_NoRedirect)


def request(method, url, body=None, content_type=None):
    ex = Exchange(method, url, body)
    req = urllib.request.Request(url, data=body.encode("utf-8") if body is not None else None, method=method)
    req.add_header("User-Agent", USER_AGENT)
    if content_type:
        req.add_header("Content-Type", content_type)
    started = time.monotonic()
    try:
        with _OPENER.open(req, timeout=TIMEOUT_S) as resp:
            ex.status = resp.status
            ex.headers = {k.lower(): v for k, v in resp.headers.items()}
            if method != "HEAD":
                ex.body = resp.read(MAX_BODY_BYTES + 1)
    except urllib.error.HTTPError as err:  # 3xx (not followed), 4xx, 5xx
        ex.status = err.code
        ex.headers = {k.lower(): v for k, v in err.headers.items()}
        try:
            ex.body = err.read(MAX_BODY_BYTES + 1) if method != "HEAD" else b""
        except Exception:  # noqa: BLE001 - the status is what matters
            ex.body = b""
    except Exception as err:  # noqa: BLE001 - DNS, connect, TLS, timeout, reset, short read
        reason = getattr(err, "reason", None) or err
        ex.error = "%s: %s" % (type(reason).__name__, clean(reason, 120))
    ex.elapsed = time.monotonic() - started
    return ex


# --- Results ---------------------------------------------------------------------------

class Result:
    def __init__(self, name):
        self.name = name
        self.status = OK
        self.summary = ""
        self.notes = []
        self.exchanges = []
        self.requests = 0
        self.output = ""  # a checker's own text output, kept as evidence

    def ask(self, *args, **kwargs):
        ex = request(*args, **kwargs)
        self.exchanges.append(ex)
        self.requests += 1
        return ex

    def verdict(self, status, summary):
        self.status = status
        self.summary = summary
        return self


def looks_blocked(ex):
    """The service turned this address away (403, rate limit, 451, a Cloudflare challenge)."""
    return ex.status in (403, 429, 451) or ex.header("cf-mitigated") is not None


def bad_answer(res, ex, expected=200):
    """
    Give `res` its verdict and return True if `ex` is not the expected answer.
    On a GitHub runner a refusal is "unknown": see the module docstring.
    """
    if ex.error:
        res.verdict(UNREACHABLE, "no answer: %s" % ex.error)
        return True
    if ex.status == expected:
        return False
    detail = "HTTP %s instead of %s" % (ex.status, expected)
    if ex.header("location"):
        detail += " (Location: %s)" % clean(ex.header("location"), 100)
    if ON_RUNNER and looks_blocked(ex):
        res.verdict(UNKNOWN, detail + " -- refused or rate-limited from this GitHub runner; "
                    "a home connection may be answered normally")
    elif ex.status is not None and ex.status >= 500:
        res.verdict(UNREACHABLE, detail)
    else:
        res.verdict(CHANGED, detail)
    return True


def is_number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


# --- The services -----------------------------------------------------------------------

def distance_km(lat1, lon1, lat2, lon2):
    """Great-circle distance, as util::feed_answer::Area does it."""
    p1, p2 = math.radians(lat1), math.radians(lat2)
    a = (math.sin((p2 - p1) / 2) ** 2
         + math.cos(p1) * math.cos(p2) * math.sin(math.radians(lon2 - lon1) / 2) ** 2)
    return 2 * 6371.0 * math.asin(min(1.0, math.sqrt(a)))


def check_feed(name, url, radius_nm=FEED_RADIUS_NM):
    """
    An ADS-B feed. The radar (adsb_client.cpp) needs a JSON object with an "ac"
    list, skips any aircraft without numeric lat/lon, and reads "now". It counts
    an answer whose positioned aircraft lie mostly beyond 1.5 radii as the feed
    failing (util::feed_answer::kReachFactor): a feed that starts ignoring the
    radius is reported here before the radar keeps handing over to the other.
    """
    res = Result(name)
    ex = res.ask("GET", url)
    if bad_answer(res, ex):
        return res
    doc, problem = ex.json()
    problems = [problem] if problem else []
    count = 0
    if not problem:
        if not isinstance(doc, dict):
            problems.append("the answer is not a JSON object")
        else:
            aircraft = doc.get("ac")
            if not isinstance(aircraft, list):
                problems.append('there is no "ac" list')
            else:
                count = len(aircraft)
                lacking = sum(1 for a in aircraft
                              if not (isinstance(a, dict) and is_number(a.get("lat")) and is_number(a.get("lon"))))
                if lacking:
                    problems.append("%d of %d aircraft have no numeric lat/lon" % (lacking, count))
                reach_km = REACH_FACTOR * radius_nm * 1.852
                placed = [a for a in aircraft
                          if isinstance(a, dict) and is_number(a.get("lat")) and is_number(a.get("lon"))]
                near = sum(1 for a in placed if distance_km(LAT, LON, a["lat"], a["lon"]) <= reach_km)
                if near * 2 < len(placed):
                    problems.append("only %d of %d aircraft lie within %.0f km: the radius is ignored"
                                    % (near, len(placed), reach_km))
            if not is_number(doc.get("now")):
                problems.append('there is no numeric "now"')
    if problems:
        return res.verdict(CHANGED, "HTTP 200 but " + "; ".join(problems))
    return res.verdict(OK, "HTTP 200, %d aircraft, %.1f s" % (count, ex.elapsed))


def check_routes():
    """
    adsb.im's route lookup. routes.cpp posts to plain HTTP and, if that
    redirects, to HTTPS from then on; it keeps callsign, _airport_codes_iata
    and plausible from each item of the answering list.
    """
    res = Result("adsb.im routes")
    ex = res.ask("POST", ROUTES_HTTP, ROUTES_BODY, "application/json")
    if ex.status in REDIRECTS:
        res.notes.append("plain HTTP answers %s; the radar follows by posting to %s" % (ex.status, ROUTES_HTTPS))
        ex = res.ask("POST", ROUTES_HTTPS, ROUTES_BODY, "application/json")
    if bad_answer(res, ex):
        return res
    doc, problem = ex.json()
    problems = [problem] if problem else []
    if not problem:
        if not isinstance(doc, list):
            problems.append("the answer is not a JSON list")
        elif not doc:
            problems.append("the list is empty for UAL1")
        else:
            for index, item in enumerate(doc):
                if not isinstance(item, dict):
                    problems.append("item %d is not an object" % index)
                    continue
                for key, kind in (("callsign", str), ("_airport_codes_iata", str), ("plausible", bool)):
                    if not isinstance(item.get(key), kind):
                        problems.append('item %d has no %s "%s"' % (index, kind.__name__, key))
    if problems:
        return res.verdict(CHANGED, "HTTP 200 but " + "; ".join(problems[:4]))
    via = "HTTPS (after a redirect)" if ex.url == ROUTES_HTTPS else "HTTP"
    return res.verdict(OK, "%s 200, %d route(s), %.1f s" % (via, len(doc), ex.elapsed))


def check_geocoding():
    """Open-Meteo's town search. geocode.cpp reads results[].name, latitude, longitude and more."""
    res = Result("Open-Meteo geocoding")
    url = "https://geocoding-api.open-meteo.com/v1/search?name=Evanston&count=3&language=en&format=json"
    ex = res.ask("GET", url)
    if bad_answer(res, ex):
        return res
    doc, problem = ex.json()
    problems = [problem] if problem else []
    count = 0
    if not problem:
        results = doc.get("results") if isinstance(doc, dict) else None
        if not isinstance(results, list) or not results:
            problems.append('there is no non-empty "results" list')
        else:
            count = len(results)
            lacking = sum(1 for r in results
                          if not (isinstance(r, dict) and isinstance(r.get("name"), str)
                                  and is_number(r.get("latitude")) and is_number(r.get("longitude"))))
            if lacking:
                problems.append("%d of %d results lack name/latitude/longitude" % (lacking, count))
    if problems:
        return res.verdict(CHANGED, "HTTP 200 but " + "; ".join(problems))
    return res.verdict(OK, "HTTP 200, %d result(s), %.1f s" % (count, ex.elapsed))


def manifest_problems(manifest, ex):
    """What the updater (ota.cpp fetchManifest, docs/OTA.md) would refuse in the manifest."""
    problems = []
    length = ex.header("content-length")
    if length is None or not length.isdigit() or not 0 < int(length) <= MANIFEST_MAX_BYTES:
        problems.append("Content-Length is %s (the radar needs it present, 1..%d)"
                        % (clean(length, 20) if length is not None else "missing", MANIFEST_MAX_BYTES))
    if len(ex.body) > MANIFEST_MAX_BYTES:
        problems.append("the manifest is %d bytes, more than the %d the radar reads" % (len(ex.body), MANIFEST_MAX_BYTES))
    if not isinstance(manifest, dict):
        return problems + ["the manifest is not a JSON object"]
    version = manifest.get("version")
    if not isinstance(version, str) or not RELEASE_RE.fullmatch(version):
        problems.append('"version" is not a release (vMAJOR.MINOR.PATCH)')
    size = manifest.get("size")
    if not is_number(size) or int(size) != size or not 0 < size <= APP_SLOT_BYTES:
        problems.append('"size" is not an integer in 1..%d' % APP_SLOT_BYTES)
    sha = manifest.get("sha256")
    if not isinstance(sha, str) or not SHA256_RE.fullmatch(sha):
        problems.append('"sha256" is not 64 lowercase hex digits')
    sig = manifest.get("sig")
    try:
        raw = base64.b64decode(sig, validate=True) if isinstance(sig, str) else b""
    except (binascii.Error, ValueError):
        raw = b""
    if not 8 <= len(raw) <= 72:
        problems.append('"sig" is not base64 of 8..72 bytes')
    return problems


def check_pages():
    """The update manifest and the image it names, as ota.cpp fetches them."""
    res = Result("GitHub Pages")
    ex = res.ask("GET", PAGES_OTA + "manifest.json")
    if bad_answer(res, ex):
        return res
    manifest, problem = ex.json()
    problems = [problem] if problem else manifest_problems(manifest, ex)
    if problems:
        return res.verdict(CHANGED, "manifest.json: " + "; ".join(problems))

    version, size = manifest["version"], int(manifest["size"])
    image = res.ask("HEAD", "%sfirmware-%s.bin" % (PAGES_OTA, version))
    if bad_answer(res, image):
        res.summary = "firmware-%s.bin: %s" % (version, res.summary)
        return res
    length = image.header("content-length")
    if length is None or not length.isdigit() or int(length) != size:
        return res.verdict(CHANGED, "firmware-%s.bin: Content-Length is %s, the manifest says size %d"
                           % (version, clean(length, 20) if length is not None else "missing", size))
    return res.verdict(OK, "%s, %d bytes, %.1f s" % (version, size, ex.elapsed + image.elapsed))


def check_certificates():
    """Whether every server still ends its chain in a root the radar's bundle has."""
    res = Result("Certificates")
    script = REPO_ROOT / "scripts" / "check_hosts_against_bundle.py"
    started = time.monotonic()
    try:
        proc = subprocess.run([sys.executable, str(script)], cwd=str(REPO_ROOT), capture_output=True,
                              text=True, timeout=300)
    except subprocess.TimeoutExpired:
        return res.verdict(UNREACHABLE, "the certificate check did not finish in 300 s")
    except OSError as err:
        return res.verdict(CHANGED, "cannot run the certificate check: %s" % clean(err, 100))
    output = (proc.stdout or "") + (proc.stderr or "")
    res.notes.append("`scripts/check_hosts_against_bundle.py` exited %d after %.0f s" % (proc.returncode, time.monotonic() - started))
    res.output = output
    lines = [l for l in (proc.stdout or "").splitlines() if l.startswith(("ok ", "FAIL "))]
    failed = [l for l in lines if l.startswith("FAIL ")]
    if proc.returncode == 0:
        return res.verdict(OK, "%d hosts verify against the bundle" % len(lines))
    if proc.returncode == 1 and failed:
        silent = all("no certificate chain" in l for l in failed)
        names = ", ".join(clean(l.split(":")[0][5:], 60) for l in failed)
        if silent:
            return res.verdict(UNREACHABLE, "no certificate chain from %s" % names)
        return res.verdict(CHANGED, "the radar would reject: %s" % names)
    tail = " ".join(output.split()[-12:])
    return res.verdict(CHANGED, "the certificate check failed to run (exit %d): %s" % (proc.returncode, clean(tail, 150)))


SERVICES = (
    ("adsb.fi", lambda: check_feed(
        "adsb.fi", "https://opendata.adsb.fi/api/v3/lat/%.4f/lon/%.4f/dist/%d" % (LAT, LON, FEED_RADIUS_NM))),
    ("adsb.lol", lambda: check_feed(
        "adsb.lol", "https://api.adsb.lol/v2/point/%.4f/%.4f/%d" % (LAT, LON, FEED_RADIUS_NM))),
    ("adsb.im routes", check_routes),
    ("Open-Meteo geocoding", check_geocoding),
    ("GitHub Pages", check_pages),
    ("Certificates", check_certificates),
)


# --- Reports ----------------------------------------------------------------------------

def evidence_md(ex):
    lines = ["`%s %s`" % (ex.method, ex.url)]
    if ex.request_body:
        lines.append("")
        lines += ["Request body:", "```", clean(ex.request_body, BODY_SNIPPET_CHARS), "```"]
    if ex.error:
        lines += ["", "No answer after %.1f s: `%s`" % (ex.elapsed, clean(ex.error, 160))]
        return lines
    lines += ["", "Answer: **HTTP %s** after %.1f s." % (ex.status, ex.elapsed)]
    kept = [(k, v) for k, v in sorted(ex.headers.items())
            if k in EVIDENCE_HEADERS or k.startswith(EVIDENCE_HEADER_PREFIXES)]
    if kept:
        lines += ["", "```"] + ["%s: %s" % (k, clean(v, 120)) for k, v in kept] + ["```"]
    if ex.body:
        lines += ["", "Start of the body:", "```", clean(ex.body.decode("utf-8", "replace"), BODY_SNIPPET_CHARS), "```"]
    return lines


def details_md(res):
    """The section of the report about one service that is not ok."""
    lines = [res.summary + ".", ""] if res.summary else []
    lines += ["- " + note for note in res.notes]
    if res.notes:
        lines.append("")
    for ex in res.exchanges:
        lines += evidence_md(ex) + [""]
    if res.output:
        shown = [clean(line, 200) for line in res.output.splitlines() if line.strip()][:30]
        lines += ["Output:", "```"] + shown + ["```", ""]
    return "\n".join(lines).rstrip() + "\n"


def table_md(results):
    rows = ["| Service | Status | Detail |", "|---|---|---|"]
    for r in results:
        status = LABEL[r.status] if r.status == OK else "**%s**" % LABEL[r.status]
        rows.append("| %s | %s | %s |" % (r.name, status, clean(r.summary, 220).replace("|", "/")))
    return "\n".join(rows)


def table_text(results):
    width = max(len(r.name) for r in results)
    lines = ["%-*s  %-19s  %s" % (width, "Service", "Status", "Detail")]
    lines += ["%-*s  %-19s  %s" % (width, r.name, LABEL[r.status], clean(r.summary, 220)) for r in results]
    return "\n".join(lines)


def run_url():
    if os.environ.get("GITHUB_RUN_ID") and os.environ.get("GITHUB_REPOSITORY"):
        return "%s/%s/actions/runs/%s" % (os.environ.get("GITHUB_SERVER_URL", "https://github.com"),
                                          os.environ["GITHUB_REPOSITORY"], os.environ["GITHUB_RUN_ID"])
    return None


def report_md(results, when):
    where = "a GitHub Actions runner" if ON_RUNNER else "a local machine"
    url = run_url()
    lines = ["# API watch", "",
             "Checked %s from %s%s." % (when, where, " ([run](%s))" % url if url else ""), "",
             table_md(results), ""]
    for r in results:
        if r.status != OK:
            lines += ["## %s: %s" % (r.name, LABEL[r.status]), "", details_md(r)]
    if any(r.status == UNKNOWN for r in results):
        lines += ["*unknown from runner* means the service refused or limited this GitHub runner, whose "
                  "datacenter address a home radar does not share. It says nothing about the radar and "
                  "opens no issue.", ""]
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Check the services the radar depends on (tools/api_watch.py).")
    parser.add_argument("only", nargs="*", help="check only services whose name contains one of these")
    parser.add_argument("--report", default="api-watch-report.md", help="Markdown report (default: %(default)s)")
    parser.add_argument("--json", metavar="FILE", help="also write the results as JSON, for the workflow")
    args = parser.parse_args(argv)

    wanted = [w.lower() for w in args.only]
    results = []
    for name, check in SERVICES:
        if wanted and not any(w in name.lower() for w in wanted):
            continue
        try:
            result = check()
        except Exception as err:  # noqa: BLE001 - a bug in a check must not hide the other services
            result = Result(name).verdict(CHANGED, "the check itself failed: %s: %s" % (type(err).__name__, clean(err, 120)))
        results.append(result)
    if not results:
        parser.error("no service matches %s" % ", ".join(args.only))

    when = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    print(table_text(results))
    report = report_md(results, when)
    Path(args.report).write_text(report, encoding="utf-8")
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as summary:
            summary.write(report + "\n")
    if args.json:
        document = {
            "checked": when,
            "runner": ON_RUNNER,
            "table": table_md(results),
            "services": [{"name": r.name, "status": r.status, "label": LABEL[r.status], "summary": r.summary,
                          "requests": r.requests, "details": details_md(r) if r.status != OK else ""}
                         for r in results],
        }
        Path(args.json).write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    needs_attention = [r.name for r in results if r.status in (CHANGED, UNREACHABLE)]
    if needs_attention:
        print("\nNeeds attention: %s" % ", ".join(needs_attention))
    return 1 if needs_attention else 0


if __name__ == "__main__":
    sys.exit(main())
