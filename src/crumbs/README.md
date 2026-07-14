# CRUMBS Session Layer

`src/crumbs/` holds the provider-owned CRUMBS session boundary.

Contains:

- `session.*`: generic retry, query-read, scan, and error-normalization logic
- `linux_transport.*`: Linux and CRUMBS-backed transport adapter compiled only when hardware is enabled
- non-hardware session tests driven through a fake transport

BREAD semantics should stay above this layer. This code owns bus/session behavior, not RLHT/DCMT meaning.
Wire-format knowledge lives below it: frame geometry in CRUMBS (`crumbs_frame_length`, decode) and
payload layouts in `bread-crumbs-contracts` (`*_parse_state_payload`, version/caps parsers). The
contracts' `_get_*` round-trip helpers are deliberately not used here — they hardcode the query
delay and carry no locking or retry, which `Session` provides.
