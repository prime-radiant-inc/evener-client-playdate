// enroll prepares an isolated Tailscale identity for the native Playdate client.
package main

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/netip"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"golang.org/x/crypto/curve25519"

	"tailscale.com/client/local"
	"tailscale.com/derp/derphttp"
	"tailscale.com/hostinfo"
	"tailscale.com/ipn"
	"tailscale.com/ipn/ipnstate"
	"tailscale.com/ipn/store"
	"tailscale.com/net/netmon"
	"tailscale.com/tailcfg"
	"tailscale.com/tsnet"
	"tailscale.com/types/key"
)

type configuration struct {
	DiscoPrivateKey   string     `json:"disco_private_key"`
	DiscoPublicKey    string     `json:"disco_public_key"`
	PeerDiscoKey      string     `json:"peer_disco_key"`
	MachinePrivateKey string     `json:"machine_private_key"`
	ControlPublicKey  string     `json:"control_public_key"`
	EnrolledOS        string     `json:"enrolled_os"`
	Hostname          string     `json:"hostname"`
	NodePrivateKey    string     `json:"node_private_key"`
	NodePublicKey     string     `json:"node_public_key"`
	NodeIP            string     `json:"node_ip"`
	PeerPublicKey     string     `json:"peer_public_key"`
	PeerIP            string     `json:"peer_ip"`
	PeerPort          int        `json:"peer_port"`
	DERPHost          string     `json:"derp_host"`
	DERPPort          int        `json:"derp_port"`
	DERPPublicKey     string     `json:"derp_public_key"`
	Seed              string     `json:"seed"`
	NodeKeyExpiry     *time.Time `json:"node_key_expiry"`
	PeerKeyExpiry     *time.Time `json:"peer_key_expiry"`
	NodeID            string     `json:"node_id"`
	PeerID            string     `json:"peer_id"`
	NodeDERPRegion    string     `json:"node_derp_region"`
	PeerDERPRegion    string     `json:"peer_derp_region"`
	DERPRegionID      int        `json:"derp_region_id"`
	ExportedAt        time.Time  `json:"exported_at"`
}

func quiet(string, ...any) {}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "Enrollment failed:", err)
		os.Exit(1)
	}
}

