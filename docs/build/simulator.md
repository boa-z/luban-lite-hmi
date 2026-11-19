# Simulator contract

> [中文版](simulator.zh-CN.md)

`hmi-sim` owns the LVGL loop and emits one JSON object on stdout after a finite run. It exits zero only when the object count remains stable, selected scenario completed and optional capture succeeded.

| Option | Meaning |
|---|---|
| `--smoke` | 240-frame hidden CI run and navigation checks |
| `--hidden --frames N` | finite SDL run without a window |
| `--scenario normal\|warning\|stale\|offline\|error\|unknown` | synthetic state |
| `--visual min\|mid\|max` | speed/SOC/steering visual fixture |
| `--page 0..3` | open Dashboard, Monitor, Faults or Settings |
| `--capture path.bmp` | save the current rendered frame |
| `--set-language english\|chinese` | set the language for this run |
| `--set-units metric\|imperial` | set units for this run |

The report includes `frames`, object count, heap high-water, average/max presentation time, runtime dispatch diagnostics, active fault count with its `fault_ids` identities, validity state, value, page, `language` (`en` or `zh-CN`) and reserved `ge_hits`/`sw_fallbacks` fields. GE2D values remain `null` because the host target uses LVGL software rendering.

English is the default. The Settings page and command-line options apply preferences for the current run. Unsupported language arguments exit with code 2.

Linux CI runs on `ubuntu-latest` with SDL2 and `SDL_VIDEODRIVER=dummy`. The scenario tests verify both languages on all pages. The `i18n-ui` test checks every label for missing glyphs while switching English to Chinese and back, including unknown/stale/error states.

Font subsets are generated with `npm run fonts:generate` using the declared `lv_font_conv@1.5.3` MIT dev dependency (Node.js/npm required only for regeneration). Normal builds use committed C sources and require no font downloads. `npm run fonts:check` or `python3 tools/generate_fonts.py --check` verifies translation coverage, provenance, license and hashes without Node or network.
