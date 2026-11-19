# Framework documentation

> [中文版](README.zh-CN.md)

Choose the task you need to complete, then open its module. Operator guides are separated from technical references.

## Start with a task

- [Build the first Product](build/build.md)
- [Run the simulator](build/simulator.md)
- [Integrate an independent customer Product](product/downstream.md)
- [Inspect UART and runtime diagnostics](runtime/diagnostics.md)

## Browse by module

| Directory | Contents | Entry |
|---|---|---|
| `build/` | Build and board integration | [Module index](build/README.md) |
| `product/` | Product composition and services | [Module index](product/README.md) |
| `runtime/` | Runtime, protocols and storage | [Module index](runtime/README.md) |

## Maintaining documentation

Place new documents in their module rather than directly in this directory. Keep English and Chinese files together, and update module indexes and cross-links in the same change.

Operator guides follow: goal/artifacts → prerequisites → steps and pass criteria → troubleshooting → terms → technical references. State where commands run and which result fields to check.
