# Framework 文档

> [English](README.md)

先选择你要完成的任务，再进入对应模块。操作指南面向使用者，技术说明单独维护。

## 从任务开始

- [构建第一个 Product](build/build.zh-CN.md)
- [运行模拟器](build/simulator.zh-CN.md)
- [接入独立客户 Product](product/downstream.zh-CN.md)
- [查看串口和运行诊断](runtime/diagnostics.zh-CN.md)

## 按模块浏览

| 目录 | 内容 | 入口 |
|---|---|---|
| `build/` | 构建与板端集成 | [模块索引](build/README.zh-CN.md) |
| `product/` | Product 组合与服务 | [模块索引](product/README.zh-CN.md) |
| `runtime/` | 运行时、协议与诊断 | [模块索引](runtime/README.zh-CN.md) |

## 文档维护

新增文档放入对应模块，不再平铺到本目录。中英文文件在同一文件夹成对维护；各模块索引与交叉链接一起更新。

操作文档按“目标与产物 → 前提 → 步骤与通过判据 → 排错 → 术语 → 技术说明”组织。命令注明执行位置，结果注明检查字段。

迁移保留原有测试 SHA、镜像身份和验收边界。历史验证不能直接归因于后续提交。
