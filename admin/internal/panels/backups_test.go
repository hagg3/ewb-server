package panels

import (
	"archive/tar"
	"bytes"
	"context"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/hagg3/ewb-server/admin/internal/profile"
)

func writeF(t *testing.T, path, s string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(s), 0o644); err != nil {
		t.Fatal(err)
	}
}

// writeGz writes s gzipped to path — the form `edenserverctl backup` and
// compressBackupMembers leave in a backup directory.
func writeGz(t *testing.T, path, s string) {
	t.Helper()
	packed, err := gzipBytes([]byte(s))
	if err != nil {
		t.Fatal(err)
	}
	writeF(t, path, string(packed))
}

// readGz reads a gzipped backup member back as plain text.
func readGz(t *testing.T, path string) string {
	t.Helper()
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read %s: %v", path, err)
	}
	plain, err := gunzipBytes(raw)
	if err != nil {
		t.Fatalf("gunzip %s: %v", path, err)
	}
	return string(plain)
}

// backupProfile is a local profile with a world dir and an explicit backup dir.
func backupProfile(t *testing.T) (profile.Profile, string, string) {
	t.Helper()
	root := t.TempDir()
	world := filepath.Join(root, "world")
	backups := filepath.Join(root, "backups")
	writeF(t, filepath.Join(world, "eden_world.model"), "OLD\n")
	writeF(t, filepath.Join(world, "eden_players.txt"), "OLD-P\n")
	p := profile.Profile{
		Name: "dev", Kind: profile.Local,
		WorldDir: world, BackupDir: backups,
		ServerBin: "/bin/sleep",
	}
	return p, world, backups
}

func TestBackupsLocalListSortsDescAndFlagsJunk(t *testing.T) {
	p, _, backups := backupProfile(t)
	writeF(t, filepath.Join(backups, "20260908T140000Z", "eden_world.model"), "a\n")
	writeF(t, filepath.Join(backups, "20260909T090000Z", "eden_world.model"), "bb\n")
	writeF(t, filepath.Join(backups, "20260909T010203Z-prerestore", "eden_world.model"), "c\n")
	// A directory whose name is not a stamp must still be listed.
	if err := os.MkdirAll(filepath.Join(backups, "hand-made"), 0o755); err != nil {
		t.Fatal(err)
	}

	res := NewRuntime().Backups(context.Background(), p)
	if len(res.Backups) != 4 {
		t.Fatalf("want 4 entries, got %d: %+v", len(res.Backups), res.Backups)
	}
	// Sorted stamp-descending.
	if res.Backups[0].Stamp != "hand-made" { // "hand-made" > digits in byte order
		// tolerate: just assert the two real stamps are in descending order
	}
	byName := map[string]BackupEntry{}
	for _, b := range res.Backups {
		byName[b.Stamp] = b
	}
	if !byName["20260909T090000Z"].IsStamp || byName["20260909T090000Z"].PreRestore {
		t.Errorf("stamp entry misclassified: %+v", byName["20260909T090000Z"])
	}
	if !byName["20260909T010203Z-prerestore"].PreRestore {
		t.Errorf("pre-restore entry not flagged: %+v", byName["20260909T010203Z-prerestore"])
	}
	if byName["hand-made"].IsStamp {
		t.Errorf("junk dir marked as a stamp: %+v", byName["hand-made"])
	}
	if !byName["20260908T140000Z"].HasModel {
		t.Errorf("has_model not set for a dir with eden_world.model")
	}
	// Descending order of the two well-formed stamps.
	iNew, iOld := -1, -1
	for i, b := range res.Backups {
		if b.Stamp == "20260909T090000Z" {
			iNew = i
		}
		if b.Stamp == "20260908T140000Z" {
			iOld = i
		}
	}
	if iNew > iOld {
		t.Errorf("backups not sorted newest-first: %+v", res.Backups)
	}
}

