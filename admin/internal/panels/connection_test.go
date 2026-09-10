package panels

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/hagg3/ewb-server/admin/internal/profile"
	"github.com/hagg3/ewb-server/admin/internal/transport"
)

// fakeTransport scripts replies by a substring match on the joined argv and
// records every command it was asked to run (for assertions like "stop never
// shelled out to systemctl").
type fakeTransport struct {
	replies []fakeReply
	calls   []string
}
type fakeReply struct {
	match  string
	stdout string
	stderr string
	exit   int
	err    error
}

func (f *fakeTransport) Kind() string                     { return "vps" }
func (f *fakeTransport) Describe(c transport.Command) string { return strings.Join(c.Argv, " ") }
func (f *fakeTransport) Run(_ context.Context, c transport.Command) (transport.Result, error) {
	joined := strings.Join(c.Argv, " ")
	f.calls = append(f.calls, joined)
	for _, r := range f.replies {
		if strings.Contains(joined, r.match) {
			return transport.Result{
				Display:  joined,
				Stdout:   []byte(r.stdout),
				Stderr:   []byte(r.stderr),
				ExitCode: r.exit,
			}, r.err
		}
	}
	return transport.Result{Display: joined, ExitCode: 127}, nil
}

func fakeFactory(f *fakeTransport) transportFactory {
	return func(profile.Profile) (transport.Transport, error) { return f, nil }
}

func mkExec(t *testing.T, path string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte("#!/bin/sh\n"), 0o755); err != nil {
		t.Fatal(err)
	}
}

func probeByName(r ConnResult, name string) (Probe, bool) {
	for _, p := range r.Probes {
		if p.Name == name {
			return p, true
		}
	}
	return Probe{}, false
}

func TestConnectionLocalHappyPath(t *testing.T) {
	dir := t.TempDir()
	world := filepath.Join(dir, "worlds", "dev")
	os.MkdirAll(world, 0o755)
	mkExec(t, filepath.Join(dir, "edenserver"))
	mkExec(t, filepath.Join(dir, "edenctl"))
	mkExec(t, filepath.Join(dir, "eden_import"))

	p := profile.Profile{
		Name: "dev", Kind: profile.Local,
		WorldDir: world, WorldRoot: filepath.Join(dir, "worlds"),
		ServerBin: filepath.Join(dir, "edenserver"),
		Socket:    "/tmp/edenadmin-conntest.sock", // short: a t.TempDir() path blows sun_path
	}
	r := TestConnection(context.Background(), p)
	if !r.OK {
		t.Fatalf("expected OK; probes: %+v", r.Probes)
	}
	// The control socket probe is present but soft (server not running).
	sp, ok := probeByName(r, "control socket")
	if !ok || sp.OK || !sp.Soft {
		t.Fatalf("control socket probe wrong: %+v (found=%v)", sp, ok)
	}
}

func TestConnectionLocalMissingEdenImportFails(t *testing.T) {
	dir := t.TempDir()
	world := filepath.Join(dir, "w")
	os.MkdirAll(world, 0o755)
	// No eden_import, no edenctl.
	p := profile.Profile{
		Name: "dev", Kind: profile.Local,
		WorldDir: world,
		Socket:   "/tmp/edenadmin-conntest.sock",
		// ServerBin unset so EdenImportPath() falls back to a bare name that
		// won't stat as a file.
	}
	r := TestConnection(context.Background(), p)
	if r.OK {
		t.Fatal("expected failure with eden_import missing")
	}
	ip, ok := probeByName(r, "eden_import")
	if !ok || ip.OK || ip.Soft {
		t.Fatalf("eden_import probe should be a hard failure: %+v", ip)
	}
}

func TestConnectionLocalBadWorldDir(t *testing.T) {
	p := profile.Profile{Name: "dev", Kind: profile.Local, WorldDir: "/no/such/dir/anywhere"}
	r := TestConnection(context.Background(), p)
	if r.OK {
		t.Fatal("expected failure for a missing world dir")
	}
}

