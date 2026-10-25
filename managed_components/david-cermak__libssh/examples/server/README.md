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

## Authentication methods

This example supports password authentication, public-key authentication, or both. You can control this in this example menuconfig.

- `ALLOW_PASSWORD_AUTH`: set to 1 to enable password authentication
- `ALLOW_PUBLICKEY_AUTH`: set to 1 to enable public-key authentication

If both are enabled, clients may authenticate using either method.

### Password authentication

- Default username: `user`
- Default password: `password` (only if `ALLOW_PASSWORD_AUTH` is 1)

### Public-key authentication

1) Enable: set `#define ALLOW_PUBLICKEY_AUTH 1` in `libssh/examples/server/main/server.c`.

2) Generate a client keypair (recommended: Ed25519) on your development machine:

```bash
ssh-keygen -t ed25519 -f client_ssh_key -N "" -C "user@client"
```

3) Add the public key to the allowed keys string in the server:

- Open `libssh/examples/server/main/ssh_allowed_client_key.pub`
- Append the exact content of `client_ssh_key.pub` as a new line in the string, ending with `\n`.

You can add multiple keys by placing each key on its own line in the same string, separated by `\n`.

4) Rebuild and flash: `idf.py build flash monitor`

Note: For demonstration purpose, we provide a pre-generated temporary client key, which is added to the allowed client list.
To test is with this default key, just keep the default settings and run:

```bash
ssh -i main/client_ssh_key user@192.168.0.34 -p 2222
```

Warning: Do not use this key in any real-life project.
Warning: Edit the "ssh_allowed_client_key.pub" before using in a real project -- Do not forget to remove the temporary client key!

### Configure and build

* Configure the connection (WiFi or Ethernet per your board options)
* Build and run the project normally with `idf.py build flash monitor`

### Connect to the server

If password authentication is enabled:

```bash
ssh user@[IP-address] -p 2222
```

If public-key authentication is enabled and you added your key:

```bash
ssh -i ./client_ssh_key -o IdentitiesOnly=yes user@[IP-address] -p 2222
```

run some demo commands provided by this example
* `reset` -- restarts the ESP32
* `hello` -- says hello-world
* `exit` -- exit the shell
