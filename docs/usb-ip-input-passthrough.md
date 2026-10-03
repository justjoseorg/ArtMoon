# USB/IP input passthrough — locked terminology and design

**Decided 2026-10-03 by Nik and Rias.** This file exists because "host" and "client" mean *opposite* things
depending on which layer you are talking about, and we both got tangled by it. Read this before designing,
naming a variable, or writing UI copy in this area.

## The two machines

| Thing | Machine | Plain meaning |
|---|---|---|
| **ArtMoon** — streaming **client** | mini PC (`niks-minipc`) | Displays the stream. Owns the device list and the toggles in Input settings. |
| **ArtLight** — streaming **host** | gaming PC (`niks-gaming`) | Serves the video. Receives the devices. |
| **USB/IP host** | mini PC | The machine the keyboard and mouse are physically plugged into. Serves them out. |
| **USB/IP client** | gaming PC | The machine that attaches the served devices. |

## Why the collision is real

- USB/IP names the role **after the device side**: the exporter's kernel module is `usbip-host.ko`; the importer
  is `vhci-hcd.ko` — a *virtual host controller*.
- `usbip-win2` titles itself **"USB/IP Client for Windows"**.
- Meanwhile in streaming, ArtLight is "the host" because it **serves the video**.

So on this pair of machines: **streaming-host = USB/IP-client** and **streaming-client = USB/IP-host** — exactly
inverted.

## Rule

Never write `host`, `client` or `server` bare — not in the UI, not in variable names, not in comments, not in
prose. Use:

- **ArtMoon** / **ArtLight** for the products and the streaming roles
- **exporter** for the machine the device is plugged into
- **importer** for the machine the device arrives on

## The verbs, and which side owns them

| Verb | Side | Meaning |
|---|---|---|
| **bind** | exporter (ArtMoon) | Make a device shareable. Persistent, privileged, local. |
| **attach** | importer (ArtLight) | Take a shared device. Moves it — the exporter loses it. |
| **detach** | importer (ArtLight) | Give it back. |
| **unbind** | exporter (ArtMoon) | Stop sharing it. |

## The agreed flow

1. **ArtMoon** enumerates its local devices (Win32 on Windows, sysfs on Linux — no network needed to draw the
   list) and shows them in **Input settings**.
2. The user **toggles** the devices to share. ArtMoon **binds** the toggled ones. This is a privileged local step
   that ships as a service inside ArtMoon's own installer — the same shape as the service `usbipd-win` installs.
   The user never sees a CLI.
3. **Stream starts.** ArtLight enumerates what ArtMoon is exporting and **attaches** the toggled ones.
4. **Stream stops.** ArtLight **detaches** — the devices return to ArtMoon's machine.
5. Un-toggling in ArtMoon **unbinds** (or simply stops exporting).

**Ordering rule:** stream up first, attach second. Attaching *moves* the device, and if the exporter is the machine
the user is sitting at, attaching before the stream is up takes their keyboard and mouse and they cannot start
anything.

## Requirements each product carries

- **ArtMoon (exporter):** local device enumeration, the toggle UI, and a privileged local service to bind. Needs
  only that service — nothing on any other machine.
- **ArtLight (importer):** a USB/IP client stack on the host machine — the UDE driver on Windows, `usbip` +
  `vhci-hcd` on Linux. This is ArtLight's own dependency, not ArtMoon's.

## Platform support

All four orders are supported upstream. Only Windows→Windows is proven on these machines so far.

| exporter | importer | upstream? | evidence |
|---|---|---|---|
| Windows | Windows | ✅ proven | `usbipd-win` + `usbip-win2`, attached 2026-10-03 |
| Windows | Linux | ✅ documented | usbipd-win README: `usbip attach --remote=<HOST> --busid=<BUSID>` |
| Windows | WSL 2 | ✅ documented | `usbipd attach --wsl --busid=…`, no admin required |
| Linux | Windows | ✅ documented | usbip-win2 README: kernels 4.19–7.0, server protocol 1.1.1 |
| Linux | Linux | ✅ | in-kernel both ends |
| macOS | anything | ❌ | no exporter, no client |

## Network

TCP **3240**. Reachable over any IP path, but the `usbipd-win` installer's firewall rule is
`RemoteAddress = LocalSubnet`, so it is **LAN-only by default**; a remote client needs the tunnel's subnet added
to that rule. Keep it inside a tunnel — USB/IP is plaintext with no authentication, so anyone who reaches 3240 and
knows a busid gets the device. Never port-forward it.

Attaching is **non-persistent**: re-attach after a reboot, a device reset, or a replug.

## Known trap

An empty `Persisted:` table in `usbipd list` does **not** clear a repeatable `Device busy (already exported)`.
That message can mean the **vendor software** on the exporter (Razer Synapse and friends) is holding the HID
interfaces open. The engine keeps them, so the exporter cannot release the device — and it reports the wrong
cause. Stop the vendor engine on the exporter and the same attach succeeds.
