package panels

import (
	"archive/tar"
	"bytes"
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/hagg3/ewb-server/admin/internal/eden"
	"github.com/hagg3/ewb-server/admin/internal/profile"
)

func TestValidWorldName(t *testing.T) {
	good := []string{"castle", "XEN7", "my.world", "a_b-c", "world1"}
	bad := []string{"", ".", "..", "a/b", "../x", ".hidden", "with space", strings.Repeat("x", 65)}
	for _, s := range good {
		if !ValidWorldName(s) {
			t.Errorf("ValidWorldName(%q) = false, want true", s)
		}
	}
	for _, s := range bad {
		if ValidWorldName(s) {
			t.Errorf("ValidWorldName(%q) = true, want false", s)
		}
	}
}

func TestWithActiveWorld(t *testing.T) {
	// Fresh args: --world (and --signs, since the sidecar exists) are appended.
	got := WithActiveWorld([]string{"--port", "27015"}, "/w/castle", true)
	want := "--port 27015 --world /w/castle/eden_world.model --signs /w/castle/eden_signs.txt"
	if strings.Join(got, " ") != want {
		t.Errorf("append:\n  %v\nwant %q", got, want)
	}

	// Existing --world / --signs are replaced in place; no sidecar -> --signs dropped.
	got = WithActiveWorld([]string{"--world", "/old/eden_world.model", "--signs", "/old/eden_signs.txt", "--name", "X"}, "/w/new", false)
	want = "--world /w/new/eden_world.model --name X"
	if strings.Join(got, " ") != want {
		t.Errorf("replace/drop:\n  %v\nwant %q", got, want)
	}
}

