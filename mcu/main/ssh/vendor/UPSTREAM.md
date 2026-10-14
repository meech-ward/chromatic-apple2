# Embedded SSH provenance

The embedded SSH transport adapts the protocol and cryptography subset of
[staticnet](https://github.com/azonenberg/staticnet), pinned to commit
`0b3e18c6077a9c3531a960316b67a549ec6fa541`. Its BSD-3-Clause license is retained
in [staticnet/LICENSE](staticnet/LICENSE); the adapted transport also carries
that notice. Files retain their upstream copyright headers.

Included sources:

- `staticnet/crypt/CryptoEngine.{cpp,h}`: BSD-3-Clause key derivation and
  Ed25519/X25519 abstraction.
- `staticnet/contrib/tweetnacl_25519.{cpp,h}`: the upstream reduced TweetNaCl
  implementation. [TweetNaCl is public domain](https://tweetnacl.cr.yp.to/).
- `staticnet/contrib/base64.{cpp,h}`: libb64, public domain as stated in its headers.

Local adaptations remove staticnet network-header dependencies, encode SSH
`mpint` values canonically, replace two undefined signed shifts in TweetNaCl
with equivalent multiplication, and align `crypto_sign_open` declarations with
its `uint64_t` definition for Linux LP64 compatibility.

The Chromatic adapter uses ESP-IDF BSD sockets and its mbedTLS implementation
for AES-GCM and SHA-256. Randomness comes from `esp_fill_random` after Wi-Fi is
active. A random Ed25519 host seed is committed to device NVS before listening;
provisioned binaries contain the user's private Wi-Fi/API/SSH settings and must
not be published.

The server provides one password-authenticated SSH-2 terminal for user `apple`:
Curve25519/SHA-256 key exchange, Ed25519 host identity, AES-128-GCM, PTY/shell
channels, terminal resizing, bounded buffers and explicit window accounting.
It presents the Apple II screen and keyboard, not a Unix shell. Exec, SFTP,
forwarding, compression and public-key user authentication are unsupported.
Client rekey requests disconnect with a reconnect message. Sessions are capped
at 12 hours or 256 MiB; authentication allows three failures within a 60-second
handshake. Keepalives recover abandoned connections without imposing a user
inactivity timeout. The transport uses a separate 2 KiB authenticated-decryption
scratch buffer and a 12 KiB task stack.

[Host tests](../tests/test_host.py) exercise the production transport, ESP socket
adapter and terminal using test-only credentials, ordinary OpenSSH, malformed
packets, tampering, reconnects and sanitizers. These tests are distinct from the
physical-device checks described in the main project documentation.
