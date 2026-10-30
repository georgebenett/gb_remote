# Security

This firmware controls a vehicle. A flaw that lets someone else drive, stop
or confuse a board can hurt its rider, so please report it privately.

**Report** through GitHub: *Security* tab → *Report a vulnerability*. Do not
open a public issue for anything that affects throttle, braking, pairing or
the BLE link. Expect an answer within a week.

**In scope**: BLE pairing and bonding, the remote ↔ receiver protocol
(`firmware/main/gb_protocol.h`), throttle and brake handling, the USB
configuration protocol, firmware update paths.

**Pairing model**: the remote only connects to receivers it has bonded with,
except during a pairing window (no bond yet, `CMD_START_PAIRING` over USB, or
enabling dual connection). The receiver only accepts remotes on its allowlist
and only acts on writes from an encrypted link. The static passkey is
`CONFIG_GB_BLE_PASSKEY`; its default is public, so set your own for builds you
ship (on both remote and receiver).
