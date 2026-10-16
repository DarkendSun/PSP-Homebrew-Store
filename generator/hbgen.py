"""Builds and signs the PSP Homebrew Store catalogue from the Internet Archive.

  py -3 generator/hbgen.py keygen            make the signing key (once) and app/src/pubkey.h
  py -3 generator/hbgen.py build [--limit N] fetch, filter, write + sign relay/public/catalogue.txt
  py -3 generator/hbgen.py verify            check relay/public/catalogue.txt against its signature

Source: the "PlayStation Portable Library: Homebrews" collection (archive.org/details/psp-homebrew-library).
Only entries with a .zip that holds an EBOOT.PBP app are kept (the PSP app can unzip, not un-7z), and
anything listed in the not-working-psp item is dropped. Requests go only to archive.org, one at a time with a
delay, and every answer is cached under generator/cache/ so later runs only ask about new entries.
The archive.org request code follows the PSP CFW Helper's catalogue code.

The private key lives in %USERPROFILE%/.psp-hbstore/signing.key, never in the repo.
"""

import argparse
import datetime
import hashlib
import html
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path, PurePosixPath

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ed25519  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
CACHE = ROOT / "generator" / "cache"
OUT_TXT = ROOT / "relay" / "public" / "catalogue.txt"
OUT_SIG = ROOT / "relay" / "public" / "catalogue.sig"
OUT_JSON = ROOT / "generator" / "out" / "catalogue.json"
APP_EBOOT = ROOT / "relay" / "public" / "app" / "EBOOT.PBP"
VERSION_H = ROOT / "app" / "src" / "version.h"
PUBKEY_H = ROOT / "app" / "src" / "pubkey.h"
KEY_DIR = Path(os.environ.get("PSP_HBSTORE_KEY_DIR", Path.home() / ".psp-hbstore"))
KEY_FILE = KEY_DIR / "signing.key"

COLLECTION = "psp-homebrew-library"
NOT_WORKING = "not-working-psp"
UA = "PSP-Homebrew-Store-generator/1.0 (personal catalogue builder; Python urllib)"
FORMAT = "PSPHBS\t1"
RESERVED = {"update", "hbstore"}


# ------------------------------------------------------------------ archive.org (only)

def is_ia_url(url):
    p = urllib.parse.urlparse(url)
    host = (p.hostname or "").lower()
    return p.scheme == "https" and (host == "archive.org" or host.endswith(".archive.org"))


class _IARedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not is_ia_url(newurl):
            raise urllib.error.URLError("refused a redirect away from archive.org")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


_opener = urllib.request.build_opener(_IARedirect)
_last = [0.0]


def ia_get(url, delay):
    if not is_ia_url(url):
        raise ValueError("only archive.org is allowed")
    wait = _last[0] + delay - time.time()
    if wait > 0:
        time.sleep(wait)
    for attempt in range(3):
        try:
            with _opener.open(urllib.request.Request(url, headers={"User-Agent": UA}), timeout=60) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code not in (429, 500, 502, 503, 504) or attempt == 2:
                raise
            time.sleep(10 * (attempt + 1))
        finally:
            _last[0] = time.time()


def cached_json(kind, key, fetch):
    path = CACHE / kind / (hashlib.sha1(key.encode()).hexdigest() + ".json")
    if path.is_file():
        return json.loads(path.read_text(encoding="utf-8"))
    data = fetch()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data), encoding="utf-8")
    return data


def scrape(delay):
    fields = "identifier,title,description,creator,date,subject,mediatype"
    rows, cursor = [], None
    while True:
        q = {"q": f"collection:{COLLECTION}", "fields": fields, "count": "10000"}
        if cursor:
            q["cursor"] = cursor
        data = json.loads(ia_get("https://archive.org/services/search/v1/scrape?" + urllib.parse.urlencode(q), delay))
        rows += data.get("items", [])
        cursor = data.get("cursor")
        if not cursor:
            return rows


def metadata(item_id, delay):
    return cached_json("meta", item_id, lambda: json.loads(
        ia_get(f"https://archive.org/metadata/{urllib.parse.quote(item_id, safe='')}", delay)))


_ROW = re.compile(r'<tr><td><a href="[^"]*">([^<]+)</a><td>.*?<td id="size">(\d*)</tr>')


def zip_listing(item_id, f, delay):
    """[(path, size)] inside a zip, from archive.org's own zip browser (cached by the zip's sha1)."""
    def fetch():
        url = (f"https://archive.org/download/{urllib.parse.quote(item_id, safe='')}/"
               f"{urllib.parse.quote(f['name'])}/")
        page = ia_get(url, delay).decode("utf-8", "replace")
        return [(html.unescape(n), int(s or 0)) for n, s in _ROW.findall(page)]
    return cached_json("zip", f"{item_id}/{f['name']}/{f.get('sha1', '')}", fetch)


