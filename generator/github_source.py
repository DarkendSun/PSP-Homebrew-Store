"""Second catalogue source: GitHub releases of open-source PSP homebrew.

Candidates come from the PSP-Archive organisation, the `psp-homebrew` topic and generator/github_repos.txt.
A repo is kept only if GitHub reports a clear open-source license, it isn't CFW/flasher tooling, and its
latest release has a .zip asset that contains an EBOOT.PBP app. API answers are cached for a day and assets
by their id, so re-runs cost few requests. Unauthenticated calls are limited to 60 an hour; set GITHUB_TOKEN
in your environment (never in a file in the repo) for more.
"""

import hashlib
import json
import os
import re
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

GH_HOSTS = {"api.github.com", "github.com", "objects.githubusercontent.com", "release-assets.githubusercontent.com"}
UA = "PSP-Homebrew-Store-generator/1.0 (personal catalogue builder; Python urllib)"
API_TTL = 24 * 3600
MAX_ASSET = 50 * 1024 * 1024
# CFW installers, flashers and firmware tools don't belong in an app store
EXCLUDE = re.compile(r"(^|[^a-z])ark([^a-z]|$)|arkmenu|\bcfw\b|custom.?firmware|flasher|downgrad|"
                     r"chronoswitch|infinity|pro_custom|minimum_edition|^leda$|xmbctrl|customhome|autoboot|"
                     r"\bipl\b|pandora|despertar", re.I)
OPEN_LICENSES = {"MIT", "BSD-2-Clause", "BSD-3-Clause", "Apache-2.0", "GPL-2.0", "GPL-3.0", "LGPL-2.1",
                 "LGPL-3.0", "AGPL-3.0", "MPL-2.0", "Unlicense", "Zlib", "ISC", "CC0-1.0", "0BSD",
                 "GPL-2.0-only", "GPL-2.0-or-later", "GPL-3.0-only", "GPL-3.0-or-later", "BSL-1.0", "WTFPL"}


class RateLimited(Exception):
    pass


def _allowed(url):
    p = urllib.parse.urlparse(url)
    return p.scheme == "https" and (p.hostname or "").lower() in GH_HOSTS


class _GHRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not _allowed(newurl):
            raise urllib.error.URLError("refused a redirect away from GitHub")
        new = super().redirect_request(req, fp, code, msg, headers, newurl)
        if new is not None and urllib.parse.urlparse(newurl).hostname != "api.github.com":
            new.headers.pop("Authorization", None)   # never send the token to the asset CDN
        return new


_opener = urllib.request.build_opener(_GHRedirect)


def _get(url, accept="application/vnd.github+json", api=True):
    if not _allowed(url):
        raise ValueError("only GitHub is allowed here")
    headers = {"User-Agent": UA, "Accept": accept}
    token = os.environ.get("GITHUB_TOKEN", "").strip()
    if token and api:
        headers["Authorization"] = f"Bearer {token}"
    try:
        with _opener.open(urllib.request.Request(url, headers=headers), timeout=60) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        if e.code in (403, 429) and e.headers.get("x-ratelimit-remaining") == "0":
            raise RateLimited()
        raise


def api(path, cache_dir, delay):
    """GET https://api.github.com<path>, cached for a day. Returns parsed JSON, or None for 404."""
    key = hashlib.sha1(path.encode()).hexdigest()
    f = cache_dir / "api" / f"{key}.json"
    if f.is_file() and time.time() - f.stat().st_mtime < API_TTL:
        return json.loads(f.read_text(encoding="utf-8"))
    time.sleep(delay)
    try:
        data = json.loads(_get("https://api.github.com" + path))
    except urllib.error.HTTPError as e:
        if e.code != 404:
            raise
        data = None
    f.parent.mkdir(parents=True, exist_ok=True)
    f.write_text(json.dumps(data), encoding="utf-8")
    return data


