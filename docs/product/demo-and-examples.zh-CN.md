# Demo Product 管理

> [English](demo-and-examples.md)

## 选择参考实现

| 包 | 归属 | 用途 | 构建选择 |
|---|---|---|---|
| `products/demo` | 独立公开 Product 仓库，克隆到 `products/demo` | 完整合成仪表、双语 LVGL UI、协议、设置和 Product 文档 | `-DHMI_PRODUCT_ROOT=products/demo`（主机）或 `HMI_PRODUCT_ROOT=products/demo`（固件） |

完整开发规范参考 [Demo Product 手册](../../products/demo/README.zh-CN.md)及其 [SDL2 实际截图](../../products/demo/docs/ui/screenshots.zh-CN.md)。每个固件只链接一个 Product。

## 归属与范围

Framework 文档描述可复用契约、构建工具与集成。Demo 的需求、协议、UI、资源、截图和验证属于 Demo 仓库；客户对应资料属于私有 Product。不要在 Framework `docs/` 中复制这些文档。

Framework 不携带示例 Product。用于回归或可替换性检查的第二个 Product 放在独立仓库，通过 `HMI_PRODUCT_ROOT` 选择；需要它的 Framework 测试从 `tools/product_template` 临时生成。

## 开发与更新 Product

1. 构建前初始化 Framework 的 submodule，并把 Product 仓库克隆到 `products/<id>`。
2. 在 Product 仓库内部建立开发分支。
3. 实现修改，同时维护双语需求、协议/UI 说明和测试。
4. 通过 Framework 在专用构建目录编译，运行 Product 与 Framework 测试。
5. UI 变更导出 SDL2 实际帧，并目视检查两种语言。记录渲染输入、代码身份、可执行文件哈希及图片哈希。
6. 在 Product 自己的仓库提交并推送；Framework 不记录 Product 的任何信息。

```text
git clone https://github.com/boa-z/forklift-meter-platform-demo.git products/demo
git -C products/demo switch -c codex/demo-change
cmake -S . -B build-demo -G Ninja -DCMAKE_BUILD_TYPE=Debug -DHMI_PRODUCT_ROOT=products/demo
cmake --build build-demo --parallel
ctest --test-dir build-demo --output-on-failure
```

每个 Product 使用独立构建目录。`HMI_PRODUCT_ROOT` 选择源码清单，不是运行时 Product 菜单。仓库结构与 Product 契约见[Product 接入指南](downstream.zh-CN.md)。

## Product 文档验收

标准 Product 提供根 README，并在 `docs/` 下分别管理需求、架构/协议、构建运行、UI 和验证。字体与图标许可证随 Product 素材管理。明确标记未确认的协议语义，不把模拟值当成客户默认参数。

UI 图集必须来自实际渲染器，覆盖 unknown/stale 状态、导航页面和两种语言。PNG 转换可保留像素，但不能重绘或缩放证据。截图只证明所捕获的布局；测试证明其命名的软件行为，上板验收还需单独标识固件、板卡及原始记录。

共享双语检查器支持独立 Product 根目录：

```text
python tools/check_docs_sync.py --root products/demo
```

Product 专项检查维护在 `product/tests.cmake`。私有仓库可关闭 Actions，仅在本地运行这些检查。