# ------------------------------------------------------------------ cleaning

def first(v):
    return (v[0] if v else "") if isinstance(v, list) else (v or "")


def ascii_text(text, limit):
    text = " ".join(text) if isinstance(text, list) else (text or "")
    text = html.unescape(re.sub(r"<[^>]+>", " ", text))
    text = text.replace("’", "'").replace("‘", "'").replace("“", '"').replace("”", '"')
    text = "".join(c if 32 <= ord(c) < 127 else ("" if ord(c) < 32 else "?") for c in text.replace("\t", " "))
    text = re.sub(r"\s+", " ", text).strip()
    return text[:limit - 3].rstrip() + "..." if len(text) > limit else text


def kind_of(subject):
    subs = subject if isinstance(subject, list) else re.split(r"[;,]", subject or "")
    subs = [s.strip(" '[]\"") for s in subs]
    return ascii_text(next((s for s in subs if s and "homebrew" not in s.lower()), "app"), 16)


def folder_name(name):
    name = re.sub(r"[^A-Za-z0-9 _.()!+-]", "_", name).strip(" .")[:40].strip(" .")
    return name if name and name.lower() not in RESERVED else ""


def stem_key(name):
    name = PurePosixPath(name).name.lower()
    for ext in (".7z", ".zip", ".rar"):
        if name.endswith(ext):
            name = name[:-len(ext)]
    return re.sub(r"[^a-z0-9]", "", name)


def is_archive(name):
    return name.lower().endswith((".7z", ".zip", ".rar"))


def pick_zip(meta):
    zips = [f for f in meta.get("files", []) if f.get("source") == "original"
            and f.get("name", "").lower().endswith(".zip") and f.get("sha1")]
    zips.sort(key=lambda f: ("/" in f["name"] and f["name"].lower().startswith("old"), -int(f.get("size") or 0)))
    return zips


def apps_in(listing, title):
    """[(prefix inside zip, PSP/GAME folder name)] for every EBOOT.PBP app, or None if the zip is unsafe."""
    for path, _ in listing:
        pp = PurePosixPath(path.replace("\\", "/"))
        if pp.is_absolute() or ".." in pp.parts or any(":" in x for x in pp.parts):
            return None
    dirs = sorted({str(PurePosixPath(p).parent) for p, _ in listing if PurePosixPath(p).name.lower() == "eboot.pbp"})
    dirs = [x for x in dirs if not any(o != "." and x != o and x.startswith(o + "/") for o in dirs)]
    out = []
    for x in dirs:
        name = folder_name(title if x == "." else PurePosixPath(x).name)
        if name:
            out.append(("" if x == "." else x + "/", name))
    return out


# ------------------------------------------------------------------ commands

def read_key():
    try:
        sk = bytes.fromhex(KEY_FILE.read_text().strip())
    except (OSError, ValueError):
        sys.exit(f"No signing key at {KEY_FILE}. Run: py -3 generator/hbgen.py keygen")
    if len(sk) != 32:
        sys.exit(f"{KEY_FILE} isn't a 32-byte ed25519 key")
    return sk


def write_pubkey_h(pk):
    body = ", ".join(f"0x{b:02x}" for b in pk)
    PUBKEY_H.write_text(
        "/* Generated by generator/hbgen.py keygen. Public key that signs catalogue.txt. */\n"
        "#ifndef HB_PUBKEY_H\n#define HB_PUBKEY_H\n"
        f"static const unsigned char HB_PUBKEY[32] = {{{body}}};\n#endif\n", encoding="utf-8")


def cmd_keygen(args):
    if KEY_FILE.exists():
        sys.exit(f"{KEY_FILE} already exists; not replacing it (the app only trusts that key). "
                 "Delete it yourself if you really want a new one, then rebuild the app.")
    KEY_DIR.mkdir(parents=True, exist_ok=True)
    sk = os.urandom(32)
    KEY_FILE.write_text(sk.hex() + "\n")
    pk = ed25519.public_key(sk)
    write_pubkey_h(pk)
    print(f"Private key: {KEY_FILE}  (keep it; never commit it)")
    print(f"Public key:  {pk.hex()}  (written to {PUBKEY_H.relative_to(ROOT)})")


def app_version():
    m = re.search(r'APP_VERSION\s+"([0-9.]+)"', VERSION_H.read_text(encoding="utf-8"))
    return m.group(1) if m else "0.0.0"


