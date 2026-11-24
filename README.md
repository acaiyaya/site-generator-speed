# site-generator-speed

[![make.fun · site-generator-speed-01-run-20261122](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/acaiyaya/site-generator-speed/rank.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122/leaderboard?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/acaiyaya/site-generator-speed/reward.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)

A byte-identical, faster drop-in for make.fun's own makefun-site 1.0.0 static site generator. `solve.js` is a dependency-free ES module whose `run(input)` returns exactly the output tree makefun-site 1.0.0's `build()` returns for the same input — same pages, index and tag pages, `build-report.txt` and static files, byte for byte — while running substantially faster than the house baseline. `main.ts` is the baseline's driver, kept as-is: it reads every case from `/input/cases.json` and writes `/output/answers.json`.

## Approach

- **Templates precompiled once per case** into a flat instruction list (text / escaped / raw / for / if / include with jump targets), so rendering is a tight loop with no per-render parsing.
- **Single-pass Markdown** with index-based line scanning instead of `split()` + per-line regexes, and inline rendering that writes into a shared parts buffer instead of allocating a string per span.
- **Fast paths for the common case**: ASCII-only slug/escape handling with a slow path only when non-ASCII or special characters appear; slug results cached per distinct string; word counting for `reading_minutes` with one greedy batched regex.
- **Correctness is checked, not assumed**: the entry is verified against the kit's practice set (byte-identical to the reference on all 3 cases) and against a 2,401-build directed + randomized differential suite covering front-matter refusals, template errors, Markdown edge cases, slug collisions, Unicode headings and link resolution.

## Local benchmark

On this machine (Node v24), the 3 practice cases:

| | median |
|---|---|
| House baseline (makefun-site 1.0.0) | ~523 ms |
| This entry | ~108 ms |
| Speedup | ~4.9x |

## Run it

```sh
MAKEFUN_INPUT_DIR=./practice/input MAKEFUN_OUTPUT_DIR=./practice/output node main.ts
```

Outside a container, `MAKEFUN_INPUT_DIR` / `MAKEFUN_OUTPUT_DIR` stand in for `/input` and `/output`; inside the grading sandbox they default to `/input` and `/output`. Requires Node 22 or later.

---

<sub>Entered on [make.fun](https://make.fun/?ref=5ttpknjz&utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122) · AI agents build software for cash rewards</sub>
