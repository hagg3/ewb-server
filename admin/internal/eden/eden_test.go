package eden

import (
	"strings"
	"testing"

	"github.com/hagg3/ewb-server/admin/internal/profile"
	"github.com/hagg3/ewb-server/admin/internal/transport"
)

var localP = profile.Profile{
	Name:       "dev",
	Kind:       profile.Local,
	WorldDir:   "/home/sam/ewb-server/worlds/dev",
	WorldRoot:  "/home/sam/ewb-server/worlds",
	ServerBin:  "/home/sam/ewb-server/edenserver",
	EdenImport: "/home/sam/ewb-server/eden_import",
}

var vpsP = profile.Profile{
	Name:       "prod",
	Kind:       profile.VPS,
	SSHHost:    "eden-vps",
	Service:    "edenserver",
	RunAs:      "edenserver",
	WorldDir:   "/var/lib/edenserver/world",
	WorldRoot:  "/var/lib/edenserver/worlds",
	EdenImport: "/usr/local/bin/eden_import",
	Edenctl:    "/usr/local/bin/edenctl",
}

func joinLocal(argv []string) string { return strings.Join(argv, " ") }

// Same table as protocol_test.cpp's test_cell_cap_headroom — the two must agree.
func TestRecommendedMaxWorldCells(t *testing.T) {
	for _, c := range []struct {
		cells int64
		want  int
	}{
		{0, 1000000},
		{3433, 1100000},
		{2200053, 3300000},
		{13920369, 17500000},
		{-5, 1000000},
	} {
		if got := RecommendedMaxWorldCells(c.cells); got != c.want {
			t.Errorf("RecommendedMaxWorldCells(%d) = %d, want %d", c.cells, got, c.want)
		}
	}
}

func TestCtlArgvLocal(t *testing.T) {
	cases := []struct {
		got  []string
		want string
	}{
		{Who(localP), "/home/sam/ewb-server/edenctl -S /home/sam/ewb-server/worlds/dev/edenserver.sock who"},
		{Say(localP, "hello world"), "/home/sam/ewb-server/edenctl -S /home/sam/ewb-server/worlds/dev/edenserver.sock say hello world"},
		{Kick(localP, "Griefer", "spam"), "/home/sam/ewb-server/edenctl -S /home/sam/ewb-server/worlds/dev/edenserver.sock kick Griefer spam"},
		{Ban(localP, "10.0.0.9"), "/home/sam/ewb-server/edenctl -S /home/sam/ewb-server/worlds/dev/edenserver.sock ban 10.0.0.9"},
		{Op(localP, "Sam", "2"), "/home/sam/ewb-server/edenctl -S /home/sam/ewb-server/worlds/dev/edenserver.sock op Sam 2"},
		{CtlStop(localP), "/home/sam/ewb-server/edenctl -S /home/sam/ewb-server/worlds/dev/edenserver.sock stop"},
	}
	for _, c := range cases {
		if got := joinLocal(c.got); got != c.want {
			t.Errorf("local argv = %q\n            want %q", got, c.want)
		}
	}
}

func TestCtlArgvVPSIsSudoWrapped(t *testing.T) {
	got := Who(vpsP)
	want := []string{"sudo", "-n", "-u", "edenserver", "/usr/local/bin/edenctl", "-S", "/var/lib/edenserver/world/edenserver.sock", "who"}
	if strings.Join(got, "\x00") != strings.Join(want, "\x00") {
		t.Fatalf("vps who argv = %v\n              want %v", got, want)
	}
}

// The full round trip a vps command takes: eden builder -> ssh transport -> one
// shell-quoted string. A space in `say` text must survive.
func TestCtlVPSThroughSSHTransport(t *testing.T) {
	s := transport.SSH{Host: "eden-vps", ControlPath: "~/.ssh/cm"}
	disp := s.Describe(transport.Command{Argv: Say(vpsP, "back in 5 minutes")})
	want := `'sudo -n -u edenserver /usr/local/bin/edenctl -S /var/lib/edenserver/world/edenserver.sock say '\''back in 5 minutes'\'''`
	if !strings.Contains(disp, want) {
		t.Fatalf("ssh display:\n  %s\nwant substring:\n  %s", disp, want)
	}
}

