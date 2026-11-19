# Product 接入

> [English](downstream.md)

## 仓库

每个 Product，无论公开还是私有，都是克隆到 `products/<product>`（或用绝对路径选择）的独立 Git 仓库。框架忽略 `products/*`，不记录 Product gitlink，也不需要私有 fork：所有 Product 都基于公开框架的 `main` 构建。客户代码、协议、UI、素材与需求都留在 Product 仓库。

```text
git clone PRODUCT_REPOSITORY products/my-product
cmake -S . -B build-my-product -G Ninja -DHMI_PRODUCT_ROOT=products/my-product
```

固件在板级 defconfig 里用 `CONFIG_AIC_HMI_PRODUCT_ROOT="products/my-product"` 指定 Product。没有任何机制固定 Product 提交，所以每次发布构建都要记录 Product、框架与 SDK 的提交。新 Product 用 `python tools/create_product.py --id my-product --output products/my-product` 起步：模板只有目录、LVGL UI 和一段本地业务（`app_tick`），不打开任何总线；加 `--can` 会带上一路由 DBC 生成的 CAN 解码与路由。接入新 Product 不需要修改框架源码。不要在 `contracts`、`core`、`runtime`、`protocols/common` 或 `ui/common` 里加入客户符号；缺少的通用能力向框架提出。

## Product 提供什么

Product 可以提供两个可选启动回调：`prepare()` 在 Core 初始化之前恢复业务状态和初始设置，
`start()` 在平台设备就绪后、UI 创建和协议推进之前启动外设服务。每次进程启动各调用一次，
返回 false 会终止启动。没有这些需求的 Product 留空即可。固件、UI 主机运行器与无界面的协议运行器
均遵守此顺序；协议运行器的 `--catalog` 只准备目录与设置，不启动设备服务。

Product 设置负责默认值、合法范围、存储值修复和运行时效果。Core 直接复制 `prepare()` 提供的设置，
不重复这些规则；目录与存储容量检查继续保留。自有设置存储的 Product 不绑定 `storage`，
自行控制背光时关闭 `AIC_HMI_BACKLIGHT`。Framework 仍在打开总线前锁存配置的 CAN 速率，
并检查设备操作结果。

启动回调不访问 LVGL，也不在协议首帧、首个 tick 或页面创建时补调用。
`hmi_firmware_compose()` 只装配存储，`locale_init()` 只注册语言资源；二者不承担设置恢复和业务启动。
UI 主机的合成数据源在 start 之后才推进。Product 自己启动的硬件 worker 必须遵守自己的状态同步边界。

`hmi_product_get()` 返回一个静态的 `hmi_product_t`（`contracts/hmi_product.h`）。只有 `id` 与 `catalog` 是必需的，目录里的各表都可以为空。不接任何总线的 Product 不设 `protocols`、`routes` 与 `periodic`，运行时就不打开 CAN 总线，也不启动 CAN 发送线程。

| 字段 | 用途 |
|---|---|
| `catalog`、`initial_settings` | 信号、本机设置、故障；首次开机的语言、亮度、单位，CAN Product 还有总线速率。页面怎样排列信号（监视列表等）属于 Product 的 UI |
| `protocols`、`routes` | 接收路由到无状态 DBC 解码或有状态适配器（SDO、CANopenNode 包装、私有握手、在 `process` 里轮询的串口） |
| `command_route` | 由哪个适配器执行 `hmi_command_t`；App 经同步命令端口发送命令 |
| `periodic`、`periodic_count` | 按快照编码的周期帧（见[运行时](../runtime/runtime-production.zh-CN.md)） |
| `app_run`、`app_tick`、`app_reset` | 每轮运行时的 Product 业务；每个新会话开始时复位 |
| `local_action` | 来自 UI 或 MSH 的 `HMI_ACTION_PRODUCT` 意图 |
| `ui` | LVGL 工厂，只在 UI 线程调用 |
| `storage`、`allow_parameter_extension` | 设置记录身份、去抖以及只增参数的格式扩展 |