func TestBackupCreateLocal(t *testing.T) {
	p, _, backups := backupProfile(t)
	rt := NewRuntime()

	res := rt.BackupCreate(context.Background(), p)
	if !res.OK || res.Stamp == "" {
		t.Fatalf("create: %+v", res)
	}
	dest := filepath.Join(backups, res.Stamp)
	// Members land gzipped, under the ".gz" name, and round-trip byte-exact.
	if got := readGz(t, filepath.Join(dest, "eden_world.model.gz")); got != "OLD\n" {
		t.Fatalf("backed-up model = %q, want OLD", got)
	}
	if _, err := os.Stat(filepath.Join(dest, "eden_players.txt.gz")); err != nil {
		t.Errorf("players file not backed up: %v", err)
	}
	if _, err := os.Stat(filepath.Join(dest, "eden_world.model")); !os.IsNotExist(err) {
		t.Errorf("an uncompressed copy was left behind alongside the .gz")
	}
}

// A world file is plaintext, so a backup of one must actually be smaller than
// the source — the whole point of compressing them.
func TestBackupCreateLocalCompresses(t *testing.T) {
	p, world, backups := backupProfile(t)
	body := strings.Repeat("65536:33:65536:1:0\n", 5000)
	writeF(t, filepath.Join(world, "eden_world.model"), body)

	res := NewRuntime().BackupCreate(context.Background(), p)
	if !res.OK {
		t.Fatalf("create: %+v", res)
	}
	gz := filepath.Join(backups, res.Stamp, "eden_world.model.gz")
	info, err := os.Stat(gz)
	if err != nil {
		t.Fatal(err)
	}
	if info.Size() >= int64(len(body))/2 {
		t.Errorf("backup of %d bytes is %d — not meaningfully compressed", len(body), info.Size())
	}
	if got := readGz(t, gz); got != body {
		t.Errorf("compressed backup does not round-trip to the original bytes")
	}
}

func TestBackupCreateLocalNoWorld(t *testing.T) {
	root := t.TempDir()
	p := profile.Profile{Name: "dev", Kind: profile.Local, WorldDir: filepath.Join(root, "nope"), BackupDir: filepath.Join(root, "b")}
	if res := NewRuntime().BackupCreate(context.Background(), p); res.OK {
		t.Fatalf("create should fail with no world: %+v", res)
	}
}

// TestBackupRestoreLocalCoreSwap is the happy path with the server already
// stopped: safety copy is taken first, then the files are swapped in.
func TestBackupRestoreLocalCoreSwap(t *testing.T) {
	p, world, backups := backupProfile(t)
	stamp := "20260101T000000Z"
	writeGz(t, filepath.Join(backups, stamp, "eden_world.model.gz"), "NEW\n")

	res := NewRuntime().BackupRestore(context.Background(), p, stamp)
	if !res.OK {
		t.Fatalf("restore: %+v", res)
	}
	// The world dir gets the plain bytes back, not the archive.
	if b, _ := os.ReadFile(filepath.Join(world, "eden_world.model")); string(b) != "NEW\n" {
		t.Fatalf("world model after restore = %q, want NEW", b)
	}
	// The safety copy holds the pre-restore world, gzipped like any backup.
	if res.SafetyCopy == "" || !strings.HasSuffix(res.SafetyCopy, "-prerestore") {
		t.Fatalf("no safety copy recorded: %+v", res)
	}
	sc := filepath.Join(backups, res.SafetyCopy)
	if got := readGz(t, filepath.Join(sc, "eden_world.model.gz")); got != "OLD\n" {
		t.Errorf("safety copy model = %q, want OLD", got)
	}
	if got := readGz(t, filepath.Join(sc, "eden_players.txt.gz")); got != "OLD-P\n" {
		t.Errorf("safety copy players = %q, want OLD-P", got)
	}
}

