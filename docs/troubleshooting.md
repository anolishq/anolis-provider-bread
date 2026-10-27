# Troubleshooting

Common failure modes with their diagnostic signatures and fixes.

## Reading The Logs

All log lines are written to stderr and do not appear on stdout.
The ADPP framed-stdio transport uses stdout exclusively, so logs are always safe to redirect or capture separately.

| Prefix | Stream | Meaning |
|--------|--------|---------|
| `[INFO]` | `stderr` | Normal progress and startup events |
| `[WARN]` | `stderr` | Recoverable failure or degraded state |
| `[ERROR]` | `stderr` | Failed operation, or a condition that leaves the provider not ready; only a config error exits |

---

## Provider Fails To Start

### Config parse error

The provider exits with code 1 before opening the bus.

```text
[ERROR] Failed to parse config '<path>': <yaml exception>
```

or, with one `<field.path>: <message>` line per validation error:

```text
[ERROR] Invalid config '<path>':
hardware.bus_path: is required
```

Use `--check-config` to validate before running:

```bash
./anolis-provider-bread --check-config config/example.local.yaml
```

A valid config prints `[INFO] Config valid: ...` and exits with code 0.

### Host requirements unmet, or the bus will not open

Applies only when `hardware.bus_path` is a real device node (not a `mock://...` path).
The provider does not exit. It stays up with no devices, reports itself not ready, and
says why in the log and in its WaitReady diagnostics (`host_check`, `host_unmet`,
`startup_message`). Its provider health is `DEGRADED`, with the message
`startup degraded: N of N devices failed to initialize` when devices are configured
and the reason itself when none are. Every configured device shows as missing with
that reason.

Before opening the bus it checks what the bus needs from the host (executable
profile v1 §6). An unmet requirement logs:

```text
[ERROR] host requirements unmet: i2c.bus_present: /dev/i2c-1 does not exist
[INFO] serving, not ready (transport=stdio+uint32_le)
```

| Requirement | Unmet means | Fix |
|-------------|-------------|-----|
| `i2c.bus_present` | the device node does not exist | enable the I2C bus on the host and load `i2c-dev` |
| `i2c.bus_access` | the provider's user cannot open it read-write; the detail names the node's group and mode | add that user to the named group, then restart the service |
| `i2c.bus_clock` | the bus's configured clock is above `hardware.max_bus_hz` (checked only when set) | lower the bus clock in the platform's configuration |

`--check-host` runs the same checks without starting, prints the result as JSON on
stdout, and exits 0 (nothing unmet), 1 (something unmet) or 2 (invalid config).
Run it as the user the runtime runs as: root opens any node, so `i2c.bus_access`
reports `unknown` under root.

```bash
sudo -u anolis ./anolis-provider-bread --check-host config/example.local.yaml
```

If the checks pass but the bus still will not open, the log names the open error
instead:

```text
[ERROR] open failed code=open_failed attempts=1 native_code=<errno> message="failed to open I2C bus '/dev/i2c-1': failed to open /dev/i2c-1: <strerror>"
[ERROR] failed to open CRUMBS bus '/dev/i2c-1': failed to open I2C bus '/dev/i2c-1': failed to open /dev/i2c-1: <strerror>
```

To run without hardware, set `hardware.bus_path` to a `mock://...` value.

---

## Device Not Responding During Startup

The probe sequence runs once at startup.
A device that fails the version query, version parse, type check or compatibility check is excluded from the inventory.
A failed capability query falls back to the baseline profile instead (see below).

### Version query fails (timeout or transport error)

```text
[ERROR] query_read failed addr=0x08 code=write_failed attempts=3 native_code=<errno> message="failed to send CRUMBS frame to 0x08: write failed on /dev/i2c-1: <strerror>"
[WARN]  probe 0x08 version failed: <message>
[WARN]  1 unsupported or incompatible probe(s) excluded from inventory
[WARN]    excluded 0x08 status=version_read_failed detail=version query failed: ...
```

