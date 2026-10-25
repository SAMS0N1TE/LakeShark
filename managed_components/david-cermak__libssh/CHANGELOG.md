See the upstream [CHANGELOG](libssh-mirror/CHANGELOG)

0.12.2
- Based on upstream tag libssh-0.12.2
- Manually patches upstream to include mbedtls-v4 support (ac4b723c)

0.12.0~3
- Based on upstream master branch (HEAD)
- Includes mbedtls-v4 support
- Includes security fixes from upstream:
  - Upstream 0.12.0:
    - CVE-2025-14821: Fix global config location on Windows (6a7f19ec)
    - CVE-2026-0964: SCP Protocol Path Traversal (daa80818)
    - CVE-2026-0965: DoS parsing config files (a5eb30db)
    - CVE-2026-0966: Buffer underflow in ssh_get_hexa() (417a095e)
    - CVE-2026-0967: ReDoS via crafted patterns (a411de5c)
    - CVE-2026-0968: OOB Read in sftp_parse_longname() (20856f44)
    - libssh-2026-sftp-extensions: OOB read in SFTP extensions (855a0853)
  - Upstream 0.12.1:
    - CVE-2026-15370: Stack buffer overrun in sftpserver (347c69d2)
    - CVE-2026-59842: Missing length checks in kex-gss Curve25519 (ed9109df)
    - CVE-2026-59844: Unbounded len in SSH_FXP_READ (6dba2e06)
    - CVE-2026-59845: Missing fork() return check in socket (92b6fb9c)
    - CVE-2026-59846: Shell metacharacters in usernames (6309df22)
    - CVE-2026-59847: AES-GCM tag verification issues (a5173c6a / 6d6cb6cb)
    - CVE-2026-59848: sftp unknown request ID handling (00876f76 / 26147eb4)
    - CVE-2026-59849: auth infinite loop (bcfa1912 / bb964323)
    - CVE-2026-59850: DATA on closed channels (a8a3fa35)
    - CVE-2026-59851: gssapi-keyex missing callback dispatch (a45d20b7)
  - Upstream 0.12.2:
    - CVE-2026-59843: Zero max packet size in channel open (3f785905)
- See upstream [CHANGELOG](libssh-mirror/CHANGELOG)

0.12.0~2
- See upstream changes 0.12.0
- ESP-IDF: Add SBOM-yml
- Port: Fix version header to match component version (0.12.0)

0.12.0~1
- See upstream changes 0.12.0
- Port: Using native mbedtls-v4 support (instead of mbedtls-v3-shim component)
- Port: Switched to submodule upstream (instead of downloading release packages)

0.12.0
- See upstream changes 0.12.0 (Support PQC KEX, security fixes)
- Port: Make PQC KEX configurable (Default OFF)

0.11.0
- See upstream changes 0.11.0
- ESP-IDF: Simple ESP32 SSH server
