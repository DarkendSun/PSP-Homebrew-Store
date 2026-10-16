# PSP Homebrew Store

A homebrew store that runs on a PSP with CFW, for a few friends. It shows a **signed catalogue** of free
homebrew from the Internet Archive's
[PlayStation Portable Library: Homebrews](https://archive.org/details/psp-homebrew-library), downloads apps
through a small **relay** (a Cloudflare Worker), checks each download, and installs it to
`PSP/GAME/<folder>/`.

> These are third-party homebrew files, made by many different people. You use them at your own risk.
> The store never touches `PSP/GAME/UPDATE`, and it only deletes folders it installed itself.

**Status: a first working shell.** The catalogue generator, signing and relay are tested on Windows. The PSP
app has every screen and step written, but it hasn't been compiled or run yet: there's no PSP toolchain on
this PC. The GitHub Actions workflow builds it, and then it needs a run in PPSSPP and on a real PSP (see
"What's left").

```
app/        PSP app (C, pspdev): EBOOT.PBP
generator/  catalogue builder + ed25519 signer (Python, no extra packages)
relay/      Cloudflare Worker: serves the signed catalogue and passes through listed archive.org files
```

## How it fits together

1. **Generator** (your PC): lists the archive.org collection and GitHub releases of open-source PSP
   homebrew. It keeps entries whose `.zip` contains an `EBOOT.PBP` app, drops anything in the
   `not-working-psp` item, and writes `relay/public/catalogue.txt`, signed with your ed25519 key
   (`catalogue.sig`).
2. **Relay** (Cloudflare): the PSP can't do modern HTTPS, so it talks **plain HTTP** to the relay. The relay
   serves the catalogue, and fetches from `https://archive.org` or GitHub only files that are **listed in
   the catalogue**. It isn't an open proxy: anything else gets a 404.
3. **App** (PSP): it accepts a catalogue only if its signature matches the public key built into the app.
   It checks every download's SHA-1 against the catalogue before unpacking, and its own updates against a
   SHA-512.

## 1. Catalogue generator

```
py -3 generator/hbgen.py keygen       # once: makes the key + app/src/pubkey.h (already done on this PC)
py -3 generator/hbgen.py build        # whole collection
py -3 generator/hbgen.py build --limit 40   # quick test
py -3 generator/hbgen.py verify
```

- **The private key** is `%USERPROFILE%\.psp-hbstore\signing.key`, outside the repo and gitignored. Back it
  up somewhere private. Lose it and you'd have to ship a new app build with a new key. `keygen` refuses to
  overwrite it.
- **The first full build** checks about 4,170 entries with 1 to 2 polite requests each (1 s apart, cached in
  `generator/cache/`), so it takes a couple of hours. Later builds only ask about new entries. A test run of
  40 entries kept 37.
- **Only `.zip` versions are used**, because the app unpacks zip but not 7z. If an entry has several zips, it
  prefers the newest-looking one (not under "Old versions").
- **GitHub releases** (`generator/github_source.py`): candidates come from the
  [PSP-Archive](https://github.com/PSP-Archive) organisation, the `psp-homebrew` topic and
  `generator/github_repos.txt`. A repo is listed only if:
  - GitHub reports an open-source license (MIT, BSD, GPL, LGPL, Apache, MPL, Unlicense and so on).
  - It isn't CFW, flasher or firmware tooling.
  - Its latest release has a `.zip` asset with an `EBOOT.PBP` app. The generator downloads the zip once to
    look inside and to take its SHA-1.

  The source, author, license and repo link are recorded, and shown on the app's details screen. If a GitHub
  app is also in the archive.org library, the GitHub release is kept and the archive.org copy dropped. API
  answers are cached for a day. Unauthenticated GitHub allows 60 API calls an hour; for more, set
  `GITHUB_TOKEN` in your shell for that run (read-only, never put it in a file in the repo). Add
  `--no-github` to skip this source.
- **Catalogue format** (`catalogue.txt`, UTF-8, ASCII text fields):
  `PSPHBS 2 <generated> <app version> <app size> <app sha512>` on the first line, then one line per entry:
  `E <id> <title> <kind> <date> <zip file> <size> <sha1> <prefix>NAME|... <description> <source> <author>
  <license> <link>`, tab-separated. GitHub entries have ids `gh:<owner>/<repo>` and files `<tag>/<asset>`.
  The signature covers the exact bytes.

## 2. Relay (a new, separate Cloudflare Worker)

`relay/` is its own Worker, **`psp-homebrew-relay`**, separate from anything else on your Cloudflare
account.

- `GET /catalogue.txt`, `/catalogue.sig`, `/app/EBOOT.PBP`: published files from `relay/public/`.
- `GET /dl/<id>/<file>`: streams `https://archive.org/download/<id>/<file>`, or for `gh:` entries
  `https://github.com/<owner>/<repo>/releases/download/<tag>/<asset>`, only when that pair is in the
  catalogue.
  - Redirects are followed only within archive.org, or within github.com, `objects.githubusercontent.com`
    and `release-assets.githubusercontent.com`.
  - It retries once if the source has a hiccup.
- **Rate limit:** 60 requests a minute per IP, using Cloudflare's rate-limit binding.
- **No login or token:** anyone with the address can read the catalogue and fetch the listed files. The
  allow-list and the rate limit stop it being used for anything else.

Run it locally (tested):

```
cd relay
npm install
npx wrangler dev --ip 0.0.0.0 --port 8787
```

Then point the app (or `curl`) at `http://<this PC's LAN IP>:8787`.

**Deploying is your call.** Nothing has been deployed or changed on Cloudflare. When you want it:

```
cd relay
npx wrangler login
npx wrangler deploy
```

Each `deploy` publishes whatever is in `relay/public/`, so run the generator
first.

**Plain HTTP:** `workers.dev` answers plain `http://` as well as `https://` and doesn't redirect, so
`http://psp-homebrew-relay.<your-subdomain>.workers.dev` works with **no settings changes**. That's the
recommended address. (Browsers force HTTPS on `.dev` because of HSTS preloading, but the PSP's network
stack doesn't, which is what we want here.)

If you'd rather use a subdomain of your own domain on Cloudflare (e.g. `psp.example.com`):

1. Add it as a Custom Domain on this Worker.
2. In that zone, add a **Configuration Rule** for that hostname that turns **Always Use HTTPS off**,
   so plain HTTP isn't redirected to HTTPS.
3. Check that no Redirect Rule or Page Rule forces HTTPS for that host.

HSTS doesn't matter, since the PSP ignores it. The workers.dev address needs none of this.

## 3. PSP app

### Build

- **GitHub Actions** (`.github/workflows/build.yml`) builds `EBOOT.PBP` in the `pspdev/pspdev` Docker image
  on every push, and uploads it as the `hbstore-EBOOT` artifact. Set the repository variable `RELAY_URL` to
  your relay address so it's built in.
- **Locally** you need Docker Desktop (not installed on this PC):

  ```
  powershell -File app\build.ps1 -Relay http://psp-homebrew-relay.<you>.workers.dev
  ```

  `app/build.sh` does the same on Linux or macOS.

### Install on a PSP

Make a folder named `HBSTORE` with the `EBOOT.PBP` in it. In the PSP CFW Helper, tick **Skip
Infinity/CFW**, add that folder under **Extra apps**, then click Preview and **Back up and copy**. On the PSP
it's under **Game > Memory Stick > PSP Homebrew Store**. CFW (PRO) must be loaded.

The relay address is built in. You can change it under **SELECT > Settings** (using the PSP's keyboard),
or in `PSP/GAME/HBSTORE/config.txt`:

```
relay=http://psp-homebrew-relay.<you>.workers.dev
```

### Using it

| Button | List | Details |
|---|---|---|
| Up/Down, L/R | move, page | |
| X | open | download + install (or reinstall) |
| Square | show installed only | uninstall (only folders it installed) |
| Triangle | search (PSP keyboard) | |
| START | connect (PSP's own Wi-Fi dialog) and fetch the catalogue | |
| SELECT | settings: relay address, free space | |
| O | | back / cancel a download |

- **Installing:** the app downloads into `PSP/HBSTORE_TMP/` and checks the SHA-1. It unpacks only the app
  folder named in the catalogue, refusing unsafe paths, then marks it with `HBSTORE.ID` and moves it into
  `PSP/GAME/<folder>/`.
- **Existing folders:** a folder that already exists and wasn't installed by the store is left alone.
- **Uninstalling:** only removes folders carrying that marker. Saved games in `PSP/SAVEDATA` stay.

### App updates

The signed catalogue carries the app's latest version, and the size and SHA-512 of `relay/public/app/EBOOT.PBP`.
After fetching the catalogue, the app says **"Update available"** and asks before downloading. It checks the
hash, replaces its own `EBOOT.PBP`, and asks you to restart. To release a new version:

1. Bump `APP_VERSION` in `app/src/version.h`.
2. Build, and copy the new `EBOOT.PBP` to `relay/public/app/EBOOT.PBP`.
3. Run `py -3 generator/hbgen.py build`, then deploy the relay.

## What's left / untested

- **The PSP app hasn't been compiled or run yet.** It was written against the real pspsdk headers but never
  compiled. Expect a round of fixes from the first CI build. Then test it in PPSSPP (installed on this PC
  through scoop) and on a real PSP.
- **PPSSPP:** its Wi-Fi emulation varies by version, so the download path is best tested on a real PSP.
- **Icons:** the list shows a coloured G/A type tag. Real `ICON0.PNG` icons would need the generator to
  extract them and the relay to serve them, plus a PNG decoder in the app.
- **Zips only:** entries that only exist as `.7z` aren't in the catalogue. 7z on the PSP is a possible later
  step.
- **Speed:** the "installed only" filter checks every app's folder, so it's slow with a full catalogue.

## Credits

- **TweetNaCl** by D. J. Bernstein, W. Janssen, T. Lange, P. Schwabe, S. Smetsers and others: public
  domain, from <https://tweetnacl.cr.yp.to> (`app/src/tweetnacl.c`, `tweetnacl.h`, unchanged).
- **Ed25519 reference code** from RFC 8032, section 6 (`generator/ed25519.py`, adapted).
- **pspdev / PSPSDK** toolchain and libraries, and **zlib** by Jean-loup Gailly and Mark Adler.
- **GitHub homebrew authors**: each GitHub entry's author, license and repository are listed in the
  catalogue and shown in the app.
- **The Internet Archive** and the **PSP-Archive** preservation community
  (<https://github.com/PSP-Archive>) for the homebrew library. Their repositories were read for reference
  only; no code was copied from them.
- The archive.org request code follows the PSP CFW Helper's catalogue code.

No license is chosen yet. That's the owner's decision.

## Putting it on GitHub (private)

`gh` isn't installed, so this is done on the website:

1. Go to <https://github.com/new>. Set the owner to DarkendSun, the name to `psp-homebrew-store`, and choose
   **Private**. Don't add a README, license or .gitignore.
2. Then:

   ```
   cd "%USERPROFILE%\Desktop\PSP Homebrew Store"
   git remote add origin https://github.com/DarkendSun/psp-homebrew-store.git
   git push -u origin main
   ```

3. In the repo settings, add the Actions variable `RELAY_URL`. The **Actions** tab then has the built `EBOOT.PBP`.