// makeWorldDir writes a minimal world directory and returns its path.
func makeWorldDir(t *testing.T, root, name string, withSigns bool) string {
	t.Helper()
	dir := filepath.Join(root, name)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	write := func(f, s string) {
		if err := os.WriteFile(filepath.Join(dir, f), []byte(s), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	write("eden_world.model", "65536:33:65536:1:0\n65537:33:65536:1:0\n")
	write(".gitignore", "*\n!.gitignore\n")
	write("eden_spawn.txt", "65536:33.92:65536\n")
	if withSigns {
		write("eden_signs.txt", "65536:33:65536:0:0:0:hi\n")
	}
	return dir
}

func TestWorldsLocalListsAndFlagsActive(t *testing.T) {
	root := t.TempDir()
	makeWorldDir(t, root, "alpha", true)
	makeWorldDir(t, root, "beta", false)
	// A junk directory with no model must be skipped.
	os.MkdirAll(filepath.Join(root, "empty"), 0o755)

	p := profile.Profile{
		Name: "dev", Kind: profile.Local,
		WorldRoot: root, WorldDir: filepath.Join(root, "beta"),
		Args: []string{"--world", filepath.Join(root, "beta", "eden_world.model")},
	}
	res := NewRuntime().Worlds(context.Background(), p)
	if len(res.Worlds) != 2 {
		t.Fatalf("want 2 worlds, got %d (%+v)", len(res.Worlds), res.Worlds)
	}
	byName := map[string]WorldEntry{}
	for _, e := range res.Worlds {
		byName[e.Name] = e
	}
	if byName["alpha"].Blocks != 2 || !byName["alpha"].HasSigns {
		t.Errorf("alpha entry = %+v", byName["alpha"])
	}
	if !byName["beta"].Active || byName["alpha"].Active {
		t.Errorf("active flag wrong: %+v", res.Worlds)
	}
}

func TestBundleRoundTripPreservesGitignore(t *testing.T) {
	root := t.TempDir()
	dir := makeWorldDir(t, root, "castle", true)

	tarBytes, err := packLocalBundle(dir)
	if err != nil {
		t.Fatal(err)
	}
	names := tarEntryNames(t, tarBytes)
	if !hasStr(names, ".gitignore") || !hasStr(names, "eden_world.model") || !hasStr(names, "eden_signs.txt") {
		t.Fatalf("bundle missing a member: %v", names)
	}
	// eden_players.txt was never written -> must not appear.
	if hasStr(names, "eden_players.txt") {
		t.Errorf("bundle carries a file that does not exist: %v", names)
	}

	if err := validateBundle(tarBytes); err != nil {
		t.Fatalf("validateBundle: %v", err)
	}

	// Extract into a fresh dir; .gitignore must land intact.
	out := filepath.Join(root, "restored")
	if err := extractLocalBundle(out, tarBytes); err != nil {
		t.Fatal(err)
	}
	gi, err := os.ReadFile(filepath.Join(out, ".gitignore"))
	if err != nil || !strings.Contains(string(gi), "!.gitignore") {
		t.Fatalf(".gitignore not preserved: %q %v", gi, err)
	}
}

func TestValidateBundleRejections(t *testing.T) {
	// A tar with a traversal / unexpected entry.
	var buf bytes.Buffer
	tw := tar.NewWriter(&buf)
	tw.WriteHeader(&tar.Header{Name: "../evil", Mode: 0o644, Size: 1})
	tw.Write([]byte("x"))
	tw.Close()
	if err := validateBundle(buf.Bytes()); err == nil {
		t.Error("traversal entry should be rejected")
	}

	// A tar with only a sidecar, no model.
	buf.Reset()
	tw = tar.NewWriter(&buf)
	tw.WriteHeader(&tar.Header{Name: "eden_signs.txt", Mode: 0o644, Size: 0})
	tw.Close()
	if err := validateBundle(buf.Bytes()); err == nil {
		t.Error("bundle without eden_world.model should be rejected")
	}
}

func TestUploadBundleRefusedWhileRunning(t *testing.T) {
	root := t.TempDir()
	dir := makeWorldDir(t, root, "castle", false)
	tarBytes, _ := packLocalBundle(dir)

	p := profile.Profile{Name: "dev", Kind: profile.Local, WorldRoot: root, WorldDir: dir, ServerBin: "/bin/sleep"}
	rt := NewRuntime()
	writeSelfPidfile(t, rt.Supervisor(p))

	err := rt.UploadBundle(context.Background(), p, "castle", tarBytes)
	if err == nil || !strings.Contains(err.Error(), "running") {
		t.Fatalf("upload into a running server should be refused, got %v", err)
	}
}

// stubImport writes an eden_import stand-in that records the source path it was
// given (so a test can prove the temp file was cleaned up) and prints a summary.
func stubImport(t *testing.T, srcSink string, stdout, stderr string, exit int) string {
	t.Helper()
	dir := t.TempDir()
	path := filepath.Join(dir, "eden_import")
	script := "#!/bin/sh\n" +
		"printf '%s' \"$1\" > " + shquote(srcSink) + "\n" +
		"cat <<'OUT'\n" + stdout + "\nOUT\n" +
		"cat >&2 <<'ERR'\n" + stderr + "\nERR\n" +
		"exit " + itoaTest(exit) + "\n"
	if err := os.WriteFile(path, []byte(script), 0o755); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestImportProjectLocalCleansUpTempOnSuccess(t *testing.T) {
	root := t.TempDir()
	sink := filepath.Join(root, "src-seen.txt")
	imp := stubImport(t, sink, `world "Stub"  (version 4, 64z, 1 saved chunks, 0 signs from none)
  cells         1  of 4,000,000 cap  (0.0%)
  spawn         65536.00, 33.92, 65536.00  (header)`, "", 0)

	p := profile.Profile{Name: "dev", Kind: profile.Local, WorldRoot: root, EdenImport: imp}
	o := eden.NewImportOptions()
	o.OutDir = filepath.Join(root, "stub")

	res := NewRuntime().ImportProject(context.Background(), p, []byte("PRETEND-EDEN-BYTES"), o)
	if !res.OK || res.Summary.Cells != 1 {
		t.Fatalf("project result: %+v", res)
	}
	seen := readFile(t, sink)
	if seen == "" {
		t.Fatal("stub never saw a source path")
	}
	if _, err := os.Stat(seen); !os.IsNotExist(err) {
		t.Fatalf("temp file %q was not removed after a successful projection (err=%v)", seen, err)
	}
}

func TestImportWriteLocalCleansUpTempOnRefusal(t *testing.T) {
	root := t.TempDir()
	sink := filepath.Join(root, "src-seen.txt")
	imp := stubImport(t, sink,
		`world "Big"  (version 4, 64z, 1024 saved chunks, 0 signs from none)
  cells         4,004,403  of 4,000,000 cap  (100.1%)`,
		`eden_import: error: 4,004,403 cells exceeds the 4,000,000 cap.`, 1)

	p := profile.Profile{Name: "dev", Kind: profile.Local, WorldRoot: root, EdenImport: imp}
	o := eden.NewImportOptions()
	o.OutDir = filepath.Join(root, "big")

	res := NewRuntime().ImportWrite(context.Background(), p, []byte("PRETEND"), o)
	if res.OK || !res.Summary.Refused {
		t.Fatalf("refusal not surfaced: %+v", res)
	}
	seen := readFile(t, sink)
	if _, err := os.Stat(seen); !os.IsNotExist(err) {
		t.Fatalf("temp file %q was not removed after a refusal (err=%v)", seen, err)
	}
}

// --- tiny test helpers ---

func tarEntryNames(t *testing.T, b []byte) []string {
	t.Helper()
	var names []string
	tr := tar.NewReader(bytes.NewReader(b))
	for {
		h, err := tr.Next()
		if err != nil {
			break
		}
		names = append(names, h.Name)
	}
	return names
}

func readFile(t *testing.T, p string) string {
	t.Helper()
	b, err := os.ReadFile(p)
	if err != nil {
		return ""
	}
	return string(b)
}

func hasStr(ss []string, want string) bool {
	for _, s := range ss {
		if s == want {
			return true
		}
	}
	return false
}

func shquote(s string) string { return "'" + strings.ReplaceAll(s, "'", `'\''`) + "'" }

func itoaTest(n int) string {
	if n == 0 {
		return "0"
	}
	neg := n < 0
	if neg {
		n = -n
	}
	var d []byte
	for n > 0 {
		d = append([]byte{byte('0' + n%10)}, d...)
		n /= 10
	}
	if neg {
		d = append([]byte{'-'}, d...)
	}
	return string(d)
}
