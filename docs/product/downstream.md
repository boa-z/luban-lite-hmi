# Product integration

> [中文版](downstream.zh-CN.md)

## Repositories

Every Product, public or private, is its own Git repository cloned into `products/<product>` (or selected by an absolute path). The Framework ignores `products/*`, records no Product gitlink and needs no private fork: all Products build against the public Framework `main`. Customer code, protocols, UI, assets and requirements stay in the Product repository.

```text
git clone PRODUCT_REPOSITORY products/my-product
cmake -S . -B build-my-product -G Ninja -DHMI_PRODUCT_ROOT=products/my-product
```

Firmware names the Product with `CONFIG_AIC_HMI_PRODUCT_ROOT="products/my-product"` in the board defconfig. Nothing pins the Product commit, so record the Product, Framework and SDK commits for every release build. Start a new Product with `python tools/create_product.py --id my-product --output products/my-product`: the template has a catalog, an LVGL UI and one piece of local business (`app_tick`) and opens no bus; add `--can` for a DBC-generated CAN decoder and routes. A Product never edits Framework sources to be added. Do not add customer symbols to `contracts`, `core`, `runtime`, `protocols/common` or `ui/common`; propose missing generic capabilities to the Framework instead.

## What a Product provides

Two optional startup callbacks separate business startup from rendering. `prepare()` restores Product
state and initial settings before Core initialization. `start()` starts peripheral services after
platform devices are ready, before UI creation or protocol processing. Each runs once per process
startup; false aborts startup. Products without these needs leave the callbacks unset. Firmware, the
UI host runner and the headless protocol runner follow this ordering; `--catalog` prepares settings
and catalog data without starting device services.

Product settings own defaults, valid ranges, repair of stored values and runtime effects. Core copies
the settings returned by `prepare()` without repeating those rules; catalog and storage-capacity checks
still apply. A Product with its own settings store leaves `storage` unset and disables
`AIC_HMI_BACKLIGHT` when it controls the backlight. The Framework still latches the configured CAN
rate before opening buses and checks device operations.

Startup callbacks do not access LVGL and must not be called again from the first protocol frame,
tick or page creation. `hmi_firmware_compose()` only composes storage and `locale_init()` only
registers language resources. Neither restores settings or starts business services. Synthetic host
input advances only after start. Product hardware workers must respect their own state synchronization.

`hmi_product_get()` returns one static `hmi_product_t` (`contracts/hmi_product.h`). Only `id` and `catalog` are required, and the catalog tables may be empty. A Product that talks to no bus leaves `protocols`, `routes` and `periodic` unset. The runtime then opens no CAN bus and starts no CAN transmit thread.

| Field | Purpose |
|---|---|
| `catalog`, `initial_settings` | Signals, local settings, faults; first-boot language, brightness, units and, for CAN Products, bit rate. How a page lays out signals (monitor lists and so on) belongs to the Product UI |
| `protocols`, `routes` | Receive routing to stateless DBC decoders or stateful adapters (SDO, CANopenNode wrapper, private handshakes, a UART poller in `process`) |
| `command_route` | Which adapter executes a `hmi_command_t`; App sends commands through the synchronous command port |
| `periodic`, `periodic_count` | Periodic frames encoded from the snapshot (see [runtime](../runtime/runtime-production.md)) |
| `app_run`, `app_tick`, `app_reset` | Product workflow each runtime iteration; reset on every new session |
| `local_action` | `HMI_ACTION_PRODUCT` intents from the UI or MSH |
| `ui` | LVGL factory, called only on the UI thread |
| `storage`, `allow_parameter_extension` | Settings record identity, debounce and append-only schema growth |

Every callback except the UI factory runs on the single runtime thread, so Product state needs no locks. Callbacks must not block: blocking peripherals (MCU UART, camera, audio, haptics) get their own Product thread and exchange data through a Product-owned queue that an adapter's `process` or `app_tick` drains. Publish values computed locally (timers, counters, peripheral readings) with `hmi_snapshot_write()`, and a device model detected at run time with `hmi_snapshot_profile()`.

## Layout and layering

Every Product uses the same directories, so reading one Product prepares you for the next; how files are split inside a directory is up to the Product.

| Directory | Contents | Framework directories it may include |
|---|---|---|
| `product/` | Assembly only: `product.c` (`hmi_product_t`), `firmware.c`, `sources.json`, `tests.cmake`, optional `product.cmake`, `lv_conf.h`, version files | all contracts |
| `catalog/` | Ids and tables of signals, settings, faults, actions | `contracts/` |
| `protocol/` | Bus protocols: decode tables, protocol adapters, periodic-frame encoders, generated protocol data | `contracts/`, `protocols/common/` |
| `app/` | Business rules and state: `app_tick`/`local_action` in the runtime thread, presentation data | `contracts/` |
| `components/` | Optional: reusable mechanisms without file-scope state, configuration as first argument | `contracts/` |
| `platform/`, `board/` | Optional: peripheral adapters and blocking hardware workers; board role mapping | SDK and OS |
| `ui/` | LVGL pages and the UI factory; reaches business logic only through the Product's presentation data and actions | `contracts/`, `ui/common/` |
| `i18n/` | `languages.json`, `translations/`, `generated/` | — |
| `sim/` | Host entry `main.c`, with OS stand-ins in `sim/host/` when needed | `sim/` |
| `tests/`, `tools/`, `assets/`, `generated/`, `docs/` | Tests, generators and gates, assets, generated sources, documents | — |

