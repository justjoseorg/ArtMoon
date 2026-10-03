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

**Proven in the wild, 2026-10-02:** Nik attached the mini PC's keyboard and mouse to the gaming PC and played
**Aion 2** — a game that drops injected input — and it registered his inputs normally once the devices were
passed over. That is the whole justification for this feature, demonstrated end to end before any code was
written.

| exporter | importer | upstream? | evidence |
|---|---|---|---|
| Windows | Windows | ✅ proven in use | `usbipd-win` + `usbip-win2`; Aion 2 accepted the real HID, 2026-10-02 |
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

## The privileged service — how the desired set reaches it

**Decided 2026-10-03.** `bind` is refused to a normal user (verified on the mini PC: a limited token gets
`LastTaskResult : 10`), so ArtMoon cannot bind for itself. Each platform carries a small privileged local
service that owns the bind step, and the app talks to it. **The user never sees a command line.**

### Linux

The helper is a static binary at `/usr/libexec/artmoon-input-service`. The AppImage carries a copy at *the same
relative path it occupies on the system* (`usr/libexec/…`), so placing it is a copy and not a remapping — and a
normally-installed ArtMoon resolves the bundled path to the real system path, which exists only once the helper
is genuinely there. That relationship is what makes the app's "should I offer to set this up" answer honest.

Setting it up is one administrator prompt: `pkexec install -m 0755 -o root -g root <staged> /usr/libexec/artmoon-input-service`.

`<staged>` is deliberately not the copy inside the AppImage. An AppImage runs from a FUSE mount, and the AppImage
runtime mounts it **without** `allow_other` or `allow_root` — verified on Niks-z13 on 2026-10-03, mount options
`ro,nosuid,nodev,relatime,user_id=1000,group_id=1000`. Only the mounting user may traverse such a mount, and root is
refused too, because `CAP_DAC_OVERRIDE` does not bypass the FUSE mount-owner check. Handing the in-mount path to an
elevated `install` therefore fails with `cannot stat: Permission denied`. So the app copies the helper into a
directory only that user can reach — 0700 under `$XDG_RUNTIME_DIR` — and the elevated step reads from there. No other
principal can redirect what root reads; the user alone can, and they are the one approving the prompt. Hardening that
last step means handing the payload to pkexec over stdin rather than as a path, which needs evidence that pkexec
carries stdin through — not established, so not what ships.

`install(1)` specifically, not a shell command and not a script carried in the bundle. Whatever is handed to
pkexec runs as root, so it must be a system binary that takes two paths and does one thing — an approver can see
exactly what is being authorised. Approving a script inside a user-writable mount would authorise *that script's
path*, and anything able to write there could then get a program of its choosing run as root. `install` also
applies mode and owner itself, so the helper never exists at its system path with the wrong ownership, not even
briefly.

### Windows

The installer creates the service (`ArtMoonInputService`) — the same shape as the service `usbipd-win` installs
for itself. The app never binds; the service does.

**How the desired set reaches the service — and the two options that are ruled out:**

- **Not `sc start … reconcile`.** A service that is already running refuses a start with error **1056**, so start
  arguments cannot carry a *changing* desired set. They can only ever deliver the first one.
- **Not a file.** Any path that a normal process can write, the service must not read as instruction. A file the
  user's own session can rewrite would let anything on the machine choose which devices are offered over the
  network — the same rule the Linux side is already held to. The desired set is *requested and validated*, never
  trusted as stored state.
- **A local named pipe, created by the service**, with an explicit DACL granting the interactive user and
  administrators, and `PIPE_REJECT_REMOTE_CLIENTS` set so that a remote machine cannot reach it even if the pipe
  name is known. The app sends the desired set on each change; the service validates every busid with the same
  guards as the Linux helper and refuses anything it does not recognise.

On both platforms the service keeps **no state of its own** that can disagree with the Input tab: it reconciles,
it does not remember. `usbipd list` (Windows) and `/sys/bus/usb/drivers/usbip-host` (Linux) stay the authority on
what is *actually* shared, which is why the tab can show "not shared yet" and mean it.