The `[ERROR]` line is the raw CRUMBS session failure. Before it, the session logs one `[WARN] query_read failed ... retrying` line for each attempt it retries (two with the default `hardware.retry_count: 2`). The `[WARN]` lines after it are the higher-level probe summary.

Common causes:

- Wrong I2C address in `discovery.addresses`
- Device not powered or not wired
- `hardware.query_delay_us` too low for the device to prepare its reply

Try increasing `query_delay_us` (default 10 000 µs). On a working device this probe always succeeds.

### Unexpected version reply opcode

```text
[WARN] probe 0x08 unexpected version reply opcode 0xNN
```

The device responded but returned an unexpected opcode. Most likely cause is a bus collision or a non-BREAD device at that address.

### Unknown BREAD type ID

```text
[INFO] probe 0x08 unsupported type_id 0x05
[WARN]  excluded 0x08 status=unsupported_type detail=unknown BREAD type_id: 0x05
```

The device probed successfully but its type is not supported by this provider version.
This is not a wiring issue; it is a firmware/software mismatch.
Either update the provider to support the new type or remove the address from the config.

---

## Version Mismatch

Version compatibility is checked after a successful version query.
A mismatch excludes the device from the inventory.

```text
[WARN] probe 0x08 compat failed: <detail>
[WARN]  excluded 0x08 status=incompatible_crumbs_version detail=crumbs version <n> is below minimum
```

The `status` field indicates what was incompatible:

| Status | Meaning | Fix |
|--------|---------|-----|
| `incompatible_crumbs_version` | Device firmware uses a CRUMBS version older than the minimum required by `bread-crumbs-contracts` | Update device firmware |
| `incompatible_module_major` | Module major version mismatch (breaking change) | Update firmware to match the contracts version expected by this provider |

The exact minimum versions expected are defined in `bread-crumbs-contracts`.

---

## GET_CAPS Failure — Baseline Fallback

If the capability query (opcode `0x7F`) fails or returns a parse error, the provider falls back to the baseline capability profile for that device type.

```text
[WARN] probe 0x08 caps query failed (<session error message>), using baseline fallback
```

or when the reply carries another opcode:

```text
[WARN] probe 0x08 unexpected caps reply opcode 0xNN, using baseline fallback
```

or after a successful caps response that cannot be parsed:

```text
[WARN] probe 0x08 caps parse failed, using baseline fallback
```

The device is still added to the inventory. The impact depends on the device type:

| Device | Baseline flags | Practical impact |
|--------|----------------|-----------------|
| RLHT | Mode, setpoint, PID, period, TC select and open-duty control only | `clear_watchdog_trip` and the `estop_count` signal absent from `DescribeDevice`; the command watchdog is not armed |
| DCMT | `OPEN_LOOP \| BRAKE_CONTROL` only | Closed-loop control functions absent from `DescribeDevice`; the command watchdog is not armed |

For DCMT, falling back to baseline offers `set_open_loop`, `set_brake` and `set_mode` (`open_loop` only). `set_setpoint`, the closed-loop modes, `set_pid_x1000`, `set_speed_pid_x1000` and `clear_watchdog_trip` are absent until the provider restarts and its caps query succeeds.

Neither baseline carries the `REFUSES` flag, so on firmware with CRUMBS 1600 or later the device health metric `set_confirmation` reads `delivery_only` instead of `acknowledged`.

If caps queries fail intermittently, check for I2C bus contention or try increasing `hardware.query_delay_us`.

---

## Expected Device Absent — Provider Degraded

When a config `devices:` entry specifies an `id` and `address` but the device is not found or excluded during startup, the provider enters a degraded state.

```text
[WARN] 1 expected device(s) not found in inventory
[WARN]   missing expected device id=dcmt0
```

`GetHealth` returns:

```text
provider.state  = STATE_DEGRADED
provider.message = "startup degraded: 1 of 2 devices failed to initialize"
```