`tools/check_architecture.py` enforces the includes of `ui/`, `app/` (and the older name `application/`) and `protocol/` from this table. Names describe responsibility and carry no customer or model prefix (`settings_get`, `application_tick`, `PRODUCT_SIGNAL_*`); external contracts (update identity, NVM namespace, protocol ids, language ids, asset paths) keep their values. Product tests guard external behaviour (frames, persistence, composition, the UI starting) and generated-source sync; design-fidelity and document checks stay as scripts run by hand. Two code gates are the same for every Product: `tools/check_product_format.py` (clang-format; `format_exclude` in `sources.json` skips vendored code and generator output) and `tools/check_product_static.py` (cppcheck with the MISRA addon's checkable subset; `static` in `sources.json` names the scanned directories, excluded prefixes and whether every baselined finding needs a recorded deviation; baseline and deviation register live in the Product's `tools/`).

## Source and dependency declarations

`product/sources.json` declares the shared source groups plus optional `rtthread` (firmware only), `host_support` (linked into the host Application only), `include_dirs` (both builds), and `host_include_dirs` (host only). Paths must stay inside the Product and exist; include entries must be directories. Keep hardware sources and host substitutes in separate groups. The host build always starts at the Framework root; an optional `product/product.cmake` is included after every Product target exists, for third-party warning flags, host dependencies (such as FreeType) and resource directories. The header named by `lvgl.config` also layers after `sim/lv_conf.h` on the host; host-only `LV_*` options go in its `#ifndef __RTTHREAD__` branch.

Firmware can declare `lvgl.config` as a Product-relative configuration header. It is included after the SDK `lv_conf.h` for every translation unit (through `platform/rtthread/lvgl/lv_conf.h`, which `#include_next`s the SDK file, so SDK sources stay untouched); use it for native `LV_*` options, and `#undef` the ones the SDK already defines before redefining them (for example `LV_USE_FREETYPE`). The UI runs in the SDK LVGL thread, so its stack budget is `LPKG_LVGL_THREAD_STACK_SIZE`.

The optional CAN TX port `stop()` accepts a one-way silence request. The existing TX worker rejects new submissions, discards queued frames and selects the controller's native listen mode after the current driver write returns. It keeps RX buffers open and does not resume sending on a protocol session reset. Acceptance is asynchronous; verify silence and absence of ACK on the board.

A route with `id = HMI_CAN_ID_ANY` handles data frames without an exact route on the same bus and with the same standard/extended format. Exact routes always win. Declare at most one fallback for each bus/format pair; the fallback does not admit malformed or remote frames. Use it only when a protocol or raw-frame monitor needs otherwise unknown identifiers.

## Hardware differences

| Need | Where |
|---|---|
| Other CAN controller, EEPROM location, file-based settings, no exclusive display layer, own backlight control | Board defconfig, see [board integration](../build/board-integration.md) |
| Resolution, portrait layout, touch | SDK panel and touch configuration; Product UI layout |
| CANopenNode, private protocols, several buses | Product adapters and routes on `HMI_BUS_CAN0`/`HMI_BUS_CAN1` |
| MCU serial link, camera, vibration, light sensor | Product code and threads; set `AIC_HMI_DISPLAY_EXCLUSIVE=n` for video and `AIC_HMI_BACKLIGHT=n` for automatic brightness |

## Product-sized domain storage

The platform declares no capacity for your catalog. You declare the arrays and bind them when you init the core:

```c
static hmi_value_t signals[PRIVATE_SIGNAL_SLOTS];
static float parameters[PRIVATE_PARAMETER_SLOTS];
static hmi_fault_state_t faults[PRIVATE_FAULT_SLOTS];
const hmi_core_storage_t storage = {signals, PRIVATE_SIGNAL_SLOTS, parameters, PRIVATE_PARAMETER_SLOTS,
                                      faults, PRIVATE_FAULT_SLOTS};
hmi_core_init(&core, &private_catalog, &storage);
```

`hmi_core_init` fails if any table is larger than the storage you bind. Firmware composition needs three such storages (runtime, published copy, UI copy). Identities are 16-bit handles resolved through your catalog, never array positions; private extensions start at `HMI_ID_PRIVATE_FIRST` (`0x1000`). Read values with `hmi_snapshot_read`, `hmi_snapshot_parameter` and `hmi_snapshot_fault_active`.

The settings size comes from `hmi_settings_size()`. `version.tool` supplies the display version and `identity.hardware` supplies the board identity in build diagnostics.