func run() error {
	state := flag.String("state-dir", "../build/enrollment", "dedicated state directory for this Playdate identity")
	out := flag.String("out", "../Source/tailnet.json", "private native configuration output")
	peerName := flag.String("peer", "magic-kingdom", "exact peer hostname, DNS name, or Tailscale IP")
	hostname := flag.String("hostname", "playdate-derp-spike", "name of the separate enrolled node")
	controlOnly := flag.Bool("control-only", false, "export control credentials from isolated state without starting the enrolled node")
	adoptNative := flag.String("adopt-native-config", "", "adopt the current native private configuration before --refresh; stop the device first")
	refresh := flag.Bool("refresh", false, "reuse this tool's existing enrolled identity")
	timeout := flag.Duration("timeout", 15*time.Minute, "time allowed for login and export")
	flag.Parse()
	// Enrollment must use the explicit interactive login, never ambient auth.
	for _, name := range []string{"TS_AUTHKEY", "TS_CLIENT_SECRET", "TS_CLIENT_ID", "TS_ID_TOKEN", "TS_AUDIENCE", "TSNET_FORCE_LOGIN"} {
		if os.Getenv(name) != "" {
			return fmt.Errorf("unset %s before interactive enrollment", name)
		}
	}
	stateDir, err := filepath.Abs(*state)
	if err != nil {
		return err
	}
	if err := prepareState(stateDir, *refresh || *controlOnly); err != nil {
		return err
	}
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt)
	defer cancel()
	ctx, deadline := context.WithTimeout(ctx, *timeout)
	defer deadline()
	if *controlOnly {
		return exportControl(ctx, stateDir, *out, *hostname)
	}
	if *adoptNative != "" && !*refresh {
		return errors.New("--adopt-native-config requires --refresh")
	}
	if *refresh {
		_, err := os.Stat(filepath.Join(stateDir, "native-owner"))
		if err == nil && *adoptNative == "" {
			return errors.New("native client owns this identity; stop it and provide its current configuration with --adopt-native-config before --refresh")
		}
		if err != nil && !os.IsNotExist(err) {
			return err
		}
		if *adoptNative != "" {
			if err := adoptNativeConfig(stateDir, *adoptNative); err != nil {
				return err
			}
		}
	}
	server := &tsnet.Server{Dir: stateDir, Hostname: *hostname, Logf: quiet, UserLogf: quiet}
	client, err := server.LocalClient()
	if err != nil {
		return fmt.Errorf("start isolated tsnet: %w", err)
	}
	closed := false
	defer func() {
		if !closed {
			server.Close()
		}
	}()
	fmt.Println("Waiting for the separate Playdate node to authenticate.")
	status, err := waitOnline(ctx, client)
	if err != nil {
		return err
	}
	peer, err := findPeer(status, *peerName)
	if err != nil {
		return err
	}
	if status.Self == nil {
		return errors.New("authenticated status has no self node")
	}
	profile, err := server.Store.ReadState(ipn.CurrentProfileStateKey)
	if err != nil {
		return fmt.Errorf("read isolated profile pointer: %w", err)
	}
	encoded, err := server.Store.ReadState(ipn.StateKey(profile))
	if err != nil {
		return fmt.Errorf("read isolated profile: %w", err)
	}
	var prefs ipn.Prefs
	if json.Unmarshal(encoded, &prefs) != nil || prefs.Persist == nil || prefs.Persist.PrivateNodeKey.IsZero() {
		return errors.New("isolated profile does not contain a valid node private key")
	}
	machine, err := readMachine(server.Store)
	if err != nil {
		return err
	}
	private := prefs.Persist.PrivateNodeKey
	if private.Public() != status.Self.PublicKey {
		return errors.New("isolated stored key does not match the authenticated node")
	}
	nodeIP, err := ipv4(status.TailscaleIPs)
	if err != nil {
		return fmt.Errorf("Playdate: %w", err)
	}
	peerIP, err := ipv4(peer.TailscaleIPs)
	if err != nil {
		return fmt.Errorf("peer: %w", err)
	}
	derpMap, err := client.CurrentDERPMap(ctx)
	if err != nil {
		return err
	}
	region, node, err := relay(derpMap, peer.Relay)
	if err != nil {
		return err
	}
	who, err := client.WhoIs(ctx, peerIP)
	if err != nil || who.Node == nil || who.Node.Key != peer.PublicKey || who.Node.DiscoKey.IsZero() {
		return errors.New("selected peer has no authenticated discovery identity")
	}
	peerDisco := who.Node.DiscoKey.Raw32()
	// The native client takes exclusive ownership of this enrolled node key.
	// Close the userspace WireGuard/DERP connections before probing or exporting.
	if err := server.Close(); err != nil {
		return fmt.Errorf("stop enrollment node: %w", err)
	}
	closed = true
	fmt.Println("Enrollment node stopped. Reading the peer home DERP server key.")
	port := node.DERPPort
	if port == 0 {
		port = 443
	}
	monitor := netmon.NewStatic()
	defer monitor.Close()
	derp, err := derphttp.NewClient(private, "https://"+net.JoinHostPort(node.HostName, strconv.Itoa(port))+"/derp", quiet, monitor)
	if err != nil {
		return err
	}
	defer derp.Close()
	probeCtx, probeCancel := context.WithTimeout(ctx, 30*time.Second)
	defer probeCancel()
	if err := derp.Connect(probeCtx); err != nil {
		return fmt.Errorf("connect to DERP: %w", err)
	}
	serverKey := derp.ServerPublicKey()
	if serverKey.IsZero() {
		return errors.New("DERP handshake returned no server public key")
	}
	if err := derp.Close(); err != nil {
		return err
	}
	var seed [32]byte
	if _, err := rand.Read(seed[:]); err != nil {
		return err
	}
	var native configuration
	if *adoptNative != "" {
		encoded, err := os.ReadFile(*adoptNative)
		if err != nil || json.Unmarshal(encoded, &native) != nil {
			return errors.New("could not reload adopted native configuration")
		}
		existing, err := hex.DecodeString(native.Seed)
		if err != nil || len(existing) != len(seed) {
			return errors.New("adopted native seed is invalid")
		}
		copy(seed[:], existing)
	}
	discoPrivate, discoPublic, err := discoveryIdentity(native.DiscoPrivateKey)
	if err != nil {
		return err
	}
	controlKey, err := fetchControlKey(ctx)
	if err != nil {
		return err
	}
	config := configuration{
		DiscoPrivateKey: discoPrivate, DiscoPublicKey: discoPublic, PeerDiscoKey: hex.EncodeToString(peerDisco[:]),
		MachinePrivateKey: hex.EncodeToString(machine.UntypedBytes()), ControlPublicKey: controlKey.UntypedHexString(), Hostname: *hostname, EnrolledOS: status.Self.OS,
		NodePrivateKey: private.UntypedHexString(), NodePublicKey: publicHex(private.Public()), NodeIP: nodeIP,
		PeerPublicKey: publicHex(peer.PublicKey), PeerIP: peerIP, PeerPort: 9180,
		DERPHost: node.HostName, DERPPort: port, DERPPublicKey: publicHex(serverKey), Seed: hex.EncodeToString(seed[:]),
		NodeKeyExpiry: status.Self.KeyExpiry, PeerKeyExpiry: peer.KeyExpiry,
		NodeID: string(status.Self.ID), PeerID: string(peer.ID), NodeDERPRegion: status.Self.Relay,
		PeerDERPRegion: peer.Relay, DERPRegionID: int(region.RegionID), ExportedAt: time.Now().UTC(),
	}
	if err := writeConfig(*out, config); err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(stateDir, "native-owner"), []byte("Native key owner: fetch the current native configuration before starting tsnet.\n"), 0600); err != nil {
		return err
	}
	fmt.Printf("Private configuration saved to %s (0600). Enrollment and DERP connections are closed.\n", *out)
	fmt.Printf("Node %s; peer %s:%d; DERP %s.\n", nodeIP, peerIP, config.PeerPort, config.DERPHost)
	if status.Self.Relay != peer.Relay {
		fmt.Printf("Home regions differ: enrolled node %s, peer %s; native return routing must account for this.\n", status.Self.Relay, peer.Relay)
	}
	return nil
}

