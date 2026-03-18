# Core

`src/core/` holds provider lifecycle, ADPP transport, and runtime orchestration code.

Contains:

- framed stdio transport for ADPP
- config-backed runtime state
- request handlers for Hello, WaitReady, ListDevices, DescribeDevice, GetHealth, ReadSignals, and Call

