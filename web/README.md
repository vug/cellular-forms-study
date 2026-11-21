# Cellular Forms in the browser

A WebGPU page that grows Cellular Forms: choose parameters and a start, watch the form evolve,
change its material and light, and pick a form from a gallery to grow it again with its settings.
Everything runs in the visitor's browser, on their GPU. The server only hands out files.

## Putting it on a web server

Copy this whole `web/` folder to the server, to any path, and open that path (its `index.html`
sends the visitor on to `app/`, where the page is).

- There is no build step and nothing to install: HTML, JavaScript modules, the gallery's pictures
  and the start shapes, 1.8 MB in all. Every address in the page is relative.
- The server must speak **https** (WebGPU only works on https and on localhost) and serve `.js`
  files as `text/javascript`, which every usual web server does. `.cfs` files are binary
  (`application/octet-stream`).
- Nothing is sent anywhere: no fonts, scripts or data from other hosts, no cookies, no storage.

## What a visitor needs

A browser with WebGPU and a GPU behind it. As of November 2025 (web.dev, "WebGPU is now
supported in major browsers"): Chrome and Edge on Windows, macOS and ChromeOS, and Chrome on
Android 12 and later with a Qualcomm or ARM GPU; Safari 26 on macOS, iOS and iPadOS 26; Firefox
141 on Windows and Firefox 145 on Apple-silicon Macs. Linux, and Firefox on Android, were
still in progress. Without WebGPU the page says what is missing. It was tried on this PC only
(Chromium, an RTX 4090): no phone, Safari or Firefox has run it yet.

## What is in the folder

| Path | What |
| --- | --- |
| `app/` | The page: `index.html`, its modules and styles |
| `app/gallery/` | The gallery: `gallery.json` (each form's start shape, seed, parameters and a few measures) and its pictures as contact sheets |
| `app/starts/` | The start shapes, as state files without food |
| `app/params.js` | The parameters the controls offer: names, ranges, defaults |
| `sim.js`, `shaders.js` | The simulation (WebGPU compute), the whole model of `src/core` |
| `start.js`, `cfs.js` | Start states from a shape, a seed and parameters; reading state files |
| `index.html` | The front door: sends the visitor on to `app/` |
| `check.html`, `main.js`, `tasks.js`, `worker.js` | The developers' check page: compares the simulation step by step with the CPU reference (`check.html?task=verify`). It needs reference data from the repository and is not needed on the server |

## A link is a setup

"Copy link" puts the whole setup into the address after `#`: the parameters that differ from
the defaults, start shape, seed, capacity, look and camera. Whoever opens the link gets the
same run. `#g=<number>` opens a gallery form. With "exact arithmetic" on, the same start, seed
and parameters give the same form as on the PC, cell for cell; a gallery form switches it on,
and it is on by itself as soon as any optional mechanism is on. (The two rough-sphere starts
are the shapes of seed 1; the PC makes another rough sphere for every seed.)

## Changing the gallery

The gallery is written from the atlas (`docs/design.md`, "Finding interesting parameters"):

    python scripts/atlas web-gallery --count 120
    python scripts/atlas web-starts

The first picks forms (those Uğur rated 4 or 5 first, then the typical forms of every kind, then
the forms with the most structure, reach, patchiness and uneven growth), grows each again for
its picture and writes `app/gallery/`. Copy the folder to the server again afterwards.

## Trying it on this PC

Any static file server will do. From this folder:

    python -m http.server

then open http://localhost:8000/ . Or, from the repository's root, `python scripts/web_serve.py`
and http://localhost:8123/web/ .

To check the simulation against the CPU reference (this needs the repository, its build and its
own server, because the reference data lies outside this folder): `scripts\web_verify_data.ps1`,
`python scripts/web_serve.py`, then http://localhost:8123/web/check.html?task=verify .
