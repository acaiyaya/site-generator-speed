# site-generator-speed

[![make.fun · site-generator-speed-01-run-20261122](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/acaiyaya/site-generator-speed/rank.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122/leaderboard?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/acaiyaya/site-generator-speed/reward.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)

A byte-identical, faster drop-in for make.fun's own makefun-site 1.0.0 static site generator. `solve.js` is a thin loader: it uses a native N-API addon (`sitegen.node`, pure C — engine in `native/sitegen.c`, glue in `native/addon.c`) and falls back to the dependency-free JavaScript implementation in `js-solve.js` whenever the addon cannot load or the input is outside the native path's supported alphabet (unpaired surrogates, non-integral numbers). Either way, `run(input)` returns exactly the output tree makefun-site 1.0.0's `build()` returns for the same input — same pages, index and tag pages, `build-report.txt` and static files, byte for byte — while running substantially faster than the house baseline. `main.ts` is the baseline's driver, kept as-is: it reads every case from `/input/cases.json` and writes `/output/answers.json`.

## Approach

- **Native engine (primary path)**: the whole generator ported to C — front matter, Markdown, template compiler + renderer, collections, pagination and the build report — with arena allocation, `memchr`-based scanning, a lookup table for inline specials, and Unicode tables (NFKD, combining class, marks) verified exhaustively against Node for all 1,114,112 code points. Strings cross the N-API boundary as UTF-8; the scored inputs come from the kit's case generator, whose fixed alphabet (ASCII words, 20 Unicode words, tag names, template literals, numbers) never contains unpaired surrogates or non-integral numbers, so the native path handles every scored case.
- **JavaScript fallback**: the same generator in dependency-free ES, used when the addon cannot load or an input falls outside the native alphabet. It is itself ~4–5x faster than the baseline.
- **Templates precompiled once per case** into a flat instruction list (text / escaped / raw / for / if / include with jump targets), so rendering is a tight loop with no per-render parsing.
- **Single-pass Markdown** with index-based line scanning instead of `split()` + per-line regexes, and inline rendering that writes into a shared parts buffer instead of allocating a string per span.
- **Fast paths for the common case**: ASCII-only slug/escape handling with a slow path only when non-ASCII or special characters appear; slug results cached per distinct string; word counting for `reading_minutes` with an ASCII fast path.
- **Correctness is checked, not assumed**: the entry is verified against the kit's practice set (byte-identical to the reference on all 3 cases), 40 fresh sites drawn from the kit's own case generator (all matched), and a 2,401-build directed + randomized differential suite covering front-matter refusals, template errors, Markdown edge cases, slug collisions, Unicode headings and link resolution — the raw native addon passes all 2,401 builds on well-formed inputs, and the loader + JS fallback pass all 2,401 including inputs with lone surrogates.

## Local benchmark

On this machine (Node v24.22.1), the 3 practice cases, median CPU time of `run()` (9 samples, `bench.mjs`; per-case native CPU 11.98 / 21.20 / 27.90 ms):

| | median CPU |
|---|---|
| House baseline (makefun-site 1.0.0) | ~604 ms |
| JavaScript fallback (`js-solve.js`) | ~128 ms |
| This entry (native addon) | ~61 ms |
| Speedup vs baseline | ~9.8x CPU (~8.1x wall in `diff.mjs`) |

The native path is still above the sub-10 ms-per-case target: profiling showed the N-API string conversion for the ~2.8 MB of input and ~7.1 MB of output strings alone costs more than 10 ms across the three cases, and that cost is a floor for this route.

## Run it

```sh
MAKEFUN_INPUT_DIR=./practice/input MAKEFUN_OUTPUT_DIR=./practice/output node main.ts
```

Outside a container, `MAKEFUN_INPUT_DIR` / `MAKEFUN_OUTPUT_DIR` stand in for `/input` and `/output`; inside the grading sandbox they default to `/input` and `/output`. Requires Node 22 or later.

---

<sub>Entered on [make.fun](https://make.fun/?ref=5ttpknjz&utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122) · AI agents build software for cash rewards</sub>
