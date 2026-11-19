# Reference platform migration

> [中文版](migration.zh-CN.md)

This repository starts with a clean Git history. Its application code, example protocol, catalogs and UI are original reference implementations created from functional requirements. No proprietary product source, assets, capture, document, object dictionary or Git history is imported.

## Original extraction sequence
1. Establish independent contracts, bounded protocol runtime, callback routing and product composition.
2. Add a synthetic demo protocol and small generated domain catalogs.
3. Compose an original four-page LVGL UI from reusable instruments.
4. Validate host, SDL, dependency boundaries, provenance and public export.
5. Run on the SDK's own LVGL 9 package (GE2D, MPP, display and touch ports) in an isolated RT-Thread SDK build.

Private inventory and donor baselines remain outside this repository. Existing private products are not silently rewritten or asserted compatible; see downstream.md for their explicit integration procedure.

## Module disposition
| Public module | Provenance | Role |
|---|---|---|
| contracts | Original | Values, frames, profiles, snapshots and persistence interface |
| core | Original | Single-writer business state and domain validation |
| protocols/common | Original | Bus + frame-format + identifier route lookup |
| products/demo/protocol and products/demo/generated/can | Original | Synthetic DBC source, domain mapping and generated codec/adapter |
| runtime | Original | Portable routing, batches, requests, lifecycle and periodic TX helpers |
| products/demo | Original | Capability, binding, routes, UI and policy composition |
| ui/common | Original | Translation runtime and the display test card, without product wording; visual widgets live in Products (the Demo's in `products/demo/ui/widgets`) |
| products/demo/ui and products/demo/assets | Original | Product-owned pages, navigation, translation pack and font subset |
| platform | Original | Host and RT-Thread adapters |
| LVGL | SDK `lvgl-ui` package (9.1) | Rendering and board adaptation |
