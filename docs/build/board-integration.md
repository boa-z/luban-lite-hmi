# Board integration

> [中文版](board-integration.zh-CN.md)

## Responsibilities

| Layer | Owns |
|---|---|
| SDK / BSP (`target/<chip>/<board>/`, defconfig) | Pinmux, clocks, panel, touch, PWM backlight channel, CAN and I2C controllers, partition table, bootloader |
| Framework Kconfig "Board resources" (same defconfig) | Which devices the application uses: CAN device per logical bus, display layer ownership, backlight curve |
| Product | Protocols, routes, periodic frames, UI, settings catalog, update identity and UDS addressing, extra peripherals |

A new board needs a defconfig, not Framework code. A Product never names a device: it addresses CAN by role (`HMI_BUS_CAN0`, `HMI_BUS_CAN1`).

## Board resources

| Option | Default | Meaning |
|---|---|---|
| `AIC_HMI_CAN0_DEVICE` / `AIC_HMI_CAN1_DEVICE` | `can0` / `can1` | RT-Thread CAN device behind each logical bus; a bus opens only when the Product uses it |
| `AIC_HMI_DISPLAY_EXCLUSIVE` | y | Turn the video layer and extra UI rectangles off and force UI alpha 255 before LVGL starts; say n when the Product shows camera or video |
| `AIC_HMI_BACKLIGHT` | y (needs `AIC_PWM_BACKLIGHT`) | Drive the SDK backlight channel `AIC_PWM_BACKLIGHT_CHANNEL` from the brightness setting; say n when the Product controls the backlight |
| `AIC_HMI_BACKLIGHT_PERIOD_NS`, `AIC_HMI_BACKLIGHT_FLOOR_PERCENT` | 20000, 10 | Duty = floor + brightness × (100 − floor) / 100 |
| `AIC_HMI_DISPLAY_GAMMA`, `AIC_HMI_DISPLAY_CCM` | 100, empty | Panel colour correction, see [diagnostics](../runtime/diagnostics.md) |

The CAN bit rate is a persisted setting (125, 250 or 500 kbit/s) shared by all opened buses; it is latched before the buses open and changes take effect after a restart.

## Display, resolution and orientation

Resolution, rotation and the touch device come from the SDK panel configuration and the SDK LVGL package options (`lvgl-ui`); the Framework does not assume a screen size. The Product UI lays itself out from `lv_display_get_horizontal_resolution()`/`..._vertical_resolution()` of the active display. A portrait variant is a different board defconfig (or Product build), not a Framework change.

The runtime starts from the RT-Thread main thread. With SDK LVGL enabled, `-Wl,--wrap=lv_user_gui_init` registers a timer in the SDK LVGL thread. The timer attaches the Product UI after the runtime publishes its first snapshot. The application layers its LVGL options over the SDK `lv_conf.h`. GE2D is enabled by default and `AIC_HMI_DRAW_GE2D` can disable it for comparison with CPU drawing.


## Build and validation

Build with SDK OneStep as described in the [build guide](build.md); keep the image, ELF/map and the generated `output/<project>/hmi/` headers with the evidence. A successful build or host test does not establish display, touch, physical CAN, durable storage or update acceptance on the board.

## Build isolation

The SDK still shares one active `.config`; build different targets sequentially. The LVGL port stages corrected sources and their objects under `build/<PRJ_NAME>/lvgl/`, with the project name derived from the selected defconfig. This prevents a generated object from one chip ABI being reused by another target. Switching targets does not require removing another target's generated objects. Integration checks use the same target directory. Host builds use separate CMake build directories per Product.
