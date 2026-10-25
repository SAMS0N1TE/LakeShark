# Simple SSH server

## How to use this example

### Generate SSH Host Keys

This example uses a hardcoded SSH host key for demonstration purposes, the demo host public key is also added for reference (to the `main` folder, it's fingerprint is `256 SHA256:XHZN4rhQ8EU4QeWCfG2+jNS7ONoKCw5DUkpiyKFFRpY`).

In a real project, you should generate your own unique host key using the `ssh-keygen` command.

**Recommended: Ed25519 Keys (Best Security & Performance)**

Ed25519 is the most secure and performant key type currently available:

```bash
ssh-keygen -t ed25519 -f ssh_host_ed25519_key -N ""
```

Alternatively use RSA or ECDSA Keys

```bash
ssh-keygen -t rsa -b 4096 -f ssh_host_rsa_key -N ""
ssh-keygen -t ecdsa -b 521 -f ssh_host_ecdsa_key -N ""
```

Copy the key to `main/ssh_host_ed25519_key` and rebuild the project

The server will automatically use your key for all SSH connections on port 2222 (default).

### Configure and build

* Configure the connection (WiFi or Ethernet per your board options)
* Build and run the project normally with `idf.py build flash monitor`

### Connect to the server

```
ssh user@[IP-address] -p 2222
```
and use the default user/password to login

run some demo commands provided by this example
* `reset` -- restarts the ESP32
* `hello` -- says hello-world
* `exit` -- exit the shell


```mermaid
flowchart TD
    A["app_main()"] --> B["initialize_esp_components()"]
    B --> C["ssh_init()"]
    C --> D["ssh_bind_new()"]
    D --> E["ssh_bind_options_set(...)"]
    E --> F["set_hostkey()"]
    F --> G["ssh_bind_listen()"]
    G --> H{"Forever Loop: Accept connections"}

    H --> I["ssh_new()"]
    I --> J["ssh_bind_accept()"]
    J --> K["ssh_set_server_callbacks()"]
    K --> L["ssh_handle_key_exchange()"]

    L --> M{"Key Exchange OK?"}
    M -- Yes --> N["ssh_set_auth_methods()"]
    N --> O["handle_connection(session)"]
    
    O --> P["ssh_event_new()"]
    P --> Q["ssh_event_add_session()"]
    Q --> R{"Authenticated & Channel Created?"}
    R -- No --> R
    R -- Yes --> S["handle_shell_io(channel)"]

    S --> T{"Read from channel"}
    T --> U["echo back to channel"]
    U --> V["Check command"]
    V -- hello --> W["Respond: Hello, world!"]
    V -- reset --> X["Call esp_restart()"]
    V -- exit --> Y["Break loop"]
    Y --> Z["Cleanup session"]
    
    M -- No --> Z
    Z --> H

    style A fill:#dfefff,stroke:#333
    style S fill:#f0f9ff,stroke:#339
    style V fill:#fffbe6,stroke:#aa0
    style Z fill:#ffdfdf,stroke:#933