def candidates(cache_dir, delay, extra_file):
    repos = {}
    page = 1
    while True:
        batch = api(f"/orgs/PSP-Archive/repos?per_page=100&page={page}", cache_dir, delay) or []
        for r in batch:
            repos[r["full_name"].lower()] = r
        if len(batch) < 100:
            break
        page += 1
    found = api("/search/repositories?q=topic:psp-homebrew&per_page=100", cache_dir, delay) or {}
    for r in found.get("items", []):
        repos.setdefault(r["full_name"].lower(), r)
    if extra_file.is_file():
        for line in extra_file.read_text(encoding="utf-8").splitlines():
            name = line.split("#")[0].strip()
            if name and name.lower() not in repos:
                r = api(f"/repos/{name}", cache_dir, delay)
                if r:
                    repos[r["full_name"].lower()] = r
    return list(repos.values())


def license_of(repo):
    spdx = (repo.get("license") or {}).get("spdx_id") or ""
    return spdx if spdx in OPEN_LICENSES else ""


def asset_zip(asset, cache_dir):
    """Download a release asset once (cached by id + updated time). Returns (path, sha1)."""
    key = f"{asset['id']}-{re.sub(r'[^0-9]', '', asset.get('updated_at', ''))}"
    path = cache_dir / "assets" / f"{key}.zip"
    if not path.is_file():
        if asset.get("size", 0) > MAX_ASSET:
            raise ValueError("asset too big")
        data = _get(asset["browser_download_url"], accept="application/octet-stream", api=False)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    return path, hashlib.sha1(path.read_bytes()).hexdigest()


def zip_listing(path):
    with zipfile.ZipFile(path) as z:
        return [(i.filename, i.file_size) for i in z.infolist() if not i.is_dir()]


def entries(cache_dir, delay, extra_file, apps_in, ascii_text, log=print):
    """Catalogue entries from GitHub releases, plus a skip count per reason."""
    out = []
    skipped = {"no open license": 0, "CFW/flasher": 0, "no release": 0, "no zip asset": 0,
               "no EBOOT.PBP": 0, "unsafe": 0, "error": 0}
    try:
        repos = candidates(cache_dir, delay, extra_file)
    except RateLimited:
        log("  GitHub rate limit reached while listing repos; set GITHUB_TOKEN or try again in an hour.")
        return out, skipped
    log(f"  GitHub: {len(repos)} candidate repos")
    for r in sorted(repos, key=lambda r: r["full_name"].lower()):
        name = r["full_name"]
        if EXCLUDE.search(r["name"]) or EXCLUDE.search(r.get("description") or ""):
            skipped["CFW/flasher"] += 1
            continue
        lic = license_of(r)
        if not lic:
            skipped["no open license"] += 1
            continue
        try:
            rel = api(f"/repos/{name}/releases/latest", cache_dir, delay)
            if not rel:
                skipped["no release"] += 1
                continue
            zips = [a for a in rel.get("assets", []) if a["name"].lower().endswith(".zip")]
            if not zips:
                skipped["no zip asset"] += 1
                continue
            hit = None
            for a in zips:
                path, sha1 = asset_zip(a, cache_dir)
                apps = apps_in(zip_listing(path), r["name"])
                if apps is None:
                    skipped["unsafe"] += 1
                    break
                if apps:
                    hit = (a, sha1, apps)
                    break
            if not hit:
                skipped["no EBOOT.PBP"] += 1
                continue
        except RateLimited:
            log("  GitHub rate limit reached; the rest is skipped this run (cached answers are kept).")
            break
        except Exception as e:  # one bad repo shouldn't stop the run
            skipped["error"] += 1
            log(f"  {name}: {type(e).__name__}")
            continue
        a, sha1, apps = hit
        if EXCLUDE.search(a["name"]) or any(EXCLUDE.search(n) for _, n in apps):
            skipped["CFW/flasher"] += 1
            continue
        tag = rel["tag_name"]
        out.append({
            "id": f"gh:{name}", "title": ascii_text(r["name"], 60),
            "kind": "app", "date": (rel.get("published_at") or "")[:10] or "-",
            "file": f"{tag}/{a['name']}", "size": int(a.get("size") or 0), "sha1": sha1, "apps": apps,
            "description": ascii_text(r.get("description") or "", 300),
            "source": "GitHub", "author": ascii_text(r["owner"]["login"], 40), "license": lic,
            "link": r["html_url"],
        })
    return out, skipped
