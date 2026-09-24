# On-demand browser dashboard

This optional Linux example keeps a dedicated Barista radio available and
starts a browser only while a paired Wii U GamePad is connected. It includes
an X11-to-AppHook video bridge and maps GamePad touch and basic buttons to
browser input. Home Assistant is one possible dashboard.

## Lifecycle

| State | Radio | Browser |
| --- | --- | --- |
| Adapter absent | Stopped | Stopped |
| No saved pairing | Stopped until an explicit pairing request | Stopped |
| Paired, GamePad asleep | Running | Stopped |
| GamePad connected | Running, encoding active | Running |
| Disconnected less than 45 seconds | Running, encoding stopped | Kept for reconnect |
| Disconnected after grace period | Running | Stopped |
| Adapter unplugged | Stopped | Stopped |

The manager polls a local UNIX socket. It needs no Home Assistant automation,
network listener, or manual wake button. Container resource limits are ceilings;
stopped containers reserve no VM memory. Actual standby use depends on the
adapter and driver and has not yet been measured on physical hardware.

## Requirements

- An always-running Linux host with systemd, Python 3.9+, Docker Engine and
  the Docker Compose plugin.
- An exclusively assigned compatible 5 GHz Wi-Fi adapter capable of hosting
  Barista's AP. Client-mode compatibility alone is insufficient.
- The adapter's PHY must be visible in the same network namespace as the Docker
  daemon. In an LXC deployment, assign the dedicated PHY to that container
  separately; this example does not move or enroll host hardware.
- Network access from the browser container to your dashboard.

The radio's `network_mode: host` refers to the Docker daemon's host namespace.
It is limited to NET_ADMIN, NET_RAW, CHOWN and FOWNER. FOWNER is required for
AppHook socket permissions after assigning the socket to the browser user.
The browser runs as UID 1000, drops all capabilities, has a private X11 display,
and receives no radio access or Docker socket. Both root filesystems are
read-only. Chromium's own sandbox is disabled for the nested-container example;
container isolation supplies the boundary. The systemd manager runs as root
and controls Docker, so its code and configuration must be administrator-owned.

## Install

Keep the complete fork checkout at `/opt/barista`; the example builds the
engine and bridge directly from that checkout. The service and CLI use
`/opt/barista/examples/on-demand` as their runtime directory.

```sh
sudo git clone --branch wake-on-demand https://github.com/Unintendedsideeffects/Barista.git /opt/barista
cd /opt/barista/examples/on-demand
sudo install -m 0600 .env.example .env
sudo install -m 0644 runtime/manager.json.example manager.json
```

Edit `.env` to set `GAMEPAD_URL` to your dashboard. Set `interface` in
`manager.json` to the dedicated adapter's current interface name. The manager
passes this value to Compose. The default name is `wlangamepad`; the example
does not rename adapters or select one automatically.

The same JSON config sets `disconnect_grace_seconds` (45),
`pairing_timeout_seconds` (180), and `retry_seconds` (60).

Build and start the manager:

```sh
sudo docker compose --profile on-demand build
sudo sh scripts/install-runtime.sh
sudo gamepadctl status
```

The build includes the engine/core tests. Installation creates private state
directories, enables the systemd service, and starts only what the lifecycle
requires. It will wait if the configured adapter is missing.

### Dashboard authentication

The example ships an empty browser profile. Use a dedicated, minimally
privileged dashboard account and sign in yourself. A pre-authenticated Chromium
profile may be prepared on a trusted desktop using a separate user-data
directory, with the same dashboard URL, and copied into
`state/browser/profile` while both the desktop browser and manager are stopped.
Use a compatible Chromium version and restore UID/GID 1000 ownership afterward.
Session portability depends on the browser and dashboard authentication method.
The profile is private application data and must never be committed or shared.

For local provisioning or debugging, `GAMEPAD_DEBUG=1` enables Chromium's
DevTools endpoint on the browser container's loopback only. No port is
published. Connect from inside that container using a local debugging client;
the endpoint gives access to the authenticated browser. Remove the setting and
recreate the dashboard container when finished. No dashboard credentials or
trusted-network authentication rules are installed by this example.

## Pair and use

Pairing is deliberate; no automatic pairing is attempted:

```sh
sudo gamepadctl pair 0123
sudo gamepadctl status
```

The digits are an example pairing code, not a stored password. Wait until the
state is `pairing_ready`, then press SYNC and enter the `pairing_symbols`
reported by the engine. A request accepted by the manager does not mean the
radio is ready. Pairing times out after 180 seconds.

```sh
sudo gamepadctl cancel-pair
sudo gamepadctl pause
sudo gamepadctl resume
```

After pairing, powering on the pad establishes the connection that starts the
browser. A reconnect within the grace period reuses the existing browser.
Adapter removal stops both containers; reinsertion restores operation once
the same interface is available. Radio failures trigger bounded retries.

The original upstream desktop service must not simultaneously own this radio.
The manager is the lifecycle owner of the two example containers.

## Private runtime data

- `.env`: local dashboard URL and optional debugging flag.
- `manager.json`: local interface and lifecycle settings.
- `state/radio/credentials.conf`: saved pairing credentials.
- `state/browser/profile`: dashboard cookies and session data.
- `state/paused`: persistent pause marker.
- `run/`: control/media sockets and local status, which can contain local paths.

These paths are excluded from Git and the Docker build context. Do not put
authentication tokens in `GAMEPAD_URL`. Container logs remain local and rotate;
inspect and redact them before sharing.

## Development and acceptance

Existing manager lifecycle checks can be run with:

```sh
python3 -m unittest discover -s tests -v
```

The optional Docker `validation` target includes an AppHook frame probe for
checking video transport without a physical GamePad. The sample bridge submits
864x480 frames at approximately 15 fps and maps touch, A/B, and the D-pad.
The application layout and physical input mapping still need device acceptance.

See [validation status](../../docs/standby.md#validation-status) for prior
software checks and outstanding hardware checks. On actual hardware, confirm
pairing, visible dashboard, touch input, disconnect shutdown, reconnect, adapter
removal/reinsertion, wake latency, and standby/active CPU and memory use.

Stop automatic operation with:

```sh
sudo systemctl disable --now barista-gamepad-manager.service
```

The manager stops both containers during shutdown and preserves private state.
