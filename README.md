# Luban-Lite HMI Framework

> [中文版](README.zh-CN.md)

A lightweight HMI application framework for the ArtInChip Luban-Lite SDK (RT-Thread + LVGL 9). Several independent Products — industrial displays, touch control panels, device status terminals, CAN instruments — share one application runtime instead of each rebuilding it. Each firmware image builds exactly one Product. The Framework is made for Luban-Lite only; it is not a cross-vendor or cross-RTOS platform.

## Who owns what

| Layer | Owns |
|---|---|
| Luban-Lite SDK / BSP | Board configuration, pinmux, drivers (CAN, I2C, PWM, RTC, WDT, display, touch), RT-Thread, LVGL with GE2D/MPP, Flash partitions and ENV |
| Framework (this repository) | The application runtime every Product needs: one runtime thread with the Product callbacks and a published snapshot for the UI, startup from the RT-Thread main thread, optional UI in the SDK LVGL thread, backlight and display colour, watchdog supervision of the runtime and UI threads, translation service, diagnostics shell, host simulator and build tools. Optional: CAN routing, per-bus transmit workers and periodic frames |
| Product (own repository) | Catalog, pages and assets, wording, communication protocols (CAN, UART, MCU links), business rules, peripherals the SDK does not cover, release identity |

A Product needs only a catalog and a UI; CAN and persistence are switched on by declaring them. Board facts come from the SDK defconfig ([board integration](docs/build/board-integration.md)); Products live in their own repositories ([Product integration](docs/product/downstream.md)); [SDK reuse](docs/runtime/reuse.md) records what the Framework takes from the SDK and why the rest stays.

## Host

```sh
python -m pip install -r tools/protocol/requirements.txt
cmake -S . -B build -G Ninja -DHMI_BUILD_UI=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Linux CI uses `ubuntu-latest`. Set `SDL_VIDEODRIVER=dummy` for SDL runs.

## Product package

The reference Demo is the independent repository `boa-z/forklift-meter-platform-demo`, cloned into `products/demo`. The Framework tracks no Product: every Product, public or private, is its own repository cloned into `products/<id>` (ignored by the Framework, see [products/README.md](products/README.md)) or selected by an absolute path:

```sh
cmake -S . -B build-private -DHMI_PRODUCT_ROOT=/path/to/product
```

Scaffold a Product with a catalog, an LVGL UI and local business only (add `--can` for a DBC-generated CAN decoder):

```sh
python tools/create_product.py --id my-product --output ../my-product
```

See the [Build and Product guide](docs/build/build.md) for Product manifests, SDK OneStep builds.

## Firmware boundary

Platform common code lives in `contracts/`, `core/`, `runtime/`, `protocols/common/`, `ui/common/`; the RT-Thread port (one runtime thread plus CAN TX and watchdog workers) is in `platform/rtthread/`, see [production runtime](docs/runtime/runtime-production.md). Firmware runs on the SDK's own LVGL 9 package (`lvgl-ui`, with its GE2D draw backend, MPP decoder, display and touch ports) and starts the Product UI from the SDK LVGL thread; the host simulator and tests build that same SDK LVGL source (`HMI_LVGL_ROOT` overrides the path). The lookup runtime for translations is the vendored `ui/common/i18n/lv_i18n` copy. The LVGL 9.6 port with its own display, touch and GE2D layer lives on the `dev/lvgl-9.6` branch.

## Documentation

Project documentation is bilingual: every first-party document ships `X.md` (English) and `X.zh-CN.md` (Simplified Chinese); update both together.

Configure with `-DHMI_PRODUCT_ROOT=` (empty) to build and test the framework without any Product.

## Diagnostics

See [Diagnostics and serial debugging](docs/runtime/diagnostics.md) for native ULog/MSH queries, structured trace, build identity and raw UART evidence.

See the [documentation index](docs/README.md) for current architecture, operating guides and validation records.
