# anolis-provider-bread — TODO

Items here are explicitly deferred and intentional. They are not omissions or accidents.

## Hardware CI

- [ ] Add self-hosted CI runner for the Linux hardware path (`ci-linux-hardware-release`) against real BREAD devices.
      The no-hardware CI lane (Linux + Windows, no hardware) covers all unit and shell tests.
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

## Clang-Format / Tidy

- [ ] Configure clang-format and add a CI check or pre-commit hook.
      Deferred until the codebase is more stable and a format policy decision is made.

## Additional BREAD Device Types

- [ ] Add adapters for future BREAD device types as new contracts land in `bread-crumbs-contracts`.
      Each new type requires: contracts header, compatibility entry, adapter, and tests.
      See [CONTRIBUTING.md](CONTRIBUTING.md) for the step-by-step guide.

## Dependency / CVE Scanning

- [ ] Add automated dependency and CVE scanning workflow.
      Third-party dependencies (protobuf, yaml-cpp, gtest) come through vcpkg; CRUMBS and bread-crumbs-contracts
      are first-party AGPL-3.0. A Dependabot or Trivy scan would catch upstream CVEs.