func TestConnectionInvalidProfile(t *testing.T) {
	p := profile.Profile{Name: "dev", Kind: profile.Local, WorldDir: "relative"}
	r := TestConnection(context.Background(), p)
	if r.OK || len(r.Probes) != 1 || r.Probes[0].Name != "profile is valid" {
		t.Fatalf("bad profile handling: %+v", r)
	}
}

var vpsProf = profile.Profile{
	Name: "prod", Kind: profile.VPS,
	SSHHost: "eden-vps", RunAs: "edenserver", Service: "edenserver",
	WorldDir: "/var/lib/edenserver/world", EdenImport: "/usr/local/bin/eden_import",
}

func TestConnectionVPSUnreachableStopsEarly(t *testing.T) {
	f := &fakeTransport{replies: []fakeReply{
		{match: "true", stderr: "ssh: Could not resolve hostname eden-vps", exit: 255},
	}}
	r := testConnection(context.Background(), vpsProf, fakeFactory(f))
	if r.OK {
		t.Fatal("unreachable host should not be OK")
	}
	if len(r.Probes) != 1 || r.Probes[0].Name != "ssh reachable" {
		t.Fatalf("expected to stop after the reachability probe, got %d: %+v", len(r.Probes), r.Probes)
	}
}

func TestConnectionVPSAllGreen(t *testing.T) {
	f := &fakeTransport{replies: []fakeReply{
		{match: "true", stdout: ""},
		{match: "test -S", exit: 0},
		{match: "systemctl show edenserver -p LoadState", stdout: "loaded\n"},
		{match: "journalctl", stdout: "Sep 09 12:00:00 host edenserver[1]: [Server] up\n"},
		{match: "eden_import -h", stdout: "eden_import — convert a .eden world\n"},
	}}
	r := testConnection(context.Background(), vpsProf, fakeFactory(f))
	if !r.OK {
		t.Fatalf("expected all green; probes: %+v", r.Probes)
	}
	if len(r.Probes) != 5 {
		t.Fatalf("want 5 probes, got %d", len(r.Probes))
	}
}

func TestConnectionVPSDetectsMissingSudoers(t *testing.T) {
	f := &fakeTransport{replies: []fakeReply{
		{match: "true", stdout: ""},
		{match: "test -S", stderr: "sudo: a password is required", exit: 1},
		{match: "systemctl show edenserver -p LoadState", stdout: "loaded\n"},
		{match: "journalctl", stdout: "x\n"},
		{match: "eden_import -h", stdout: "usage\n"},
	}}
	r := testConnection(context.Background(), vpsProf, fakeFactory(f))
	sp, ok := probeByName(r, "control socket (as edenserver)")
	if !ok {
		t.Fatal("no control-socket probe")
	}
	if sp.OK || sp.Soft || !strings.Contains(sp.Detail, "sudoers rule") {
		t.Fatalf("missing-sudoers not surfaced as a hard failure: %+v", sp)
	}
	if r.OK {
		t.Fatal("overall result should be a failure when sudoers is missing")
	}
}

func TestConnectionVPSDetectsSilentJournal(t *testing.T) {
	f := &fakeTransport{replies: []fakeReply{
		{match: "true", stdout: ""},
		{match: "test -S", exit: 0},
		{match: "systemctl show edenserver -p LoadState", stdout: "loaded\n"},
		{match: "journalctl", stdout: "   \n"}, // exit 0 but empty
		{match: "eden_import -h", stdout: "usage\n"},
	}}
	r := testConnection(context.Background(), vpsProf, fakeFactory(f))
	jp, _ := probeByName(r, "journal readable")
	if jp.OK || !jp.Soft || !strings.Contains(jp.Detail, "systemd-journal") {
		t.Fatalf("silent-journal case not surfaced: %+v", jp)
	}
	// Soft failure => overall still OK.
	if !r.OK {
		t.Fatalf("a soft journal failure should not sink the result: %+v", r.Probes)
	}
}
