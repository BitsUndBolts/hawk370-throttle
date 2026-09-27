# Web installer: release worker and publishing

The web installer (`docs/index.html`, served by GitHub Pages) flashes the ESP32-C3 from the browser. It reads the list of releases from the GitHub API and downloads the release files through this Cloudflare Worker. GitHub's download servers send no CORS header, so the browser would refuse to hand the files to the page without the worker.

```
Browser (bitsundbolts.github.io/hawk370-throttle)
   ├── api.github.com/repos/bitsundbolts/hawk370-throttle/releases   list of versions (CORS ok)
   └── hawk370-throttle-releases.bitsundbolts.workers.dev/<tag>/<file>
          └── github.com/bitsundbolts/hawk370-throttle/releases/download/<tag>/<file>
```

The installer builds the worker address from the repo name, like AirMeter: `https://<repo>-releases.<owner>.workers.dev`. That only works while the worker name in `wrangler.toml` is `hawk370-throttle-releases` and the Cloudflare account's workers.dev subdomain is `bitsundbolts`. If either changes, set `PROXY_BASE` in `docs/index.html` to the real address.

## One-time setup

1. **Make the repository public.** The worker downloads release files anonymously, and the installer reads the release list without a login.
2. **Enable GitHub Pages:** repository *Settings → Pages → Build and deployment → Deploy from a branch*, branch `main`, folder `/docs`. The installer is then at `https://bitsundbolts.github.io/hawk370-throttle/`. (`docs/.nojekyll` makes Pages serve the files as they are.)
3. **Deploy the worker** (needs Node.js and a Cloudflare account):

   ```sh
   npm install -g wrangler
   wrangler login                 # opens the browser once
   cd docs/worker
   wrangler deploy
   ```

   Wrangler prints the address, which should be `https://hawk370-throttle-releases.bitsundbolts.workers.dev`.
4. **Test it** once a release exists (see below). This should download the file:

   ```
   https://hawk370-throttle-releases.bitsundbolts.workers.dev/v0.6/HAWK370_firmware.bin
   ```

The worker only answers pages from `https://bitsundbolts.github.io` (`ALLOWED_ORIGIN` in `worker.js`); a direct download in the browser's address bar still works. Release files are cached at Cloudflare's edge for a day. Publish a new tag rather than replacing files in an existing release.

## Publishing a release

1. In the Arduino IDE: *Sketch → Export Compiled Binary*.
2. Run `Build_Release.bat` in the sketch folder. It fills `release/` with:

   | File | Flash offset | Used by |
   | --- | --- | --- |
   | `HAWK370_bootloader.bin` | `0x0` | installer |
   | `HAWK370_partitions.bin` | `0x8000` | installer |
   | `boot_app0.bin` | `0xE000` | installer |
   | `HAWK370_firmware.bin` | `0x10000` | installer and OTA (Files page) |
   | `HAWK370_littlefs.bin` | `0x290000` | installer and OTA (Files page) |

3. On GitHub: *Releases → Draft a new release*, tag e.g. `v0.6`, and attach **all five files** with exactly these names. The installer skips releases that miss one of them.
4. Open the installer page. The new release shows up as "Latest (v0.6)".

The files are written one by one, not as one merged 4 MB image. That is what makes the installer's choice possible: with "Erase device" unticked, the settings partition (NVS at `0x9000`: Wi-Fi, CPU family, base clock, presets) is never touched. The Arduino IDE's `*.merged.bin` covers the whole flash and would always wipe it, so it is not part of a release.
