# CRUMBS Session Layer

`src/crumbs/` holds the provider-owned CRUMBS session boundary.

Contains:

- `session.*`: generic retry, query-read, scan, and error-normalization logic
- `linux_transport.*`: Linux and CRUMBS-backed transport adapter compiled only when hardware is enabled
- `LinuxTransport::bind_device(...)`: bridge for `bread-crumbs-contracts` so hardware adapters use BREAD helpers instead of rebuilding RLHT/DCMT wire logic
- non-hardware session tests driven through a fake transport

BREAD semantics should stay above this layer. This code owns bus/session behavior, not RLHT/DCMT meaning.