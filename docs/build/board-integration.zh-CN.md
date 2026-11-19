# 板级接入

> [English](board-integration.md)

## 职责划分

| 层 | 负责 |
|---|---|
| SDK / BSP（`target/<chip>/<board>/`、defconfig） | 引脚复用、时钟、屏幕、触摸、背光 PWM 通道、CAN 与 I2C 控制器、分区表、bootloader |
| 框架 Kconfig "Board resources"（同一个 defconfig） | 应用用哪些设备：每条逻辑总线对应的 CAN 设备、显示层归属、背光曲线 |
| Product | 协议、路由、周期帧、UI、设置目录、额外外设 |

新板子只需要一个 defconfig，不需要改框架代码。Product 从不写设备名：CAN 按角色寻址（`HMI_BUS_CAN0`、`HMI_BUS_CAN1`）。

## 板级资源

| 选项 | 默认值 | 含义 |
|---|---|---|
| `AIC_HMI_CAN0_DEVICE` / `AIC_HMI_CAN1_DEVICE` | `can0` / `can1` | 每条逻辑总线背后的 RT-Thread CAN 设备；Product 用到该总线时才打开 |
| `AIC_HMI_DISPLAY_EXCLUSIVE` | y | LVGL 启动前关闭视频层与额外 UI 矩形并把 UI alpha 设为 255；Product 要显示摄像头或视频时选 n |
| `AIC_HMI_BACKLIGHT` | y（需要 `AIC_PWM_BACKLIGHT`） | 用亮度设置驱动 SDK 背光通道 `AIC_PWM_BACKLIGHT_CHANNEL`；Product 自己控制背光时选 n |
| `AIC_HMI_BACKLIGHT_PERIOD_NS`、`AIC_HMI_BACKLIGHT_FLOOR_PERCENT` | 20000、10 | 占空比 = 底座 + 亮度 × (100 − 底座) / 100 |
| `AIC_HMI_DISPLAY_GAMMA`、`AIC_HMI_DISPLAY_CCM` | 100、空 | 面板色彩校正，见[诊断](../runtime/diagnostics.zh-CN.md) |

CAN 速率是持久化设置（125、250 或 500 kbit/s），所有已打开的总线共用；它在总线打开前锁存，修改后重启生效。

## 显示、分辨率与方向

分辨率、旋转与触摸设备来自 SDK 屏幕配置和 SDK 的 LVGL 包（`lvgl-ui`）选项；框架不假设屏幕尺寸。Product UI 按当前显示的 `lv_display_get_horizontal_resolution()`/`..._vertical_resolution()` 自行布局。竖屏版本是另一份板级 defconfig（或另一个 Product 构建），不是框架修改。

运行时由 RT-Thread 主线程启动。启用 SDK LVGL 时，链接器选项 `-Wl,--wrap=lv_user_gui_init` 在 SDK 的 LVGL 线程注册定时器。运行时发布首份快照后，定时器接入 Product 界面。应用在 SDK 的 `lv_conf.h` 之上叠加 LVGL 配置。GE2D 默认启用，可用 `AIC_HMI_DRAW_GE2D` 关闭后与 CPU 绘制对照。


## 构建与验证

按[构建指南](build.zh-CN.md)用 SDK OneStep 构建；把镜像、ELF/map 以及生成的 `output/<project>/hmi/` 头文件随证据保存。构建成功或主机测试通过都不能代替板上的显示、触摸、物理 CAN、持久存储验收。

## 构建隔离

SDK 仍共用一个活动 `.config`，不同目标必须串行构建。LVGL 端口将修正源码及其对象放在 `build/<PRJ_NAME>/lvgl/`，工程名由选定 defconfig 决定，避免把一个芯片 ABI 的生成对象复用于另一个目标。切换目标无需删除另一目标的生成对象；集成检查使用同一目标目录。主机构建为每个 Product 使用独立的 CMake 构建目录。