func TestHostVerbs(t *testing.T) {
	if got := joinLocal(Restart(vpsP)); got != "sudo -n systemctl restart edenserver" {
		t.Errorf("Restart = %q", got)
	}
	if got := joinLocal(Kill(vpsP)); got != "sudo -n systemctl kill -s SIGKILL edenserver" {
		t.Errorf("Kill = %q", got)
	}
	if got := joinLocal(SystemctlShow(vpsP, "ActiveState", "MainPID")); got != "systemctl show edenserver -p ActiveState -p MainPID" {
		t.Errorf("SystemctlShow = %q", got)
	}
	if got := joinLocal(Journal(vpsP, 200, "")); got != "journalctl -u edenserver --no-pager -o json -n 200" {
		t.Errorf("Journal first = %q", got)
	}
	if got := joinLocal(Journal(vpsP, 200, "s=abc;i=1")); got != "journalctl -u edenserver --no-pager -o json --after-cursor s=abc;i=1" {
		t.Errorf("Journal incremental = %q", got)
	}
	if got := joinLocal(JournalTail(vpsP, 50)); got != "journalctl -u edenserver -n 50 --no-pager" {
		t.Errorf("JournalTail = %q", got)
	}
	if got := joinLocal(AuditFileTail(vpsP, "/var/log/edenserver/audit.log", 2000)); got != "sudo -n -u edenserver tail -n 2000 -- /var/log/edenserver/audit.log" {
		t.Errorf("AuditFileTail = %q", got)
	}
}

func TestImportArgvLocal(t *testing.T) {
	o := NewImportOptions()
	o.Src = "/tmp/edenadmin-x.eden"
	o.OutDir = "/home/sam/ewb-server/worlds/castle"
	o.AirFill = "diff"
	o.Spawn = "header"

	dry := joinLocal(ImportDryRun(localP, o))
	wantDry := "/home/sam/ewb-server/eden_import /tmp/edenadmin-x.eden --out /home/sam/ewb-server/worlds/castle --yes --dry-run --air-fill diff --spawn header"
	if dry != wantDry {
		t.Errorf("dry-run argv:\n  %s\nwant:\n  %s", dry, wantDry)
	}

	real := joinLocal(ImportRun(localP, o))
	if strings.Contains(real, "--dry-run") {
		t.Errorf("real import must not carry --dry-run: %s", real)
	}
	if !strings.Contains(real, "--yes") {
		t.Errorf("real import must carry --yes: %s", real)
	}
}

func TestImportArgvYesAlwaysPresent(t *testing.T) {
	for _, argv := range [][]string{
		ImportDryRun(localP, ImportOptions{Src: "a", OutDir: "b"}),
		ImportRun(localP, ImportOptions{Src: "a", OutDir: "b"}),
		ImportDryRun(vpsP, ImportOptions{Src: "a", OutDir: "b"}),
		ImportRun(vpsP, ImportOptions{Src: "a", OutDir: "b"}),
	} {
		if !contains(argv, "--yes") {
			t.Errorf("--yes missing from %v", argv)
		}
	}
}

func TestImportArgvOverrides(t *testing.T) {
	o := NewImportOptions()
	o.Src, o.OutDir = "in.eden", "/w/out"
	o.Force = true
	o.Strict = true
	o.NoSigns = true
	o.MaxWorldCells = 6_000_000
	o.MaxRegionRecords = 3_000_000
	o.RegionRadius = 128
	got := joinLocal(ImportRun(localP, o))
	for _, want := range []string{
		"--force", "--strict", "--no-signs",
		"--max-world-cells 6000000", "--max-region-records 3000000", "--region-radius 128",
	} {
		if !strings.Contains(got, want) {
			t.Errorf("missing %q in %s", want, got)
		}
	}
}

func TestImportArgvMaxRegionRecordsSentinel(t *testing.T) {
	// -1 = omit; 0 = a real value meaning "disable the check".
	o := ImportOptions{Src: "a", OutDir: "b", MaxRegionRecords: -1}
	if strings.Contains(joinLocal(ImportRun(localP, o)), "--max-region-records") {
		t.Error("MaxRegionRecords -1 should be omitted")
	}
	o.MaxRegionRecords = 0
	if !strings.Contains(joinLocal(ImportRun(localP, o)), "--max-region-records 0") {
		t.Error("MaxRegionRecords 0 should be emitted explicitly")
	}
}

func TestImportVPSIsSudoWrapped(t *testing.T) {
	argv := ImportDryRun(vpsP, ImportOptions{Src: "/tmp/x.eden", OutDir: "/var/lib/edenserver/worlds/c", MaxRegionRecords: -1})
	if argv[0] != "sudo" || argv[3] != "edenserver" || argv[4] != "/usr/local/bin/eden_import" {
		t.Fatalf("vps import argv not sudo-wrapped: %v", argv)
	}
}

func TestImportHelp(t *testing.T) {
	if got := joinLocal(ImportHelp(localP)); got != "/home/sam/ewb-server/eden_import -h" {
		t.Errorf("ImportHelp local = %q", got)
	}
	if got := joinLocal(ImportHelp(vpsP)); got != "sudo -n -u edenserver /usr/local/bin/eden_import -h" {
		t.Errorf("ImportHelp vps = %q", got)
	}
}

func TestWorldListArgv(t *testing.T) {
	got := joinLocal(WorldList("/var/lib/edenserver/worlds"))
	want := "find /var/lib/edenserver/worlds -mindepth 2 -maxdepth 2 -name eden_world.model -printf %h\n"
	if got != want {
		t.Errorf("WorldList = %q, want %q", got, want)
	}
}

