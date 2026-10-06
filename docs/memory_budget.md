# Memory budget — ESP32-S3R8 (512 KB SRAM, 8 MB octal PSRAM, 16 MB flash)

Asserted at boot by `firmware/main/psram_plan.h` / `assert_plan()`.

## Flash (16 MB) — `firmware/partitions.csv`

| Partition | Size | Holds |
|---|---|---|
| nvs | 24 KB | fish persistence (progression layer) |
| factory app | 2.5 MB | ESP-IDF + tank + LVGL/display port (~1–1.5 MB used) |
| **model** (raw, type 0x40) | **8 MB** | `model_q4.bin` = **7.56 MB** (14.3M params, 4-bit, GS 64, fp16 scales) |
| storage (spiffs) | 5.4 MB | assets / OTA headroom later |

The model is **memory-mapped** (`esp_partition_mmap`) and read through the flash
cache during inference — it is never copied into RAM. Flashing the model:
`esptool write_flash 0x290000 model/out/model_q4.bin`.

## PSRAM (8 MB)

| Item | Bytes | Note |
|---|---|---|
| framebuffers 2 × 448×368×2 | 659 KB | double-buffered RGB565 |
| KV cache 2 × 8 layers × 64 seq × 384 × fp32 | 1.5 MB | word tokens → seq 64 is enough (docs are 46) |
| activations (x, xb, q, hb, att, logits) | < 64 KB | |
| tank state | ~10 KB | |
| LVGL heap (when enabled) | ~256 KB | |
| **reserved headroom (asserted)** | 2 MB | |
| total | ~4.5 MB | of 8 MB |

## SRAM (512 KB)

FreeRTOS, task stacks (tank 12 KB, advisor 16 KB), hot-loop scratch. The matmul
inner loop streams flash weights + int8 activations; no large SRAM residents.

## Latency model (to be measured in QEMU / on hardware)

A decision = ~44 prompt tokens + 3 generated. Every token is one full pass over
7.56 MB of weights (single-token matmuls are bandwidth-bound):

| Effective weight read rate | tok/s | per decision |
|---|---|---|
| 40 MB/s (flash cache, pessimistic) | ~5 | ~9 s |
| 80 MB/s | ~10 | ~4.6 s |
| 160 MB/s (PSRAM-resident weights) | ~21 | ~2.2 s |

Levers, in order: (1) **batched prefill** — process the 44 prompt tokens as one
matrix-matrix pass so weights are read once per layer instead of 44 times
(turns the decision into ~4 weight passes ≈ 0.5–1 s if compute keeps up);
(2) ESP-DSP SIMD + dual-core matmul split (the esp32-llm recipe); (3) the 8.8M
fallback model (4.7 MB). The 1–3 s advisor cadence across 4 fish is the target.