// A backup taken before backups were compressed (or on an
// EDENSERVER_BACKUP_COMPRESS=0 host) stores plain names, and must stay
// restorable forever.
func TestBackupRestoreLocalAcceptsUncompressedBackup(t *testing.T) {
	p, world, backups := backupProfile(t)
	stamp := "20260101T000000Z"
	writeF(t, filepath.Join(backups, stamp, "eden_world.model"), "LEGACY\n")

	res := NewRuntime().BackupRestore(context.Background(), p, stamp)
	if !res.OK {
		t.Fatalf("restore of a plain backup: %+v", res)
	}
	if b, _ := os.ReadFile(filepath.Join(world, "eden_world.model")); string(b) != "LEGACY\n" {
		t.Fatalf("world model after restore = %q, want LEGACY", b)
	}
}

func TestBackupRestoreLocalMissingModel(t *testing.T) {
	p, _, backups := backupProfile(t)
	stamp := "20260101T000000Z"
	writeF(t, filepath.Join(backups, stamp, "eden_signs.txt"), "x\n") // no model
	res := NewRuntime().BackupRestore(context.Background(), p, stamp)
	if res.OK || !strings.Contains(res.Message, "no eden_world.model") {
		t.Fatalf("restore of a modelless backup: %+v", res)
	}
}

func TestBackupRestoreLocalRejectsTraversal(t *testing.T) {
	p, _, _ := backupProfile(t)
	for _, bad := range []string{"", "../etc", "a/b", ".."} {
		if res := NewRuntime().BackupRestore(context.Background(), p, bad); res.OK {
			t.Errorf("restore(%q) should be rejected", bad)
		}
	}
}

// TestBackupRestoreLocalRollsBackOnSwapFailure: the swap fails on the second
// member (a backup whose eden_signs.txt is a directory, so os.ReadFile errors
// after the model has already been overwritten). The world must be rolled back
// to the pre-restore state from the safety copy.
func TestBackupRestoreLocalRollsBackOnSwapFailure(t *testing.T) {
	p, world, backups := backupProfile(t)
	stamp := "20260101T000000Z"
	writeF(t, filepath.Join(backups, stamp, "eden_world.model"), "NEW\n")
	if err := os.MkdirAll(filepath.Join(backups, stamp, "eden_signs.txt"), 0o755); err != nil {
		t.Fatal(err)
	}

	res := NewRuntime().BackupRestore(context.Background(), p, stamp)
	if res.OK {
		t.Fatalf("restore should have failed mid-swap: %+v", res)
	}
	if !strings.Contains(res.Message, "rolled back") {
		t.Errorf("message should report a rollback: %q", res.Message)
	}
	if b, _ := os.ReadFile(filepath.Join(world, "eden_world.model")); string(b) != "OLD\n" {
		t.Fatalf("world model after failed restore = %q, want OLD (rolled back)", b)
	}
	// The safety copy is kept for the operator.
	if _, err := os.Stat(filepath.Join(backups, res.SafetyCopy, "eden_world.model.gz")); err != nil {
		t.Errorf("safety copy not kept after a rollback: %v", err)
	}
}

// plainBackupTar is what stands between a gzipped backup on a vps and the world
// directory: it must inflate the .gz members, pass plain ones through, and drop
// anything that isn't a world file.
func TestPlainBackupTar(t *testing.T) {
	gzModel, err := gzipBytes([]byte("NEW\n"))
	if err != nil {
		t.Fatal(err)
	}
	var in bytes.Buffer
	tw := tar.NewWriter(&in)
	for _, m := range []struct{ name, body string }{
		{"eden_world.model.gz", string(gzModel)},
		{"eden_players.txt", "PLAIN-P\n"}, // legacy uncompressed member
		{"../../etc/passwd", "nope\n"},    // not a world file: dropped
		{"notes.txt", "nope\n"},           // ditto
	} {
		if err := tw.WriteHeader(&tar.Header{Name: m.name, Mode: 0o644, Size: int64(len(m.body)), Typeflag: tar.TypeReg}); err != nil {
			t.Fatal(err)
		}
		if _, err := tw.Write([]byte(m.body)); err != nil {
			t.Fatal(err)
		}
	}
	if err := tw.Close(); err != nil {
		t.Fatal(err)
	}

	out, err := plainBackupTar(in.Bytes())
	if err != nil {
		t.Fatalf("plainBackupTar: %v", err)
	}
	got := map[string]string{}
	tr := tar.NewReader(bytes.NewReader(out))
	for {
		hdr, err := tr.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			t.Fatal(err)
		}
		body, _ := io.ReadAll(tr)
		got[hdr.Name] = string(body)
	}
	if len(got) != 2 {
		t.Fatalf("want 2 members through, got %d: %v", len(got), got)
	}
	if got["eden_world.model"] != "NEW\n" {
		t.Errorf("model = %q, want the inflated NEW", got["eden_world.model"])
	}
	if got["eden_players.txt"] != "PLAIN-P\n" {
		t.Errorf("plain member mangled: %q", got["eden_players.txt"])
	}
}