除 UI 工厂外，所有回调都在唯一的运行时线程里执行，所以 Product 状态不需要加锁。回调不得阻塞：会阻塞的外设（MCU 串口、摄像头、音频、振动）由 Product 自己开线程，通过 Product 自己的队列交换数据，再由适配器的 `process` 或 `app_tick` 取走。本地算出的值（计时、计数、外设读数）用 `hmi_snapshot_write()` 发布，运行中识别到的设备型号用 `hmi_snapshot_profile()` 发布。

## 目录与分层

所有 Product 用同一套目录，读一个 Product 就能读懂下一个；目录内怎么拆文件由 Product 决定。

| 目录 | 内容 | 可以包含的 Framework 目录 |
|---|---|---|
| `product/` | 只做装配：`product.c`（`hmi_product_t`）、`firmware.c`、`sources.json`、`tests.cmake`，可选 `product.cmake`、`lv_conf.h`、版本文件 | 全部契约 |
| `catalog/` | 信号、设置、故障、动作的 id 与目录表 | `contracts/` |
| `protocol/` | 总线协议：解码表、协议适配器、周期帧编码、协议生成物 | `contracts/`、`protocols/common/` |
| `app/` | 业务规则与状态：运行时线程里的 `app_tick`/`local_action`、呈现数据 | `contracts/` |
| `components/` | 可选：无文件级状态、配置作首参的可复用机制 | `contracts/` |
| `platform/`、`board/` | 可选：外设适配与会阻塞的硬件 worker；板级角色映射 | SDK 与 OS |
| `ui/` | LVGL 页面与 UI 工厂；只经 Product 的呈现数据与动作接触业务 | `contracts/`、`ui/common/` |
| `i18n/` | `languages.json`、`translations/`、`generated/` | — |
| `sim/` | 主机入口 `main.c`，需要时 `sim/host/` 放 OS 替身 | `sim/` |
| `tests/`、`tools/`、`assets/`、`generated/`、`docs/` | 测试、生成器与门禁、素材、生成物、文档 | — |

`tools/check_architecture.py` 按上表检查 `ui/`、`app/`（与旧名 `application/`）和 `protocol/` 的包含关系。命名按职责，不带客户或型号前缀（`settings_get`、`application_tick`、`PRODUCT_SIGNAL_*`）；外部契约（NVM 命名空间、协议 id、语言编号、资源路径）保持原值。Product 测试只守对外行为（报文、持久化、组合、界面能起来）与生成物同步，设计稿保真与文档检查留给手动脚本。两个代码门禁对所有 Product 相同：`tools/check_product_format.py`（clang-format，`sources.json` 的 `format_exclude` 跳过原样引入的代码与生成物）和 `tools/check_product_static.py`（cppcheck + MISRA 可检子集，`sources.json` 的 `static` 声明扫描目录、排除前缀与是否要求逐条偏差登记，基线与偏差登记在 Product 的 `tools/`）。

## 源文件与依赖声明

`product/sources.json` 在共用源文件组之外，可声明 `rtthread`（仅固件）、`host_support`（仅链接进 Host Application）、`include_dirs`（两端共用）和 `host_include_dirs`（仅 Host）。路径必须位于 Product 内且真实存在，头文件路径必须是目录。硬件源文件和主机替身分组列出。主机构建总是从 Framework 根目录开始；Product 可选的 `product/product.cmake` 在全部 Product 目标建立后被包含，用来补充第三方源码的告警、主机依赖（如 FreeType）与资源目录。`lvgl.config` 指定的头文件在主机上同样叠加在 `sim/lv_conf.h` 之后，主机专用的 `LV_*` 选项放在它的 `#ifndef __RTTHREAD__` 分支里。

固件可通过 `lvgl.config` 指定 Product 内的配置头；所有翻译单元在 SDK 的 `lv_conf.h` 之后包含它（经 `platform/rtthread/lvgl/lv_conf.h`，它用 `#include_next` 取 SDK 的文件，SDK 源码不改），用来叠加原生 `LV_*` 选项；SDK 已经定义的选项要先 `#undef` 再重新定义（例如 `LV_USE_FREETYPE`）。UI 在 SDK 的 LVGL 线程里运行，栈预算使用 `LPKG_LVGL_THREAD_STACK_SIZE`。

