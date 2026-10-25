## libssh for ESP-IDF

Minimal ESP-IDF component wrapping upstream libssh to run an SSH server on ESP32-class devices.

### What this is
- **SSH2 server** powered by upstream `libssh` (vendored sources).
- **ESP-IDF compatible** build and thin port shims under `port/`.
- **Examples** for a simple SSH server, an ESP-IDF console over SSH, and a small "bastion" that can create TCP tunnels from the device.

### Upstream and layout
- Upstream sources are vendored under `libssh-0.11.x/` and compiled via this component's `CMakeLists.txt`. Do not modify upstream sources unless intentionally vendoring changes.
- ESP-IDF compatibility shims live in `port/`.
- Reference projects live in `examples/`.

### Requirements
- ESP-IDF v5+ (tested with recent v5 and v6 releases).
- Enable networking (Wi‑Fi or Ethernet) in your project.
- Recommended: Ed25519 host keys for best performance and security.

### How to use

Add the libssh subdirectory to your component list, either using the IDF build system or via the component YAML file:

```bash
idf.py add-dependency david-cermak/libssh
```

Or pull it from the Component Registry:

```yaml
dependencies:
  david-cermak/libssh: "*"
```

### Quick start (examples)
Build any of the examples under `examples/` using standard ESP-IDF flow:

```bash
idf.py set-target esp32
cd libssh/examples/server
idf.py build flash monitor
```

Then connect from your host (default example port 2222 unless noted otherwise):

```bash
ssh user@<device-ip> -p 2222
```

### Generate SSH host keys (recommended: Ed25519)
Generate a unique host key for your device and place it in the example's `main/` folder as shown below.

```bash
ssh-keygen -t ed25519 -f ssh_host_ed25519_key -N ""
# place the private key at: examples/<name>/main/ssh_host_ed25519_key
```

Avoid using example/demo keys in real deployments. Never commit private keys to source control.

### Examples overview
- `examples/server` — Minimal SSH server with a couple of demo commands; supports password and/or public‑key auth.
- `examples/esp_ssh` — Wraps a simple ESP-IDF console over SSH (basic shell with demo commands).
- `examples/bastion` — SSH server with built-in console commands to create TCP tunnels from the device (acts as a small bastion). Default SSH port in this example is 22.
- `examples/serial` — Baseline serial console sample (for comparison with networked console).

Each example includes its own `README.md` with exact steps, menuconfig options, and connection details.

### Authentication
- Password auth: default `user/password` in examples (can be disabled).
- Public‑key auth: add your client public key to the example as documented in its `README.md`.

### Configuration notes
- Networking helpers: examples depend on `protocol_examples_common` to configure Wi‑Fi/Ethernet.
- mbedTLS threading: ensure pthread threading is enabled in menuconfig when required by your IDF version.
- On some IDF versions, enabling `LWIP_NETIF_API` may be required (see example `sdkconfig.defaults`).

### Building in your own app
Typical flow:
1. Add the dependency to your `idf_component.yml`.
2. Provide a host key (place securely, e.g. in NVS or embedded as a file in examples).
3. Initialize networking (Wi‑Fi/Ethernet).
4. Use libssh server APIs to bind, set options, load the host key, and handle sessions/channels. See `examples/esp_ssh` for a compact flow diagram and code references.

### Security tips
- Prefer Ed25519 host and client keys.
- Keep private keys out of source control; example keys are placeholders only.
- Review example `menuconfig` options to disable password auth in production and rely on public‑key auth.

### Useful links
- Top-level project README: `../README.md`
- Slides: `docs/presentation.md`
- Examples: `examples/server`, `examples/esp_ssh`, `examples/bastion`, `examples/serial`

If you run into issues, include your IDF version, target SoC/board, build steps, and SSH logs when reporting.