### Product language resources

New templates include a single-language manifest, translations and a generation check, with one
registration function shared by host and firmware. An explicit `catalog.language_count` of 1 allows
only ID 0; zero retains the legacy two-language default. Declare the language count, ID meanings and
first-boot default together rather than inferring them from Framework defaults.

The Framework provides a translation **service** (`ui/common/i18n/hmi_i18n_runtime.h`); all wording belongs to the Product, and the Framework ships no visual widgets or text of its own. Screens are built from LVGL's native widgets in the Product.

```text
key  →  language catalog (static arrays, generated)  →  text
```

- **Catalog.** Generate a static lv_i18n language pack: one `lv_i18n_lang_t` table per language (the source language first, it is the fallback), and plural tables for every `<key>.one` / `<key>.other` pair. There is no runtime JSON or file parsing; keep the source in the Product and generate C from it. `python tools/generate_i18n.py --product-root products/my-product` does this from `i18n/languages.json` plus one flat `<code>.json` per language and checks the language numbering, the key set, `{n}` placeholders, missing translations (an error unless the language says `complete: false`), glyph coverage of the declared font files (Arabic as LVGL shapes it) and literal `hmi_i18n_tr("...")` keys; `--check` fails when the committed output is stale. A Product with richer sources (customer tables, bindings) may keep its own generator as long as it emits the same pack and language table. The lookup runtime is the vendored `ui/common/i18n/lv_i18n` copy; LVGL 9.1 (the SDK version) has no `lv_translation`.
- **Language table.** The Product owns the numbering: `hmi_i18n_register_languages(table, count)` takes one table whose entry *i* has id *i* (the persisted language byte), a unique code that is also the lv_i18n locale name, and optional label/value faces. It replaces the built-in default (`0` en, `1` zh-CN), has no size cap and must outlive LVGL. Ids are append-only; reordering them reinterprets every saved setting.
- **Register** before any screen is built, from the UI factory's `create()` (the one hook both the firmware and the host runner call) and/or `hmi_firmware_composition_t.locale_init`: the language table, then `hmi_i18n_register_pack()` with the Product's one pack. Registration is idempotent and lasts one LVGL lifetime after `hmi_i18n_init()`. Both runners check that every id `hmi_catalog_t.language_count` accepts is registered.
- **One source of truth.** The persisted `snapshot.language` selects the language. The main loop calls `hmi_i18n_set_language()` before `present()`; the service then re-translates every label bound with `hmi_i18n_label_set_tag()` / `hmi_i18n_bind_text()`, re-resolves bound faces and sends `HMI_EVENT_LANGUAGE_CHANGED` to objects registered with `hmi_i18n_on_language_change()`. A plain `lv_label_set_text()` after a tag ends the text binding.
- **Bind, do not rebuild.** `hmi_i18n_bind_text(label, tag, role)` binds the translation tag and the language-dependent face in one call; `hmi_i18n_bind_font()` binds only the face. A language without a face for the role gives back the face the object was designed with.
- **Fallback.** `hmi_i18n_tr()` answers with the selected language, then the English column of registered packs, then the tag itself. Generate complete matrices so a bound label never shows a tag. `hmi_i18n_find()` is the lookup without fallback (`NULL` on a miss, no LVGL warning), for optional overrides such as a protocol-specific wording of a shared key.
- **Templates and plurals.** `{0}`..`{9}` positional placeholders are expanded by `hmi_i18n_format()` from typed arguments (`hmi_i18n_arg_u/s/str`), never by printf. A plural text is the base tag plus `.one` / `.other`; `hmi_i18n_tr_plural()` picks the form with the selected language's `hmi_plural_rule_t`.
- **More than two languages.** `hmi_language_t` stays the persisted byte; its meaning comes from the Product's language table, and `hmi_catalog_t.language_count` declares how many ids the catalog accepts. A Product that switches language only at restart (faces created per language) reports the language it is showing in `snapshot.language`, not a setting written since boot.
- **Faces.** Prefer LVGL's own fallback chain (`lv_font_t.fallback`) over per-language branches: give a language face the default face of the same role as fallback for the glyphs it lacks. LVGL's Arabic shaping draws presentation forms; `hmi_i18n_font_isolated_forms()` answers the isolated forms a face lacks with the base letter's glyph.
- **Overflowing text.** Prefer one policy for every translated label, e.g. `LV_LABEL_LONG_SCROLL_CIRCULAR`, over per-screen layout changes.

### Product font inputs

The Framework ships no font tooling and no font files: a Product keeps its own OTF/TTF files, their license texts, the generator and the generated C (the Hangcha Product and the Demo each carry `assets/fonts/SourceHanSansSC-Normal.otf` and `tools/generate_fonts.py`, which build the 14/20 px CJK subsets from the characters of the Product text and record source hash, converter and file hashes in `assets/fonts.json`; `--check` verifies them without Node). Fonts reach the UI through the `label_font` / `value_font` of the language table.
