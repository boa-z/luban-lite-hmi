# Framework development rules

Start with [the documentation map](docs/README.md). Keep behavior-preserving cleanup separate from protocol, timing, ownership, persistence, update trust/recovery and product-policy decisions.

- Prefer existing LVGL features over custom infrastructure. Use native widgets, layout, animation, translation, display and input APIs when they meet the requirement.
- For missing functionality, review maintained mainstream community libraries before writing an equivalent implementation. Pin versions and document license and provenance; do not add a dependency without a concrete need.
- 第一方代码的说明性注释全部使用中文；保留 API 标识符、单位和必要的工具指令原文。新增或修改代码时同步校正相关注释，不改写第三方版权或上游注释。
- Keep project code focused on domain rules, product composition and thin adapters. Do not introduce LVGL or OS dependencies into contracts/core.
- This is Luban-Lite HMI Framework, a runtime shared by HMI Products of any kind (instruments, touch panels, status terminals). Do not put vehicle, CAN-only or page-layout concepts into contracts/core: CAN and persistence stay optional and are enabled by what a Product declares; a Product without CAN must build without CAN code. Public identifiers use `hmi_`/`HMI_`, Kconfig `AIC_HMI_`.
- Do not rename persisted or wire formats (`MSP3`, `FMP2`, `/data/meter-settings`); see docs/product/api-migration.md.
- Each firmware image contains exactly one build-time Product. Select one Product (firmware: `CONFIG_AIC_HMI_PRODUCT_ROOT` in the defconfig, overridable by `HMI_PRODUCT_ROOT`; host CMake: `-DHMI_PRODUCT_ROOT`). Never add a runtime Product registry or bundle multiple Products into one firmware. See [production runtime](docs/runtime/runtime-production.md).
- This is a public framework. Do not import customer protocols, UI, assets, captures or private repository history. Do not name customer companies, product models, project identifiers or private repository URLs in public code, tests or documentation; use generic examples and ArtInChip SoC names.
- Validate with CMake/Ninja and CTest. Host results do not establish board acceptance.
- Documentation is bilingual: every first-party document ships `X.md` (English) and `X.zh-CN.md` (Simplified Chinese); update both together.
- One runtime thread owns Core, protocol adapters and Product callbacks; add a thread only for a call that blocks on hardware (CAN write, EEPROM/file I/O, Flash). Do not add cross-thread request/result machinery, degraded modes or generic event buses without a concrete Product need.
- Board facts (device names, EEPROM location, backlight, display layer policy) belong in the Kconfig "Board resources" menu or the SDK board configuration, never as constants in Framework code; customer behaviour belongs in the Product.
- No SDK source or persistent configuration changes.
