# Demo Product management

> [中文版](demo-and-examples.zh-CN.md)

## Choose a reference

| Package | Ownership | Purpose | Build selection |
|---|---|---|---|
| `products/demo` | Independent public Product repository, cloned into `products/demo` | Complete synthetic instrument, bilingual LVGL UI, protocol, settings and Product docs | `-DHMI_PRODUCT_ROOT=products/demo` (host) or `HMI_PRODUCT_ROOT=products/demo` (firmware) |

Use the [Demo Product handbook](../../products/demo/README.md) and its [actual SDL2 gallery](../../products/demo/docs/ui/screenshots.md) as the full development reference. Each firmware links exactly one Product.

## Ownership and scope

Framework documentation describes reusable contracts, build tools and integration. Demo requirements, protocol, UI, assets, screenshots and validation belong in the Demo repository. Customer equivalents belong in their private Product. Do not duplicate those documents under Framework `docs/`.

The Framework carries no example Products. A second Product for a regression or replaceability check belongs in its own repository and is selected with `HMI_PRODUCT_ROOT`; Framework tests that need one generate it from `tools/product_template`.

## Develop and update a Product

1. Initialize the Framework submodules and clone the Product repository into `products/<id>` before building.
2. Create a branch inside the Product repository.
3. Implement the change with its bilingual requirements, protocol/UI notes and tests.
4. Build through Framework using a dedicated build directory; run Product and Framework tests.
5. For UI changes, export actual SDL2 frames and visually inspect both languages. Record renderer inputs, code identity, executable hash and image hashes.
6. Commit and push the Product in its own repository; the Framework records nothing about it.

```text
git clone https://github.com/boa-z/forklift-meter-platform-demo.git products/demo
git -C products/demo switch -c codex/demo-change
cmake -S . -B build-demo -G Ninja -DCMAKE_BUILD_TYPE=Debug -DHMI_PRODUCT_ROOT=products/demo
cmake --build build-demo --parallel
ctest --test-dir build-demo --output-on-failure
```

Use a separate build directory per Product. `HMI_PRODUCT_ROOT` selects a source manifest, not a runtime Product menu. See the [Product integration guide](downstream.md) for repository layout and the Product contract.

## Product documentation acceptance

A standard Product provides a root README and `docs/` sections for requirements, architecture/protocol, build/run, UI and validation. Keep fonts and icon licenses with Product assets. Name unresolved protocol semantics explicitly. Do not treat simulator values as customer defaults.

The UI gallery contains real renderer output, including unknown/stale states, navigation pages and both languages. PNG conversion may preserve pixels but must not redraw or resize evidence. Screenshots prove only the captured layout; tests establish named software behaviors, and board acceptance requires a separately identified firmware, board and trace.

The shared bilingual checker accepts an independent Product root:

```text
python tools/check_docs_sync.py --root products/demo
```

Maintain Product-specific checks in `product/tests.cmake`. Private repositories may run these checks locally with Actions disabled.
