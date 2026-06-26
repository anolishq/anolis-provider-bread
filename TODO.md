# anolis-provider-bread — TODO

Items here are explicitly deferred and intentional. They are not omissions or accidents.

## Hardware CI

- [ ] Add self-hosted CI runner for the Linux hardware path against real BREAD devices, running the
      provider with a real `hardware.bus_path` (a non-`mock://` device node) instead of a `mock://` bus.
      The no-hardware CI lane (Linux + Windows, `mock://` bus) covers all unit and shell tests.
      Hardware-in-the-loop validation requires a dedicated runner; deferred until hardware lab is available.

## ARM64 Support

- [ ] Validate and add `ci-linux-arm64-release` preset and CI lane.
      The provider makes no architectural assumptions, but an ARM64 vcpkg triplet and CI runner are needed
      to confirm. Deferred until ARM64 deployment target is confirmed.

## CRUMBS Session Extraction

- [ ] Evaluate extracting `src/crumbs/` into a standalone `crumbs-host` or `crumbs-session` library once
      a second real CRUMBS-family consumer exists.
      Do not extract without a concrete second consumer. The session layer is clean and extractable, but
      premature packaging without a real use case creates maintenance burden without benefit.

## Additional BREAD Device Types

- [ ] Add adapters for future BREAD device types as new contracts land in `bread-crumbs-contracts`.
      Each new type requires: contracts header, compatibility entry, adapter, and tests.
      See [CONTRIBUTING.md](CONTRIBUTING.md) for the step-by-step guide.