Per missing device:

```text
device_health.device_id = "dcmt0"
device_health.state     = STATE_UNREACHABLE
device_health.message   = "expected device not found during startup"
```

That message appears when no probe ran at the device's address. A device whose probe excluded it shows `probe failed (<status>): <detail>`, and one left unprobed when the start-up budget ran out shows `not probed: the start-up budget was spent`.

`WaitReady` still completes — the provider is technically ready — but `GetHealth` reflects the reduced inventory.

Common causes:

- Hardware not plugged in or not powered
- Mismatch between `address` in config and actual I2C address on the bus
- Device excluded due to a probe failure (check for earlier `[WARN]` or `[ERROR]` lines at the same address)

Fix: correct the `devices:` list in the config or resolve the underlying hardware issue.

---

## Runtime Read And Call Failures

Hardware faults during normal operation are now surfaced on stderr.

### `read_signals` failure

```text
[ERROR] query_read failed addr=0x08 code=timeout attempts=3 native_code=<errno> message="failed to read CRUMBS frame from 0x08: read timed out on /dev/i2c-1"
```

When the errno is `ETIMEDOUT` (110) the session does not retry, so the line shows `attempts=1`. Otherwise each retried attempt first logs a `[WARN] query_read failed ... retrying` line.

The ADPP client receives an error status on the `ReadSignals` RPC. The provider stays running.

### `call` failure

On firmware with CRUMBS 1600 or later, a call reads GET_ACK before it sends, so a failing bus logs a `query_read` failure first:

```text
[ERROR] query_read failed addr=0x09 code=write_failed attempts=3 native_code=<errno> message="failed to send CRUMBS frame to 0x09: write failed on /dev/i2c-1: <strerror>"
```

and the `Call` status reads `DCMT SET command not sent: GET_ACK baseline failed: <message>` (`RLHT SET command` for RLHT).

On older firmware, or when the GET_ACK read succeeded and the write then failed, the log shows:

```text
[ERROR] send failed addr=0x09 code=write_failed attempts=3 native_code=<errno> message="failed to send CRUMBS frame to 0x09: write failed on /dev/i2c-1: <strerror>"
```

The ADPP client receives an error status on the `Call` RPC.

### Common session error codes and fixes

| Code | Meaning | Fix |
|------|---------|-----|
| `timeout` | Device did not respond within `timeout_ms` | Increase `hardware.timeout_ms`; check device power |
| `write_failed`, `read_failed` | I2C bus-level error (EIO, ENXIO, etc.) on a write, or an invalid or truncated reply frame | Check wiring, check I2C pull-ups, check bus speed |
| `decode_failed` | CRUMBS frame failed to decode (CRC mismatch) | EMI or loose connection; check I2C signal quality |
| `already_open` | Session open called twice | Provider logic error; should not occur in normal use |

Persistent errors from a previously healthy device usually indicate a hardware or wiring fault.
Isolated errors that self-resolve are consistent with I2C bus transients; tuning `query_delay_us` can help.

---

## Checking Runtime Health

The `GetHealth` RPC returns a snapshot. Key fields:

```text
provider.state                                  — STATE_DEGRADED if any configured device is missing or the provider is not ready
provider.metrics["startup_initialized_devices"] — number of devices in inventory
provider.metrics["startup_failed_devices"]      — configured devices not in inventory
```

The `WaitReady` diagnostics carry these keys:

```text
diagnostics["device_count"]            — number of devices in inventory
diagnostics["degraded"]                — "true" if ready and any expected device missing
diagnostics["unsupported_probe_count"] — probes excluded at startup
diagnostics["inventory_mode"]          — "manual" or "discovered" on hardware, "config_seeded" in mock mode, "unavailable" when not ready
```

With a `mock://...` `hardware.bus_path`, `inventory_mode` will be `config_seeded` and no I2C activity occurs. All probe-related warnings and errors are hardware-path only.