func prepareState(dir string, refresh bool) error {
	const marker = "playdate-derp-enrollment-v1\n"
	path := filepath.Join(dir, "playdate-enrollment")
	if refresh {
		data, err := os.ReadFile(path)
		if err != nil || string(data) != marker {
			return errors.New("--refresh requires this tool's dedicated enrollment directory")
		}
		return nil
	}
	if entries, err := os.ReadDir(dir); err == nil && len(entries) != 0 {
		return errors.New("state directory is not empty; use --refresh only for this Playdate enrollment")
	} else if err != nil && !os.IsNotExist(err) {
		return err
	}
	if err := os.MkdirAll(dir, 0700); err != nil {
		return err
	}
	if err := os.Chmod(dir, 0700); err != nil {
		return err
	}
	return os.WriteFile(path, []byte(marker), 0600)
}

func waitOnline(ctx context.Context, client *local.Client) (*ipnstate.Status, error) {
	lastURL, lastState := "", ""
	for {
		status, err := client.Status(ctx)
		if err != nil {
			return nil, err
		}
		if status.AuthURL != "" && status.AuthURL != lastURL {
			fmt.Println("Jesse, approve this separate Playdate node:", status.AuthURL)
			lastURL = status.AuthURL
		}
		if status.BackendState != lastState {
			fmt.Println("Enrollment state:", status.BackendState)
			lastState = status.BackendState
		}
		if status.BackendState == "Running" && len(status.TailscaleIPs) != 0 {
			return status, nil
		}
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-time.After(time.Second):
		}
	}
}

func findPeer(status *ipnstate.Status, name string) (*ipnstate.PeerStatus, error) {
	var found *ipnstate.PeerStatus
	for _, peer := range status.Peer {
		matches := strings.EqualFold(peer.HostName, name) || strings.EqualFold(strings.TrimSuffix(peer.DNSName, "."), strings.TrimSuffix(name, "."))
		for _, ip := range peer.TailscaleIPs {
			matches = matches || ip.String() == name
		}
		if matches {
			if found != nil {
				return nil, errors.New("peer name is ambiguous; use its Tailscale IP")
			}
			found = peer
		}
	}
	if found == nil || found.PublicKey.IsZero() {
		return nil, errors.New("peer is absent from this enrolled node's permitted network map")
	}
	return found, nil
}

func ipv4(addresses []netip.Addr) (string, error) {
	for _, address := range addresses {
		if address.Is4() {
			return address.String(), nil
		}
	}
	return "", errors.New("no assigned IPv4 address")
}

