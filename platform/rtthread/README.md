# RT-Thread adapter contract

> [中文版](README.zh-CN.md)

This directory is the only place that touches RT-Thread devices and ArtInChip SDK APIs: the execution port (one runtime thread, per-bus TX threads), the board port (display takeover, CAN devices), backlight, EEPROM and NVM worker, RTC, watchdog and the MSH front ends. Device names and board facts come from the Kconfig "Board resources" menu. RT-Thread and ArtInChip types never appear in `contracts`, `core`, `runtime` or `ui/common`.

Keep the board configuration, pinmux, panel, touch device, partitions and bootloader in the SDK and select the application's devices in the board's defconfig; see [board integration](../../docs/build/board-integration.md).
