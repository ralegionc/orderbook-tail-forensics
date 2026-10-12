# Data

This repository does not carry its raw inputs. The files listed below were present locally when the results in the README were produced, and were deleted on 2026-10-12 to reclaim **3.28 GB** of disk. Every one of them is a download from a public upstream source, so nothing unique was lost.

This file exists so that a re-download can be *checked* rather than assumed to match. It records the exact inventory, not just the source.

Upstream sources, with links and licences, are in the README under **Data**.

## What was removed

3.28 GB in 1 files.

| Path | Size | Files |
|---|---|---|
| `data/raw` | 3.28 GB | 1 |

## Restoring it

`data/raw` held the Nasdaq ITCH 5.0 sample feed for 2019-12-30, published by Nasdaq as a free historical sample. The parsed `data/events5.bin` and every latency result under `results/` were kept, so the measurements in the README stand without the raw feed.

## Inventory

`data_manifest.csv` carries every file with its exact byte length and a fingerprint. Files of 32 MiB or less have a full `sha256`. Larger files have `headtail256`: a sha256 over the first mebibyte, the last mebibyte and the exact byte length, which catches a truncated or altered download without rereading gigabytes.

| File | Size | Fingerprint |
|---|---|---|
| `data/raw/12302019.NASDAQ_ITCH50.gz` | 3.28 GB | `headtail256:80377923979458fce66a6538860a6206d614a3e34f01243de1c6a144787d37dd` |

A source that is live rather than a fixed release will not reproduce these fingerprints. Where that applies it is said above.