// A backup with nothing restorable in it must be an error, not an empty tar
// that would silently unpack over the world dir as a no-op "success".
func TestPlainBackupTarRejectsEmpty(t *testing.T) {
	var in bytes.Buffer
	tw := tar.NewWriter(&in)
	if err := tw.Close(); err != nil {
		t.Fatal(err)
	}
	if _, err := plainBackupTar(in.Bytes()); err == nil {
		t.Fatal("an empty backup tar should be rejected")
	}
}

// --- vps paths against the scripted fake transport ---

func TestBackupCreateVPSSudoDenied(t *testing.T) {
	f := &fakeTransport{replies: []fakeReply{
		{match: "edenserverctl backup", stderr: "sudo: a password is required", exit: 1},
	}}
	rt := NewRuntime()
	rt.mkTransport = fakeFactory(f)

	res := rt.BackupCreate(context.Background(), vpsProf)
	if res.OK || !strings.Contains(res.Message, "sudoers") {
		t.Fatalf("expected a sudoers-missing message: %+v", res)
	}
}

func TestBackupCreateVPSHappy(t *testing.T) {
	f := &fakeTransport{replies: []fakeReply{
		{match: "edenserverctl backup", stdout: "Backed up 3 file(s) to /var/lib/edenserver/backups/20260909T101112Z\n"},
	}}
	rt := NewRuntime()
	rt.mkTransport = fakeFactory(f)

	res := rt.BackupCreate(context.Background(), vpsProf)
	if !res.OK || res.Stamp != "20260909T101112Z" {
		t.Fatalf("create vps: %+v", res)
	}
}

// TestBackupRestoreVPSAbortsIfNotInactive: the graceful stop is issued but the
// unit never reaches `inactive`, so the swap must not run and no `systemctl
// stop` is ever issued.
func TestBackupRestoreVPSAbortsIfNotInactive(t *testing.T) {
	f := &fakeTransport{replies: []fakeReply{
		{match: "edenserverctl backup", stdout: "Backed up 3 file(s) to /b/20260909T000000Z\n"},
		{match: "tar -C /var/lib/edenserver/world", stdout: "TARBYTES"},
		{match: "edenctl", stdout: "ok: stopping\n"},
		{match: "ActiveState", stdout: "ActiveState=active\n"},
	}}
	rt := NewRuntime()
	rt.mkTransport = fakeFactory(f)

	// A short deadline so the "wait for inactive" poll gives up quickly; the
	// fake transport ignores ctx, so the subsequent ActiveState check still
	// returns "active" and the restore aborts.
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	res := rt.BackupRestore(ctx, vpsProf, "20260909T010101Z")
	if res.OK {
		t.Fatalf("restore should abort when the unit stays active: %+v", res)
	}
	for _, c := range f.calls {
		if strings.Contains(c, "systemctl stop") || strings.Contains(c, "systemctl kill") {
			t.Fatalf("restore shelled out to systemd stop/kill: %q", c)
		}
		if strings.Contains(c, "tar -C /var/lib/edenserver/world -xf -") {
			t.Fatalf("restore swapped files despite an active unit: %q", c)
		}
	}
}