CAN TX 端口可选的 `stop()` 接受单向静默请求。已有 TX worker 拒绝新提交、丢弃排队帧，在当前驱动写返回后选择控制器原生 listen 模式；保留 RX 缓冲区，不因协议会话复位而恢复发送。请求被接受不表示硬件已完成切换，静默及无 ACK 仍需上板验证。

`id = HMI_CAN_ID_ANY` 的路由接收同一总线、同一标准/扩展格式下没有精确路由的数据帧；精确路由始终优先。每个总线/格式组合最多一个后备路由；后备路由不放行畸形帧或远程帧。仅在协议或原始报文监视需要未知标识符时使用。

## 硬件差异

| 需求 | 位置 |
|---|---|
| 换 CAN 控制器、EEPROM 位置、用文件存设置、不独占显示层、自己控制背光 | 板级 defconfig，见[板级接入](../build/board-integration.zh-CN.md) |
| 分辨率、竖屏布局、触摸 | SDK 屏幕与触摸配置；Product UI 布局 |
| CANopenNode、私有协议、多条总线 | Product 在 `HMI_BUS_CAN0`/`HMI_BUS_CAN1` 上的适配器与路由 |
| MCU 串口、摄像头、振动、光感 | Product 代码与线程；视频要设 `AIC_HMI_DISPLAY_EXCLUSIVE=n`，自动亮度要设 `AIC_HMI_BACKLIGHT=n` |

## 按 Product 定容的域存储

平台不为你的目录声明容量。你自己声明数组并在初始化 core 时绑定：

```c
static hmi_value_t signals[PRIVATE_SIGNAL_SLOTS];
static float parameters[PRIVATE_PARAMETER_SLOTS];
static hmi_fault_state_t faults[PRIVATE_FAULT_SLOTS];
const hmi_core_storage_t storage = {signals, PRIVATE_SIGNAL_SLOTS, parameters, PRIVATE_PARAMETER_SLOTS,
                                      faults, PRIVATE_FAULT_SLOTS};
hmi_core_init(&core, &private_catalog, &storage);
```

任一表格大于绑定的存储时 `hmi_core_init` 失败。固件组装需要三份这样的存储（运行时、发布副本、UI 副本）。标识是经目录解析的 16 位句柄，不是数组下标；私有扩展从 `HMI_ID_PRIVATE_FIRST`（`0x1000`）开始。用 `hmi_snapshot_read`、`hmi_snapshot_parameter` 与 `hmi_snapshot_fault_active` 读取。

设置容量由 `hmi_settings_size()` 决定。`version.tool` 提供 `display` 版本字段，`identity.hardware` 提供构建诊断中的板卡身份。

### Product 语言资源

新建模板自带单语言清单、译文与生成检查，主机和固件使用同一注册函数。
`catalog.language_count` 显式设为 1 时只允许编号 0；设为 0 表示沿用旧目录的两语言默认值。
语言数量、编号含义和首次启动默认语言应一起声明，不应靠 Framework 的默认语言推断。

框架只提供翻译**服务**（`ui/common/i18n/hmi_i18n_runtime.h`），措辞全部属于 Product，框架不带可视控件，也不带自己的文字。界面由 Product 用 LVGL 原生控件搭建。

```text
key  →  language catalog (static arrays, generated)  →  text
```

