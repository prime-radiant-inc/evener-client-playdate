# Playdate node enrollment

This one-shot Mac helper uses official `tsnet` and its local API to enroll a
separate Tailscale node for the native DERP feasibility probe. It neither reads
the Mac's Tailscale state nor contacts its local daemon. There is no persistent
relay or proxy process.

Run from this directory:

```sh
export GOWORK=off
export GOMODCACHE="$PWD/.cache/mod"
export GOCACHE="$PWD/.cache/build"
go build -o enroll .
./enroll
```

The helper prints the normal Tailscale sign-in URL for Jesse to approve. It
does not open a browser or approve enrollment. Existing tailnet policies may
require separate device approval; the helper waits without changing them.
Ambient auth-key, OAuth and forced-login environment variables are rejected so
an unrelated credential cannot silently select this node's account.

The default state directory is `../build/enrollment`, created with mode 0700.
The first run requires an empty directory and places its ownership marker
there. To continue an interrupted login or refresh metadata for the same node:

```sh
# An interrupted first login, before exporting to a native owner:
./enroll --refresh
# After native ownership, stop the device and retrieve its current private config:
./enroll --refresh --adopt-native-config /private/path/to/current-tailnet.json
```

Do not run enrollment or refresh while the Playdate is using this node identity.
`--refresh` reuses the isolated state; it does not request a new identity.
Export creates a `native-owner` marker. Subsequent refresh refuses to start tsnet
unless `--adopt-native-config` explicitly supplies the native owner's current
configuration. The helper verifies the same machine identity and the node key
pair, then reconciles that current node key into the isolated profile. Retrieve
the current device state after any native rotation; an old bundled configuration
is insufficient. Resolve a staged renewal natively before returning ownership to
tsnet. The marker prevents a plain refresh from silently restarting an old key.
Tailscale may rotate an expired node key during normal authentication, so
refresh the native configuration together with the exported metadata.

After authentication, the helper finds `magic-kingdom` in the new node's own
permitted network map, reads that peer's IPv4/public key/home DERP region, and
checks that the isolated stored private key matches the authenticated self node.
It closes tsnet before making a brief official DERP client connection to obtain
the relay's public key over verified TLS. That connection also closes before
the private configuration is exported to `../Source/config.json` with mode 0600.
The parent probe's ignore rules must exclude `build/` and `Source/config.json`.

The native fields are:

- `machine_private_key`, `control_public_key`, `node_private_key`,
  `node_public_key`, `peer_public_key`, `derp_public_key`, `disco_private_key`,
  `disco_public_key`, `peer_disco_key`:
  raw 32-byte keys encoded as 64 hexadecimal characters.
- The discovery private/public pair is a stable native identity, preserved
  together with the seed when adopting current native state. The selected peer's
  discovery key comes from its authenticated node record. Native map requests
  advertise the local discovery public key; DERP discovery pings and pongs use
  the pair independently of the WireGuard node key.
- `node_ip`, `peer_ip`: assigned Tailscale IPv4 addresses.
- `peer_port`: 9180.
- `derp_host`, `derp_port`: a TLS relay in the peer's home region.
- `seed`: 32 random bytes encoded as hexadecimal.
- `hostname`, `enrolled_os`: the original enrollment fingerprint. Preserve its
  OS in authenticated requests: changing it when transferring this isolated
  identity triggers the service's copied-state restriction. `enrolled_os` does
  not describe the handheld hardware; the client is native Playdate C.
- `node_key_expiry`, `peer_key_expiry`, node/peer stable IDs, home region codes,
  selected DERP region ID, and export timestamp: metadata for the probe.

DERP regions do not forward between regions. The helper records both home
regions and reports a mismatch, because native return routing must account for
the enrolled node's advertised home region. It does not rewrite tailnet policy
or claim that DERP authentication alone proves a WireGuard or application
connection. The native probe must establish that evidence.

Official references: [tsnet server API](https://tailscale.com/docs/reference/tsnet-server-api)
and [pinned implementation](https://github.com/tailscale/tailscale/tree/v1.104.1).


To extend the existing isolated configuration with control credentials without
starting tsnet, WireGuard, or DERP:

```sh
./enroll --control-only --state-dir ../build/enrollment --out ../Source/config.json
```

This reads only the tool-owned isolated machine key and existing node profile.
It verifies that the profile matches the exported node key and obtains the
Noise control public key from `https://controlplane.tailscale.com/key?v=148` over
verified TLS. Redirects are refused. Unknown native configuration fields,
including a staged renewal key, are preserved. For this one-shot Mac enrollment,
the offline export preserves the official Mac host fingerprint and native seed.
It creates the stable native discovery pair only if none exists. A configuration
exported before discovery support gets a zero peer discovery placeholder; obtain
a successful authenticated native map before activating its relay transport.
That map supplies the selected peer's discovery key and verifies that control
retained the local discovery advertisement. It prints no
private key. No existing system Tailscale daemon or state is accessed.

The native control engine fetches that public key over verified TLS at runtime,
then uses Noise IK and HTTP/2 for authenticated register/map maintenance.
Complete uncompressed snapshots refresh the enrolled node, stable selected peer,
expiry and relay metadata. Advertised `PreferredDERP` follows the selected peer's
home region; a changed region is reconciled by another authenticated map request.
A changed relay hostname or port requires obtaining a new DERP server key over
verified TLS before using the relay.

Seven days before expiry, a replacement node key is durably staged before any
registration with `OldNodeKey` is sent. An interrupted attempt reuses that same
candidate. The service may require new human authorization, and the native
engine reports that explicitly without activating the candidate. Its
`control_start_current` path keeps the existing authorized key's maps fresh and
preserves the pending candidate. A completed authenticated map supplies the
replacement identity; the platform persists it before activating the transport.

`tools/playdate-client/src/control_host.c` is an opt-in OpenSSL interop runner,
excluded from the handheld build. Link it with `json.c`, the transport sources
in `transport.mk`, and the HPACK/control sources and flags in `control.mk`.
Run it only while every other owner of this isolated node is stopped:

```sh
control-host /private/tailnet.json /private/result.binary /private/staged.binary
```

The runner writes binary snapshots with mode 0600. `PLAYDATE_CONTROL_CURRENT_ONLY=1`
refreshes the existing key even if the stage file contains a candidate;
`PLAYDATE_CONTROL_RESULT_JSON=/private/current.json` atomically exports the
resulting private configuration. The opt-in qualification flags
`PLAYDATE_CONTROL_FORCE_RENEW=1` and `PLAYDATE_CONTROL_STOP_AFTER_REGISTER=1`
exercise renewal and interruption; they can create an authorization requirement.
`PLAYDATE_CONTROL_AUTH_URL_FILE=/private/reauth.url` saves an authenticated sign-in
URL privately without printing it. A build with `CONTROL_DIAGNOSTICS` can capture
map JSON to `PLAYDATE_CONTROL_MAP_FILE`; it contains tailnet metadata and must
remain private. None of these diagnostic paths participates in normal operation.