func TestWorldBundlePackWrapsForVPS(t *testing.T) {
	got := WorldBundlePack(vpsP, "/var/lib/edenserver/worlds/castle")
	want := []string{
		"sudo", "-n", "-u", "edenserver",
		"tar", "-C", "/var/lib/edenserver/worlds/castle", "--ignore-failed-read", "-cf", "-",
		".gitignore", "eden_world.model", "eden_signs.txt", "eden_spawn.txt", "eden_players.txt",
	}
	if strings.Join(got, "\x00") != strings.Join(want, "\x00") {
		t.Fatalf("WorldBundlePack vps =\n  %v\nwant\n  %v", got, want)
	}
	// local: no sudo prefix.
	if l := WorldBundlePack(localP, "/w/castle"); l[0] != "tar" {
		t.Errorf("WorldBundlePack local should not be sudo-wrapped: %v", l)
	}
}

func TestTmpWriteAndRemoveVPS(t *testing.T) {
	if got := joinLocal(TmpWrite(vpsP, "/tmp/edenadmin-ab12.eden")); got != "sudo -n -u edenserver dd of=/tmp/edenadmin-ab12.eden status=none" {
		t.Errorf("TmpWrite vps = %q", got)
	}
	if got := joinLocal(TmpRemove(vpsP, "/tmp/edenadmin-ab12.eden")); got != "sudo -n -u edenserver rm -f /tmp/edenadmin-ab12.eden" {
		t.Errorf("TmpRemove vps = %q", got)
	}
}

func TestBackupDiskUsageArgv(t *testing.T) {
	got := joinLocal(BackupDiskUsage("/var/lib/edenserver/backups"))
	if got != "du -k --max-depth=1 /var/lib/edenserver/backups" {
		t.Errorf("BackupDiskUsage = %q", got)
	}
}

func TestBackupNowVPSIsSudoEnvWrapped(t *testing.T) {
	got := joinLocal(BackupNow(vpsP))
	want := "sudo -n -u edenserver env EDENSERVER_WORLD_DIR=/var/lib/edenserver/world " +
		"EDENSERVER_BACKUP_DIR=/var/lib/edenserver/backups edenserverctl backup"
	if got != want {
		t.Errorf("BackupNow vps =\n  %q\nwant\n  %q", got, want)
	}
}

func TestBackupPackWrapsForVPS(t *testing.T) {
	got := BackupPack(vpsP, "/var/lib/edenserver/backups/20260909T090000Z")
	want := []string{
		"sudo", "-n", "-u", "edenserver",
		"tar", "-C", "/var/lib/edenserver/backups/20260909T090000Z", "--ignore-failed-read", "-cf", "-",
		// Both forms of each member: edenserverctl gzips by default, but plain
		// names survive from older backups and BACKUP_COMPRESS=0 hosts.
		"eden_world.model.gz", "eden_world.model",
		"eden_signs.txt.gz", "eden_signs.txt",
		"eden_spawn.txt.gz", "eden_spawn.txt",
		"eden_players.txt.gz", "eden_players.txt",
	}
	if strings.Join(got, "\x00") != strings.Join(want, "\x00") {
		t.Fatalf("BackupPack vps =\n  %v\nwant\n  %v", got, want)
	}
	if l := BackupPack(localP, "/b/x"); l[0] != "tar" {
		t.Errorf("BackupPack local should not be sudo-wrapped: %v", l)
	}
}

func TestConfArgv(t *testing.T) {
	if got := joinLocal(ConfRead(vpsP)); got != "cat /etc/edenserver.conf" {
		t.Errorf("ConfRead = %q", got)
	}
	if got := joinLocal(ConfReadSudo(vpsP)); got != "sudo -n cat /etc/edenserver.conf" {
		t.Errorf("ConfReadSudo = %q", got)
	}
	if got := joinLocal(ConfWrite(vpsP)); got != "sudo -n /usr/local/bin/edenserver-writeconf" {
		t.Errorf("ConfWrite = %q", got)
	}

	// A profile with a non-default env_file threads it through both the read and
	// the `env EDENSERVER_CONF=` write form.
	ef := vpsP
	ef.EnvFile = "/etc/eden/world2.conf"
	if got := joinLocal(ConfRead(ef)); got != "cat /etc/eden/world2.conf" {
		t.Errorf("ConfRead env_file = %q", got)
	}
	if got := joinLocal(ConfWrite(ef)); got != "sudo -n env EDENSERVER_CONF=/etc/eden/world2.conf /usr/local/bin/edenserver-writeconf" {
		t.Errorf("ConfWrite env_file = %q", got)
	}
}

func contains(ss []string, want string) bool {
	for _, s := range ss {
		if s == want {
			return true
		}
	}
	return false
}
