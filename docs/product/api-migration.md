# Migrating a Product to Luban-Lite HMI

> [中文版](api-migration.zh-CN.md)

The project `forklift-meter-platform` became **Luban-Lite HMI Framework** (repository `boa-z/luban-lite-hmi`). The rename changes identifiers only; behaviour, persisted data and wire formats are unchanged. A Product that compiled against the old Framework needs the edits below. Products are separate repositories and are not updated automatically.

## Names

| Old | New | Notes |
|---|---|---|
| `application/rt-thread/forklift-meter-platform` | `application/rt-thread/luban-lite-hmi` | SDK application directory; `CONFIG_PRJ_APP="luban-lite-hmi"` |
| `meter_*`, `METER_*` (C API, CMake functions/targets, Python tools) | `hmi_*`, `HMI_*` | Every Framework-defined symbol, header and source file, e.g. `contracts/meter_product.h` → `contracts/hmi_product.h`, `meter_product_t` → `hmi_product_t` |
| `CONFIG_AIC_FORKLIFT_METER_PLATFORM_APP` | `CONFIG_AIC_HMI_APP` | Kconfig application switch |
| `CONFIG_AIC_FORKLIFT_*` | `CONFIG_AIC_HMI_*` | Board resources, update, watchdog, display options |
| `-DMETER_PRODUCT_ROOT`, `-DMETER_BUILD_UI`, `-DMETER_ENABLE_UPDATE`, `-DMETER_LVGL_ROOT` | `-DHMI_…` | Host CMake options |
| Environment `METER_PRODUCT_ROOT`, `METER_DISPLAY_VERSION`, `METER_VERSION_SEQUENCE`, `METER_BOARD_ID`, `METER_PLATFORM_ROOT` | `HMI_…` | Build and test environment |
| `meter-demo`, `meter-protocol-runner` | `hmi-sim`, `hmi-protocol-runner` | Host executables |
| Generated `output/<project>/meter/` (`meter_build_identity.h`, `meter_update_build.h`, `meter_display_version.h`) | `output/<project>/hmi/` (`hmi_…h`) | The automatic daily `V<n>` counter starts again in the new directory |
| MSH `meter info\|diag\|…`, `meter_color`, `meter_display`, `meter_testcard`, `meter_settings`, `meter_exec`, `meter_watchdog` | `hmi …`, `hmi_color`, … | `hmi_color`, `hmi_display` and `hmi_testcard` later moved to Luban-Lite Display Lab |
| Log tags `meter.*`, thread names `meter_app`, `meter_tx0`, … | `hmi.*`, `hmi_app`, `hmi_tx0`, … | Serial log filters must follow |

## What deliberately did not change

- The Demo repository name `boa-z/forklift-meter-platform-demo` and Product-owned names (for example the Demo's `meter_gauge_*` widgets).

## Updating a Product

Replace the Framework identifiers in the Product sources, CMake files, scripts and docs; Product-owned `meter_*` names stay as they are. A token-exact replacement (only identifiers the Framework defines) is safe; a blind `meter_` → `hmi_` would also rename Product names. Then rebuild the generated i18n/CAN files with the Framework tools and run the host tests.

## Product contract changes after the rename

These make the Framework usable by HMIs that are not CAN instruments. Existing Products keep working unless they used the two removed items.

| Change | What a Product does |
|---|---|
| `hmi_monitor_def_t` and `hmi_catalog_t.monitors`/`monitor_count` removed | A monitor page is Product UI: keep the row table in the Product (the Demo now generates `demo_monitors[]`) |
| `HMI_SOURCE_DEMO` removed | Define your own source identities (the Demo uses `DEMO_SOURCE_CAN`) |
| `protocols`, `routes` optional | Leave them unset when the Product talks to no bus; no CAN bus is opened then |
| Catalog tables may all be empty; `hmi_firmware_composition_t.locale_init` optional | Nothing to do |
| `hmi_snapshot_write(snapshot, id, value, now_ms)` added | Use it in `app_tick`/`local_action` instead of a hand-written "publish local value" helper |
| `product/sources.json` groups optional on the host too | `protocol` (and any other group) can be omitted |
| `create_product.py` makes a CAN-free template; `--can` adds the DBC decoder | Nothing to do for existing Products |

The Framework no longer provides a settings record format, storage worker, EEPROM backend or file backend. Each Product owns its storage and compatibility policy.