func relay(derpMap *tailcfg.DERPMap, code string) (*tailcfg.DERPRegion, *tailcfg.DERPNode, error) {
	for _, region := range derpMap.Regions {
		if region.RegionCode != code {
			continue
		}
		for _, node := range region.Nodes {
			if !node.STUNOnly && node.HostName != "" && node.CertName == "" && !node.InsecureForTests {
				return region, node, nil
			}
		}
	}
	return nil, nil, errors.New("peer home DERP region has no standard TLS relay")
}

func publicHex(public key.NodePublic) string {
	bytes := public.Raw32()
	return hex.EncodeToString(bytes[:])
}

func writeConfig(path string, config any) error {
	data, err := json.MarshalIndent(config, "", "  ")
	if err != nil {
		return errors.New("could not encode private configuration")
	}
	if err := os.MkdirAll(filepath.Dir(path), 0700); err != nil {
		return err
	}
	file, err := os.CreateTemp(filepath.Dir(path), ".config-*.tmp")
	if err != nil {
		return err
	}
	defer os.Remove(file.Name())
	if _, err = file.Write(append(data, '\n')); err != nil {
		file.Close()
		return err
	}
	if err = file.Sync(); err != nil {
		file.Close()
		return err
	}
	if err = file.Close(); err != nil {
		return err
	}
	return os.Rename(file.Name(), path)
}

// readMachine accepts only the isolated tool-owned store, never daemon state.
func readMachine(state ipn.StateStore) (key.MachinePrivate, error) {
	var machine key.MachinePrivate
	encoded, err := state.ReadState(ipn.MachineKeyStateKey)
	if err != nil || machine.UnmarshalText(encoded) != nil || machine.IsZero() {
		return machine, errors.New("isolated state does not contain a valid machine identity")
	}
	return machine, nil
}

func fetchControlKey(ctx context.Context) (key.MachinePublic, error) {
	var keys tailcfg.OverTLSPublicKeyResponse
	req, err := http.NewRequestWithContext(ctx, "GET", fmt.Sprintf("https://controlplane.tailscale.com/key?v=%d", tailcfg.CurrentCapabilityVersion), nil)
	if err != nil {
		return keys.PublicKey, err
	}
	client := &http.Client{Timeout: 30 * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error {
		return errors.New("control key redirects are not accepted")
	}}
	response, err := client.Do(req)
	if err != nil {
		return keys.PublicKey, errors.New("verified TLS control key request failed")
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return keys.PublicKey, errors.New("control key request rejected")
	}
	data, err := io.ReadAll(io.LimitReader(response.Body, 4097))
	if err != nil || len(data) > 4096 || json.Unmarshal(data, &keys) != nil || keys.PublicKey.IsZero() {
		return keys.PublicKey, errors.New("control key response is invalid")
	}
	return keys.PublicKey, nil
}

// exportControl extends an existing private configuration without opening any
// node, WireGuard or DERP connection. This is safe while the native owner is off.
func exportControl(ctx context.Context, stateDir, out, hostname string) error {
	path := filepath.Join(stateDir, "tailscaled.state")
	if _, err := os.Stat(path); err != nil {
		return errors.New("isolated enrollment state is missing")
	}
	state, err := store.NewFileStore(quiet, path)
	if err != nil {
		return errors.New("could not read isolated enrollment state")
	}
	machine, err := readMachine(state)
	if err != nil {
		return err
	}
	data, err := os.ReadFile(out)
	if err != nil {
		return errors.New("control-only export requires the existing private configuration")
	}
	var config configuration
	if json.Unmarshal(data, &config) != nil {
		return errors.New("existing private configuration is invalid")
	}
	// Verify configuration ownership before exporting any machine credential.
	profile, err := state.ReadState(ipn.CurrentProfileStateKey)
	if err != nil {
		return errors.New("isolated profile pointer is missing")
	}
	encoded, err := state.ReadState(ipn.StateKey(profile))
	var prefs ipn.Prefs
	if err != nil || json.Unmarshal(encoded, &prefs) != nil || prefs.Persist == nil || prefs.Persist.PrivateNodeKey.IsZero() || prefs.Persist.PrivateNodeKey.UntypedHexString() != config.NodePrivateKey || publicHex(prefs.Persist.PrivateNodeKey.Public()) != config.NodePublicKey {
		return errors.New("configuration no longer matches isolated node; reconcile native rotation before re-enrollment")
	}
	control, err := fetchControlKey(ctx)
	if err != nil {
		return err
	}
	// Preserve all existing and native-added fields, including a staged key.
	var fields map[string]json.RawMessage
	if json.Unmarshal(data, &fields) != nil {
		return errors.New("existing private configuration is invalid")
	}
	discoPrivate, discoPublic, err := discoveryIdentity(config.DiscoPrivateKey)
	if err != nil {
		return err
	}
	peerDisco := config.PeerDiscoKey
	if peerDisco == "" {
		peerDisco = strings.Repeat("0", 64)
	}
	enrolledOS := config.EnrolledOS
	if enrolledOS == "" {
		enrolledOS = hostinfo.New().OS
	}
	for name, value := range map[string]string{"disco_private_key": discoPrivate, "disco_public_key": discoPublic, "peer_disco_key": peerDisco, "machine_private_key": hex.EncodeToString(machine.UntypedBytes()), "control_public_key": control.UntypedHexString(), "hostname": hostname, "enrolled_os": enrolledOS} {
		fields[name], err = json.Marshal(value)
		if err != nil {
			return errors.New("could not encode control export")
		}
	}
	if err := writeConfig(out, fields); err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(stateDir, "native-owner"), []byte("Native key owner: fetch the current native configuration before starting tsnet.\n"), 0600); err != nil {
		return err
	}
	fmt.Printf("Private control identity saved to %s (0600). No enrolled node was started.\n", out)
	return nil
}

