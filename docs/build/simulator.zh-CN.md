# 仿真器契约

> [English](simulator.md)

`hmi-sim` 拥有 LVGL 循环，并在有限次运行后在 stdout 上输出一个 JSON 对象。仅当对象数量保持稳定、所选场景完成且可选捕获成功时，才以零状态退出。

| 选项 | 含义 |
|---|---|
| `--smoke` | 240 帧隐藏 CI 运行和导航检查 |
| `--hidden --frames N` | 无窗口的有限 SDL 运行 |
| `--scenario normal\|warning\|stale\|offline\|error\|unknown` | 合成状态 |
| `--visual min\|mid\|max` | 速度/SOC/转向视觉测试夹具 |
| `--page 0..3` | 打开 Dashboard、Monitor、Faults 或 Settings |
| `--capture path.bmp` | 保存当前渲染帧 |
| `--set-language english\|chinese` | 设置语言，仅本次运行有效 |
| `--set-units metric\|imperial` | 设置本次运行的单位 |

报告包含 `frames`、对象数量、堆高水位、平均/最大呈现时间、运行时分发诊断、有效故障数及其 `fault_ids` 标识、有效性状态、值、页面、`language`（`en` 或 `zh-CN`）以及保留的 `ge_hits`/`sw_fallbacks` 字段。GE2D 值保持为 `null`，因为主机目标使用 LVGL 软件渲染。

默认语言为英语。设置页和命令行选项仅更改本次运行的偏好。不支持的语言参数以状态码 2 退出。

Linux CI 在 `ubuntu-latest` 上运行，使用 SDL2 和 `SDL_VIDEODRIVER=dummy`。场景测试在所有页面验证两种语言。`i18n-ui` 测试在英语与中文之间来回切换时检查每个标签的缺失字形，包括未知/过期/错误状态。

字体子集使用声明的 `lv_font_conv@1.5.3` MIT 开发依赖通过 `npm run fonts:generate` 生成（仅重新生成时需要 Node.js/npm）。常规构建使用已提交的 C 源码，无需下载字体。`npm run fonts:check` 或 `python3 tools/generate_fonts.py --check` 无需 Node 或网络即可验证翻译覆盖率、来源、许可证和哈希。
