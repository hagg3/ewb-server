package profile

import (
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func sampleSet() Set {
	return Set{
		Active: "dev",
		Profiles: map[string]Profile{
			"dev": {
				Name:       "dev",
				Kind:       Local,
				WorldDir:   "/abs/worlds/dev",
				WorldRoot:  "/abs/worlds",
				ServerBin:  "/abs/edenserver",
				EdenImport: "/abs/eden_import",
				Args:       []string{"--port", "27015", "--name", "Dev World"},
			},
			"prod": {
				Name:     "prod",
				Kind:     VPS,
				SSHHost:  "eden-vps",
				Service:  "edenserver",
				RunAs:    "edenserver",
				WorldDir: "/var/lib/edenserver/world",
				EnvFile:  "/etc/edenserver.conf",
			},
		},
	}
}

func TestTOMLRoundTrip(t *testing.T) {
	in := sampleSet()
	data := encodeSet(in)
	out, err := decodeSet(data)
	if err != nil {
		t.Fatalf("decode: %v\n---\n%s", err, data)
	}
	if out.Active != in.Active {
		t.Errorf("active: got %q want %q", out.Active, in.Active)
	}
	for name, want := range in.Profiles {
		got, ok := out.Profiles[name]
		if !ok {
			t.Fatalf("profile %q missing after round trip", name)
		}
		want.Name = name
		if !reflect.DeepEqual(got, want) {
			t.Errorf("profile %q:\n got %+v\nwant %+v", name, got, want)
		}
	}
}

func TestTOMLRejectsUnknownKey(t *testing.T) {
	_, err := decodeSet([]byte("[profiles.x]\nkind = \"local\"\nbogus_key = \"y\"\n"))
	if err == nil || !strings.Contains(err.Error(), "unknown key") {
		t.Fatalf("want unknown-key error, got %v", err)
	}
}

func TestTOMLRejectsStrayTopLevelKey(t *testing.T) {
	_, err := decodeSet([]byte("foo = \"bar\"\n"))
	if err == nil {
		t.Fatal("want an error for a stray top-level key")
	}
}

func TestTOMLTypeMismatch(t *testing.T) {
	_, err := decodeSet([]byte("[profiles.x]\nkind = \"local\"\nargs = \"not-an-array\"\n"))
	if err == nil || !strings.Contains(err.Error(), "expected an array") {
		t.Fatalf("want array/string mismatch, got %v", err)
	}
}

func TestTOMLComment(t *testing.T) {
	s, err := decodeSet([]byte("active = \"a\"  # trailing comment\n# whole line\n[profiles.a]\nkind = \"local\"\nworld_dir = \"/x\"\n"))
	if err != nil {
		t.Fatal(err)
	}
	if s.Active != "a" || s.Profiles["a"].WorldDir != "/x" {
		t.Fatalf("comment handling wrong: %+v", s)
	}
}

func TestTOMLStringWithSpecials(t *testing.T) {
	orig := `a "quote" and\back and	tab`
	s := Set{Active: "x", Profiles: map[string]Profile{
		"x": {Name: "x", Kind: Local, WorldDir: "/abs", ServerBin: orig},
	}}
	// ServerBin must be absolute to validate; use it only as a value carrier here.
	s.Profiles["x"] = Profile{Name: "x", Kind: Local, WorldDir: "/abs", Args: []string{orig, "second, arg"}}
	out, err := decodeSet(encodeSet(s))
	if err != nil {
		t.Fatalf("%v\n%s", err, encodeSet(s))
	}
	if !reflect.DeepEqual(out.Profiles["x"].Args, []string{orig, "second, arg"}) {
		t.Fatalf("special chars lost: %q", out.Profiles["x"].Args)
	}
}

func TestValidate(t *testing.T) {
	bad := []Profile{
		{Name: "", Kind: Local, WorldDir: "/x"},
		{Name: "has space", Kind: Local, WorldDir: "/x"},
		{Name: "x", Kind: "weird", WorldDir: "/x"},
		{Name: "x", Kind: Local, WorldDir: "relative/path"},
		{Name: "x", Kind: Local}, // no world_dir
		{Name: "x", Kind: VPS, WorldDir: "/x", RunAs: "e"},   // no ssh_host
		{Name: "x", Kind: VPS, SSHHost: "h", WorldDir: "/x"}, // no run_as
		{Name: "x", Kind: Local, WorldDir: "/x", Socket: "/" + strings.Repeat("a", 110)},
	}
	for i, p := range bad {
		if err := p.Validate(); err == nil {
			t.Errorf("case %d: expected a validation error for %+v", i, p)
		}
	}
	good := []Profile{
		{Name: "dev", Kind: Local, WorldDir: "/abs/w"},
		{Name: "prod", Kind: VPS, SSHHost: "eden-vps", RunAs: "edenserver", WorldDir: "/var/lib/edenserver/world"},
	}
	for i, p := range good {
		if err := p.Validate(); err != nil {
			t.Errorf("good case %d rejected: %v", i, err)
		}
	}
}

func TestDerivedPaths(t *testing.T) {
	p := Profile{Kind: Local, WorldDir: "/w/dev", ServerBin: "/opt/ewb/edenserver"}
	if p.SocketPath() != "/w/dev/edenserver.sock" {
		t.Errorf("SocketPath = %q", p.SocketPath())
	}
	if p.EdenctlPath() != "/opt/ewb/edenctl" {
		t.Errorf("EdenctlPath = %q", p.EdenctlPath())
	}
	if p.EdenImportPath() != "/opt/ewb/eden_import" {
		t.Errorf("EdenImportPath = %q", p.EdenImportPath())
	}
	p.Socket = "/tmp/custom.sock"
	if p.SocketPath() != "/tmp/custom.sock" {
		t.Errorf("explicit socket ignored: %q", p.SocketPath())
	}
}

func TestSaveLoadRoundTripAndMode(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "sub", "profiles.toml")
	in := sampleSet()
	if err := Save(path, in); err != nil {
		t.Fatalf("Save: %v", err)
	}
	fi, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	if fi.Mode().Perm() != 0o600 {
		t.Errorf("mode = %o, want 600", fi.Mode().Perm())
	}
	out, err := Load(path)
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	if out.Active != in.Active || len(out.Profiles) != len(in.Profiles) {
		t.Errorf("round trip mismatch: %+v", out)
	}
}

func TestLoadMissingFileIsEmptySet(t *testing.T) {
	s, err := Load(filepath.Join(t.TempDir(), "nope.toml"))
	if err != nil {
		t.Fatalf("missing file should not be an error: %v", err)
	}
	if len(s.Profiles) != 0 {
		t.Errorf("expected empty set, got %+v", s)
	}
}

func TestLoadRejectsInvalidProfile(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "profiles.toml")
	os.WriteFile(path, []byte("[profiles.x]\nkind = \"local\"\nworld_dir = \"relative\"\n"), 0o600)
	if _, err := Load(path); err == nil {
		t.Fatal("Load should reject a profile that fails validation")
	}
}
