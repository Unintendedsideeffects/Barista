# Persistent standby and wake on demand

This fork adds an opt-in `--standby` flag to the Linux engine. It keeps a
healthy runtime access point available while a paired GamePad is asleep.
Ordinary automatic discovery still cycles its access point unless this flag
is supplied. The flag applies to automatic mode; it has no effect in manual
mode.

The engine already starts its encoder and AppHook streaming when the GamePad
protocol establishes a connection, and stops them on disconnection. Standby
preserves the radio session across the normal discovery timeout and a GamePad
disconnect. An external supervisor can then start an application when the local
engine status reports `phase=runtime` and `connected=1`.

For an already paired GamePad, using a build from this fork:

```sh
sudo ./build/app/drcd --standby --np --interface wlan1
```

Replace `wlan1` with a dedicated, compatible adapter. Pair through the normal
Barista flow first, or use the deliberate pairing command in the example below.
The adapter remains owned by Barista during standby. The host and radio engine
must remain running to receive the connection; this does not wake a powered-off
host or suspended virtual machine.

Runtime hostapd monitor termination, AP disable events, and interface loss are
surfaced as backend failures. The automatic state machine retains its existing
recovery path when the access point fails.

## On-demand dashboard example

[examples/on-demand](../examples/on-demand/README.md) contains a Python manager,
Docker Compose deployment, and an X11/AppHook browser bridge. The radio remains
available; the browser starts on connection and stops after a configurable
45-second disconnect grace period. It can display Home Assistant or another
browser dashboard.

## Validation status

The carried-over engine change previously passed the 25 engine/core tests;
the manager passed 11 lifecycle tests. The original integration also rendered
an authenticated dashboard and passed frames between its browser and AppHook
containers. A simulated Wi-Fi PHY exercised namespace ownership and AP-mode
permissions. These results precede packaging the public example.

Physical pairing, wake latency, display, touch, and real-radio standby resource
usage for this deployment remain pending compatible hardware. Simulated radio
checks and application frames do not establish those results. The public
example uses fresh configuration and contains no authenticated browser profile.

Changes are based on upstream commit
`3bad9db554d71f1ae733d250f64fab0598d97d2b` (build 0.1.47).
The upstream license and attribution are retained.