def cmd_build(args):
    sk = read_key()
    delay = args.delay
    print("Listing the collection (1 request)...")
    rows = sorted(scrape(delay), key=lambda r: r.get("identifier", ""))
    rows = [r for r in rows if r.get("mediatype", "software") == "software" and r.get("identifier")]
    if args.only:
        wanted = set(args.only.split(","))
        rows = [r for r in rows if r["identifier"] in wanted]
    if args.limit:
        rows = rows[:args.limit]
    bad = {stem_key(f["name"]) for f in json.loads(
        ia_get(f"https://archive.org/metadata/{NOT_WORKING}", delay)).get("files", []) if is_archive(f.get("name", ""))}
    bad.discard("")

    entries, skipped = [], {"no zip": 0, "no EBOOT.PBP": 0, "not working": 0, "unsafe": 0, "error": 0}
    for i, r in enumerate(rows, 1):
        item_id = r["identifier"]
        title = ascii_text(r.get("title"), 60) or item_id
        try:
            meta = metadata(item_id, delay)
            names = {stem_key(f["name"]) for f in meta.get("files", []) if is_archive(f.get("name", ""))}
            names |= {stem_key(title), stem_key(item_id)}
            if names & bad:
                skipped["not working"] += 1
                continue
            zips = pick_zip(meta)
            if not zips:
                skipped["no zip"] += 1
                continue
            f = zips[0]
            apps = apps_in(zip_listing(item_id, f, delay), title)
            if apps is None:
                skipped["unsafe"] += 1
                continue
            if not apps:
                skipped["no EBOOT.PBP"] += 1
                continue
        except Exception as e:  # keep going; one bad entry shouldn't stop the run
            skipped["error"] += 1
            print(f"  {item_id}: {type(e).__name__}")
            continue
        entries.append({
            "id": item_id, "title": title, "kind": kind_of(r.get("subject")),
            "date": first(r.get("date"))[:10] or "-", "file": f["name"], "size": int(f.get("size") or 0),
            "sha1": f["sha1"].lower(), "apps": apps, "description": ascii_text(r.get("description"), 300),
            "creator": ascii_text(r.get("creator"), 60),
        })
        if i % 25 == 0:
            print(f"  {i}/{len(rows)} checked, {len(entries)} kept")

    version = args.app_version or app_version()
    app_size, app_sha = "-", "-"
    if APP_EBOOT.is_file():
        data = APP_EBOOT.read_bytes()
        app_size, app_sha = str(len(data)), hashlib.sha512(data).hexdigest()
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%MZ")
    lines = [f"{FORMAT}\t{stamp}\t{version}\t{app_size}\t{app_sha}"]
    for e in sorted(entries, key=lambda e: e["title"].lower()):
        apps = "|".join(f"{pre}>{name}" for pre, name in e["apps"])
        for field in (e["id"], e["file"]):
            if "\t" in field or "\n" in field:
                raise ValueError(f"tab or newline in {field!r}")
        lines.append("\t".join(["E", e["id"], e["title"], e["kind"], e["date"], e["file"], str(e["size"]),
                                e["sha1"], apps, e["description"] or "-"]))
    body = ("\n".join(lines) + "\n").encode("utf-8")
    OUT_TXT.parent.mkdir(parents=True, exist_ok=True)
    OUT_TXT.write_bytes(body)
    OUT_SIG.write_text(ed25519.sign(sk, body).hex() + "\n")
    OUT_JSON.parent.mkdir(parents=True, exist_ok=True)
    OUT_JSON.write_text(json.dumps({"generated": stamp, "app_version": version, "entries": entries}, indent=1),
                        encoding="utf-8")
    print(f"Kept {len(entries)} of {len(rows)} entries. Skipped: {skipped}")
    print(f"Wrote {OUT_TXT.relative_to(ROOT)} ({len(body)} bytes) and its signature; app version {version}"
          + ("" if app_sha != "-" else " (no relay/public/app/EBOOT.PBP yet, so no app update offered)"))


def cmd_verify(args):
    pk = ed25519.public_key(read_key()) if not args.pubkey else bytes.fromhex(args.pubkey)
    ok = ed25519.verify(pk, OUT_TXT.read_bytes(), bytes.fromhex(OUT_SIG.read_text().strip()))
    print("signature OK" if ok else "signature BAD")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description="Build and sign the PSP Homebrew Store catalogue.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("keygen")
    b = sub.add_parser("build")
    b.add_argument("--limit", type=int, help="only check the first N entries (for testing)")
    b.add_argument("--only", help="comma-separated archive.org ids to check")
    b.add_argument("--delay", type=float, default=1.0, help="seconds between archive.org requests (default 1)")
    b.add_argument("--app-version", help="app version to advertise (default: app/src/version.h)")
    v = sub.add_parser("verify")
    v.add_argument("--pubkey", help="hex public key to check against (default: from the private key)")
    args = ap.parse_args()
    return {"keygen": cmd_keygen, "build": cmd_build, "verify": cmd_verify}[args.cmd](args) or 0


if __name__ == "__main__":
    sys.exit(main())
