# Luban-Lite HMI Framework

> [English](README.md)

面向匠芯创 Luban-Lite SDK（RT-Thread + LVGL 9）的轻量级 HMI 应用框架。工业仪表、触摸控制屏、设备状态终端、CAN 仪表等多个独立 Product 共用同一套应用运行基础设施，不必各自重写。每个固件镜像只构建一个 Product。框架只面向 Luban-Lite，不追求跨厂商、跨 RTOS。

## 职责划分

| 层 | 负责 |
|---|---|
| Luban-Lite SDK / BSP | 板级配置、pinmux、驱动（CAN、I2C、PWM、RTC、WDT、显示、触摸）、RT-Thread、带 GE2D/MPP 的 LVGL、Flash 分区和 ENV |
| Framework（本仓库） | 每个 Product 都需要的应用运行设施：一个承载 Product 回调的运行时线程与发布给 UI 的快照、在 SDK LVGL 线程里启动、设置持久化（EEPROM 或文件，去抖、掉电安全）、背光与显示色彩、对运行时与 UI 线程的看门狗监督、翻译服务、诊断命令、主机模拟器与构建工具。可选：CAN 路由、每总线发送 worker 与周期帧 |
| Product（独立仓库） | 目录、页面与资源、文案、通信协议（CAN、UART、MCU 链路）、业务规则、SDK 未覆盖的外设、发布身份 |

Product 只需要目录和 UI；CAN、持久化都是声明了才启用。板级事实来自 SDK defconfig（[板级接入](docs/build/board-integration.zh-CN.md)）；Product 放在各自的仓库里（[Product 接入](docs/product/downstream.zh-CN.md)）；[SDK 复用](docs/runtime/reuse.zh-CN.md)记录框架从 SDK 取用了什么、其余部分为何保留。

## 主机仿真（Host）

```sh
python -m pip install -r tools/protocol/requirements.txt
cmake -S . -B build -G Ninja -DHMI_BUILD_UI=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Linux CI 使用 `ubuntu-latest`。SDL 运行时可设置 `SDL_VIDEODRIVER=dummy`。

## 产品包（Product package）

参考 Demo 是独立仓库 `boa-z/forklift-meter-platform-demo`，克隆到 `products/demo`。Framework 不跟踪任何 Product：公开或私有 Product 都是独立仓库，克隆到 `products/<id>`（Framework 忽略该目录，见 [products/README.zh-CN.md](products/README.zh-CN.md)），或用绝对路径选择。配置其他 Product：

```sh
cmake -S . -B build-private -DHMI_PRODUCT_ROOT=/path/to/product
```

创建只有目录、LVGL UI 和本地业务的 Product 模板（加 `--can` 带上由 DBC 生成的 CAN 解码）：

```sh
python tools/create_product.py --id my-product --output ../my-product
```

Product manifest、SDK OneStep 构建方式见[构建与 Product 指南](docs/build/build.zh-CN.md)。

## 固件边界（Firmware boundary）

Platform 公共代码位于 `contracts/`、`core/`、`runtime/`、`protocols/common/`、`ui/common/`；RT-Thread 端口（一个运行时线程加发送、NVM 与看门狗 worker）在 `platform/rtthread/`，见[生产运行时](docs/runtime/runtime-production.zh-CN.md)。固件运行在 SDK 自带的 LVGL 9 包（`lvgl-ui`，含 GE2D 绘制后端、MPP 解码、显示与触摸移植）上，Product UI 由 SDK 的 LVGL 线程启动；主机仿真与测试直接构建同一份 SDK 里的 LVGL 源码（`HMI_LVGL_ROOT` 可改路径）。翻译查表运行时是 vendored 的 `ui/common/i18n/lv_i18n` 副本。自带显示、触摸与 GE2D 层的 LVGL 9.6 移植在 `dev/lvgl-9.6` 分支。

## 文档（Documentation）

项目文档为中英双语：每个第一方文档同时提供 `X.md`（英文正本）与 `X.zh-CN.md`（简体中文译文），更新时必须同步修改两边。

配置时传 `-DHMI_PRODUCT_ROOT=`（空值）可在没有任何 Product 的情况下构建和测试框架。

## 诊断

参阅[诊断与串口调试](docs/runtime/diagnostics.zh-CN.md)：原生 ULog/MSH 查询、结构化 Trace、构建身份和 raw UART 证据流程。

当前架构、操作指南和验证记录入口见[文档索引](docs/README.zh-CN.md)。