- **目录**：生成静态的 lv_i18n 语言包：每种语言一张 `lv_i18n_lang_t` 表（源语言排第一，作回退语言），每对 `<键>.one` / `<键>.other` 进入复数表。运行时没有 JSON 或文件解析；来源留在 Product 里，生成 C 数组。`python tools/generate_i18n.py --product-root products/my-product` 从 `i18n/languages.json` 与每种语言一份扁平的 `<code>.json` 生成，并检查语言编号、键集合、`{n}` 占位符、缺失译文（语言未声明 `complete: false` 时为错误）、所声明字体文件的字形覆盖（阿拉伯文按 LVGL 字形变换后检查）以及 `hmi_i18n_tr("...")` 字面键；`--check` 在已提交的生成文件过期时失败。来源更复杂（客户表、绑定关系）的 Product 可以保留自己的生成器，只要产出同样的翻译包与语言表。 查表运行时是 vendored 的 `ui/common/i18n/lv_i18n` 副本；SDK 的 LVGL 9.1 没有 `lv_translation`。
- **语言表**：编号属于 Product：`hmi_i18n_register_languages(table, count)` 接收一张表，第 *i* 项的 id 就是 *i*（持久化的语言字节），带唯一的包列名、可选的标签/数值字体和复数规则。它替换内置默认（`0` en、`1` zh-CN），没有数量上限，必须活得比 LVGL 久。编号只能追加；重排会让每份已存设置换了含义。
- **注册**：在建任何界面之前，从 UI 工厂的 `create()`（固件与主机运行器都会调用的唯一钩子）和/或 `hmi_firmware_composition_t.locale_init` 注册：先语言表，再对每个包调用 `hmi_i18n_register_pack()`。注册幂等，`hmi_i18n_init()` 之后在一个 LVGL 生命周期内有效。两个运行器都会检查 `hmi_catalog_t.language_count` 接受的每个 id 都已注册。
- **唯一来源**：持久化的 `snapshot.language` 决定语言。主循环在 `present()` 之前调用 `hmi_i18n_set_language()`，LVGL 随后向每个对象发送 `LV_EVENT_TRANSLATION_LANGUAGE_CHANGED`。
- **绑定而不是重建**：`hmi_i18n_bind_text(label, tag, role)` 一次绑定翻译键与随语言变化的字体；`hmi_i18n_bind_font()` 只绑字体。语言没有该角色的字体时，对象恢复设计时的字体。
- **回退**：`hmi_i18n_tr()` 先取当前语言，再取已注册包的英文列，最后返回标签本身。生成完整矩阵，绑定的标签就不会显示键名。`hmi_i18n_find()` 是不回退的查找（找不到返回 `NULL`，不产生 LVGL 警告），用于可选覆盖，例如共享键的协议专用措辞。
- **模板与复数**：`{0}`–`{9}` 位置占位符由 `hmi_i18n_format()` 按带类型的参数（`hmi_i18n_arg_u/s/str`）展开，不经 printf。复数文字是基标签加 `.one` / `.other`，`hmi_i18n_tr_plural()` 按当前语言的 `hmi_plural_rule_t` 选形式。
- **多于两种语言**：`hmi_language_t` 仍是持久化字节，含义由 Product 的语言表决定；`hmi_catalog_t.language_count` 声明目录接受多少个 id。只在重启时切换语言的 Product（字体按语言创建）在 `snapshot.language` 里报告正在显示的语言，而不是开机后写入的设置。
- **字体**：优先用 LVGL 自带的回退链（`lv_font_t.fallback`），不要按语言写分支：语言字体缺的字由同角色的默认字体补。LVGL 的阿拉伯字形变换画的是呈现形；`hmi_i18n_font_isolated_forms()` 用基本字母的字形补上字体缺的孤立形。
- **溢出文字**：给所有译文标签一个统一策略（例如 `LV_LABEL_LONG_SCROLL_CIRCULAR`），不要逐页改版式。

### Product 字体输入

框架不带字体工具，也不带字体文件：字体由 Product 自己管理——OTF/TTF 源文件、许可证文本、生成脚本和生成的 C 都在 Product 里（Hangcha Product 与 Demo 各自带 `assets/fonts/SourceHanSansSC-Normal.otf` 和 `tools/generate_fonts.py`，按 Product 文字里的字符生成 14/20 px 的中文子集，并把来源哈希、转换器版本和生成文件哈希记在 `assets/fonts.json`；`--check` 不需要 Node 即可校验）。字体通过语言表的 `label_font` / `value_font` 交给界面。