// adoptNativeConfig reconciles native key maintenance into the one-shot store
// before it becomes a node owner again. No network connection is opened here.
func adoptNativeConfig(stateDir, configPath string) error {
	data, err := os.ReadFile(configPath)
	if err != nil {
		return errors.New("could not read current native configuration")
	}
	var config configuration
	var fields map[string]json.RawMessage
	if json.Unmarshal(data, &config) != nil || json.Unmarshal(data, &fields) != nil {
		return errors.New("native configuration is invalid")
	}
	if _, staged := fields["staged_node_private_key"]; staged {
		return errors.New("resolve staged native renewal before returning identity ownership to tsnet")
	}
	state, err := store.NewFileStore(quiet, filepath.Join(stateDir, "tailscaled.state"))
	if err != nil {
		return errors.New("could not read isolated enrollment state")
	}
	machine, err := readMachine(state)
	if err != nil {
		return err
	}
	var nativeMachine key.MachinePrivate
	var nativeNode key.NodePrivate
	if nativeMachine.UnmarshalText([]byte("privkey:"+config.MachinePrivateKey)) != nil || !nativeMachine.Equal(machine) || nativeNode.UnmarshalText([]byte("privkey:"+config.NodePrivateKey)) != nil || nativeNode.IsZero() || publicHex(nativeNode.Public()) != config.NodePublicKey {
		return errors.New("native configuration does not belong to this isolated machine")
	}
	profile, err := state.ReadState(ipn.CurrentProfileStateKey)
	if err != nil {
		return errors.New("isolated profile pointer is missing")
	}
	encoded, err := state.ReadState(ipn.StateKey(profile))
	var prefs ipn.Prefs
	if err != nil || json.Unmarshal(encoded, &prefs) != nil || prefs.Persist == nil {
		return errors.New("isolated profile is invalid")
	}
	if !prefs.Persist.PrivateNodeKey.Equal(nativeNode) {
		prefs.Persist.OldPrivateNodeKey = prefs.Persist.PrivateNodeKey
		prefs.Persist.PrivateNodeKey = nativeNode
		encoded, err = json.Marshal(&prefs)
		if err != nil {
			return errors.New("could not encode reconciled isolated profile")
		}
		if err := state.WriteState(ipn.StateKey(profile), encoded); err != nil {
			return errors.New("could not save reconciled isolated profile")
		}
	}
	return nil
}

// discoveryIdentity retains the native discovery key across metadata export.
func discoveryIdentity(encoded string) (string, string, error) {
	var private [32]byte
	if encoded == "" {
		if _, err := rand.Read(private[:]); err != nil {
			return "", "", err
		}
		private[0] &= 248
		private[31] = (private[31] & 127) | 64
	} else {
		raw, err := hex.DecodeString(encoded)
		if err != nil || len(raw) != 32 {
			return "", "", errors.New("native discovery private key is invalid")
		}
		copy(private[:], raw)
		if private == [32]byte{} {
			return "", "", errors.New("native discovery private key is zero")
		}
	}
	public, err := curve25519.X25519(private[:], curve25519.Basepoint)
	if err != nil {
		return "", "", errors.New("native discovery public key derivation failed")
	}
	return hex.EncodeToString(private[:]), hex.EncodeToString(public), nil
}
