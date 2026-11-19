# Build and Product guide

## Prerequisites

Run host commands from the Framework root with Python 3, CMake, Ninja and a C/C++ compiler; UI builds also need SDL2 development files. Initialize the pinned dependencies and install host test tools:

```text
python -m pip install -r tools/protocol/requirements.txt -r tools/serial/requirements.txt pytest
```

Keep the SDK-bundled Python/SCons/toolchain unchanged. SDK OneStep may invoke Python 2.7, while host generation, tests use Python 3.

## Choose a Product

Every firmware contains exactly one Product. A Product is a directory with `product/sources.json`; the manifest lists the source closure for `application`, `protocol`, `catalog`, `ui`, `product`, `firmware` and optional `ui_binding` groups. Host CMake has no default Product: it builds and tests the framework alone until `-DHMI_PRODUCT_ROOT` names one. A firmware build has no default either: an unset or blank `HMI_PRODUCT_ROOT` stops the build instead of silently selecting a Product.

The reference Demo is maintained in the public [`boa-z/forklift-meter-platform-demo`](https://github.com/boa-z/forklift-meter-platform-demo) repository. It is optional and not a submodule: clone it into `products/demo` to build it. Every Product is its own repository under `products/<id>`, which the Framework ignores; see [products/README.md](../../products/README.md).

Create a starting point (catalog, LVGL UI and local business; `--can` adds a DBC-generated CAN decoder and routes) and inspect its closure:

```text
python tools/create_product.py --id my-product --output ../my-product
python tools/firmware_product.py --product-root ../my-product
```

The resolver applies the firmware rule, so an unfinished selection fails during pre-flight with the list of selectable roots instead of during the build:

```text
python tools/firmware_product.py
python tools/firmware_product.py --product-root products/my-product
```

Keep customer protocol tables, assets and fonts in a private Product repository. The public Framework must contain only synthetic or reference data.

## Host build

Use a separate build directory for each Product:

```text
python -m pip install -r tools/protocol/requirements.txt
cmake -S . -B build-demo -G Ninja -DHMI_PRODUCT_ROOT=products/demo -DHMI_BUILD_UI=ON -DBUILD_TESTING=ON
cmake --build build-demo --parallel
ctest --test-dir build-demo --output-on-failure
```

`HMI_PRODUCT_ROOT` may be an absolute path to an external private Product. Never configure two Products in one build directory.

## Luban-Lite SDK build

The SDK owns the board, bootloader, partition map and toolchain. The Framework owns the Product source selection. On Windows, start `win_cmd.bat`, then select the Framework defconfig and build with the normal OneStep commands:

```text
lunch 12
m
```

The Product is stored in the defconfig, so there is no per-shell setup. Put `CONFIG_AIC_HMI_PRODUCT_ROOT="products/demo"` in the Framework's defconfig (or set *Luban-Lite HMI Framework -> Product root for the firmware build* in `scons --menuconfig`); a relative value resolves against the Framework directory and an absolute path selects an external Product. `lunch` applies the defconfig, so the choice follows the configuration you pick. The `HMI_PRODUCT_ROOT` environment variable still overrides the defconfig for a single build, for example `set HMI_PRODUCT_ROOT=%CD%/application/rt-thread/luban-lite-hmi/products/demo`; the build prints a line when it does. If neither is set the build stops and lists the initialized Products instead of defaulting to the Demo.

Run these commands from the SDK root. `12` is an example menu index; run `list` and select the board's `rt-thread_luban-lite-hmi` configuration using its displayed index. `m` builds the matching bootloader first and then the application. Firmware selection resolves relative Product paths against the Framework directory, so an environment override run from the SDK root needs an absolute path.

Before compiling, the application manifest prints the resolved Product root, the Product identity, the public board alias and the source count. The Product line also states where the choice came from (`defconfig` or `env`). Confirm those four lines belong to the Product you asked for; a stale shell keeps the previous `HMI_PRODUCT_ROOT`, which overrides the defconfig. The generated `output/<project>/hmi/hmi_build_identity.h` records the same identity as `HMI_BUILD_PRODUCT` and `HMI_BUILD_PRODUCT_REVISION`, so an archived image states which Product it was composed from. Switching Products inside one SDK tree leaves the previous Product's object files on disk; they are not linked, but they make `git status`, link maps and evidence confusing, so run `scons -c` after changing the selection.

## Firmware display version

Set `HMI_DISPLAY_VERSION` or declare `version.tool` in `product/sources.json`. The script accepts `--date YYMMDD --sequence N --format json`. The Framework reads the `display` field, up to 47 ASCII characters.

The daily number `V<n>` is automatic by default. The build fingerprints the platform, SDK, LVGL and Product (commits and uncommitted changes), the SDK configuration and the build options. While the fingerprint is unchanged since the last successful build, the number is kept; when anything changed it goes up by one; a new day starts at 1. The state is stored in `output/<project>/hmi/version-state.json` and written only after a build succeeds, so failed builds and commands that only read the configuration do not consume a number. To fix the number yourself, set `CONFIG_AIC_HMI_VERSION_SEQUENCE=<1-99>` in the defconfig (0 means automatic) or `HMI_VERSION_SEQUENCE=<1-99>` for one build; the next automatic build continues from it. SCons prints `METER sequence` with the number and why it was chosen.

## Validation boundaries

Host CMake/CTest validates contracts and synthetic transport. A successful build is not board acceptance. For board work, preserve the exact SDK revision, Product revision, image hash, UART startup log, CAN trace and post-reboot identity.
