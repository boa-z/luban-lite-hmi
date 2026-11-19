# 构建与 Product 指南

## 构建前准备

Host 命令从 Framework 根目录运行，需要 Python 3、CMake、Ninja 和 C/C++ 编译器；UI 构建还需 SDL2 开发文件。初始化固定版本依赖并安装 Host 测试工具：

```text
python -m pip install -r tools/protocol/requirements.txt -r tools/serial/requirements.txt pytest
```

保持 SDK 自带 Python/SCons/工具链不变。SDK OneStep 可能使用 Python 2.7，Host 生成器、测试 使用 Python 3。

## 选择 Product

每个固件只包含一个 Product。Product 是包含 `product/sources.json` 的目录；manifest 列出 `application`、`protocol`、`catalog`、`ui`、`product`、`firmware` 以及可选 `ui_binding` 的源码闭包。Host CMake 没有默认 Product：在 `-DHMI_PRODUCT_ROOT` 指定 Product 之前只构建并测试框架本身；固件构建同样没有默认值，未设置或空白的 `HMI_PRODUCT_ROOT` 会直接中止构建，而不是静默选择某个 Product。

参考 Demo 维护在公开仓库 [`boa-z/forklift-meter-platform-demo`](https://github.com/boa-z/forklift-meter-platform-demo)，它是可选的，也不是 submodule：需要构建时把它克隆到 `products/demo`。每个 Product 都是 `products/<id>` 下的独立仓库，Framework 忽略该目录，见 [products/README.zh-CN.md](../../products/README.zh-CN.md)。

创建模板（目录、LVGL UI 和本地业务；`--can` 加上由 DBC 生成的 CAN 解码与路由）并检查源码闭包：

```text
python tools/create_product.py --id my-product --output ../my-product
python tools/firmware_product.py --product-root ../my-product
```

解析器施加固件规则，未完成的选择会在构建前就失败，并列出可选的 Product 根目录，而不是等到编译阶段：

```text
python tools/firmware_product.py
python tools/firmware_product.py --product-root products/my-product
```

客户协议表、素材和字体必须放在私有 Product 仓库。公开 Framework 只保留合成数据或参考数据。

## Host 构建

每个 Product 使用独立构建目录：

```text
python -m pip install -r tools/protocol/requirements.txt
cmake -S . -B build-demo -G Ninja -DHMI_PRODUCT_ROOT=products/demo -DHMI_BUILD_UI=ON -DBUILD_TESTING=ON
cmake --build build-demo --parallel
ctest --test-dir build-demo --output-on-failure
```

`HMI_PRODUCT_ROOT` 也可以是外部私有 Product 的绝对路径。不要在同一个构建目录中切换多个 Product。

## Luban-Lite SDK 构建

SDK 负责板级配置、Bootloader、分区和工具链，Framework 负责 Product 源码选择。在 Windows 上启动 `win_cmd.bat`，使用 SDK 的 OneStep 命令：

```text
lunch 12
m
```

Product 保存在 defconfig 中，不需要每个 shell 重新设置。在 Framework 的 defconfig 中写入 `CONFIG_AIC_HMI_PRODUCT_ROOT="products/demo"`（或在 `scons --menuconfig` 的 *Luban-Lite HMI Framework -> Product root for the firmware build* 中设置）；相对路径相对 Framework 目录解析，绝对路径可选择外部 Product。`lunch` 会应用 defconfig，因此选择随所选配置走。环境变量 `HMI_PRODUCT_ROOT` 仍可对单次构建覆盖 defconfig，例如 `set HMI_PRODUCT_ROOT=%CD%/application/rt-thread/luban-lite-hmi/products/demo`，覆盖时构建会打印一行提示。两者都未设置时构建会中止并列出已初始化的 Product，而不会回落到 Demo。

在 SDK 根目录运行这些命令。`12` 是菜单编号示例；先执行 `list`，选择对应板卡的 `rt-thread_luban-lite-hmi` 配置，使用实际显示的编号。`m` 会先构建匹配的 Bootloader，再构建应用。固件选择器相对 Framework 目录解析 Product 相对路径，因此在 SDK 根目录使用环境变量覆盖时需要写绝对路径。

读取配置阶段，应用构建脚本会打印解析后的 Product 根目录、Product 身份、对外板卡别名和源码数量，Product 行还会标明选择来源（`defconfig` 或 `env`）。请先确认这四行就是你要求的 Product；残留的旧 shell 往往还带着上一次的 `HMI_PRODUCT_ROOT`，它会覆盖 defconfig。生成的 `output/<project>/hmi/hmi_build_identity.h` 以 `HMI_BUILD_PRODUCT` 和 `HMI_BUILD_PRODUCT_REVISION` 记录同一身份，归档镜像因此自带“由哪个 Product 组成”的说明。在同一个 SDK 目录内切换 Product 会把上一个 Product 的目标文件留在磁盘上；它们不会参与链接，但会让 `git status`、链接映射和证据变得混乱，所以更换选择后请执行 `scons -c`。

## 固件显示版本

设置 `HMI_DISPLAY_VERSION`，或在 `product/sources.json` 声明 `version.tool`。版本脚本接受 `--date YYMMDD --sequence N --format json`，框架读取输出 JSON 的 `display` 字段（最多 47 个 ASCII 字符）。

当日序号 `V<n>` 默认自动确定。构建会对平台、SDK、LVGL 和 Product（已提交与未提交的改动）、SDK 配置以及构建选项取指纹：与上一次成功构建相比指纹不变就沿用序号，有任何变化加一，换天从 1 开始。状态保存在 `output/<project>/hmi/version-state.json`，只在构建成功后写入，所以失败的构建和只读取配置的命令不会占用序号。需要自己指定时，在 defconfig 里设置 `CONFIG_AIC_HMI_VERSION_SEQUENCE=<1-99>`（0 表示自动），或对单次构建设置环境变量 `HMI_VERSION_SEQUENCE=<1-99>`；之后的自动构建从它继续。SCons 会打印 `METER sequence` 一行，说明序号及其来源。

## 验证边界

Host CMake/CTest 验证契约和合成传输；构建成功不等于实板验收。实板测试应保留准确的 SDK/Product 版本、镜像 hash、UART 启动日志、CAN trace 和重启后身份。
