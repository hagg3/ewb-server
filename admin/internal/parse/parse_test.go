package parse

import (
	"os"
	"path/filepath"
	"testing"
)

// golden reads a file from admin/testdata/golden/. The corpus was captured from
// a real local edenserver / eden_import (see the commit that added stage 6.2);
// the host-format files (systemctl / df / journalctl) are hand-written Linux
// samples since no Linux host has been exercised yet.
func golden(t *testing.T, name string) []byte {
	t.Helper()
	b, err := os.ReadFile(filepath.Join("..", "..", "testdata", "golden", name))
	if err != nil {
		t.Fatalf("golden %s: %v", name, err)
	}
	return b
}

func TestWhoTwoPlayers(t *testing.T) {
	ps, err := Who(golden(t, "who_two.txt"))
	if err != nil {
		t.Fatalf("Who: %v", err)
	}
	if len(ps) != 2 {
		t.Fatalf("got %d players, want 2", len(ps))
	}
	if ps[0] != (Player{Name: "Alice", Type: 0, IP: "127.0.0.1", X: 65540, Y: 34, Z: 65530, Level: 2}) {
		t.Errorf("player 0 = %+v", ps[0])
	}
	// The name with a space must survive intact.
	if ps[1].Name != "Bob Marley" || ps[1].Type != 3 || ps[1].Level != 0 {
		t.Errorf("player 1 = %+v", ps[1])
	}
}

func TestWhoEmpty(t *testing.T) {
	ps, err := Who(golden(t, "who_empty.txt"))
	if err != nil {
		t.Fatalf("Who empty: %v", err)
	}
	if len(ps) != 0 {
		t.Errorf("want no players, got %d", len(ps))
	}
}

func TestWhoCRLF(t *testing.T) {
	ps, err := Who(golden(t, "who_crlf.txt"))
	if err != nil {
		t.Fatalf("Who CRLF: %v", err)
	}
	if len(ps) != 1 || ps[0].Name != "Carol" || ps[0].IP != "10.0.0.5" {
		t.Errorf("CRLF parse = %+v", ps)
	}
}

func TestWhoTruncated(t *testing.T) {
	_, err := Who(golden(t, "who_truncated.txt"))
	if err == nil {
		t.Fatal("truncated who should be a parse error, not a partial result")
	}
}

func TestBanlist(t *testing.T) {
	es, err := Banlist(golden(t, "banlist_three.txt"))
	if err != nil {
		t.Fatalf("Banlist: %v", err)
	}
	want := []BanEntry{
		{Token: "10", Kind: "ip"},
		{Token: "203.0.113.9", Kind: "ip"},
		{Token: "Griefer", Kind: "name"},
	}
	if len(es) != 3 {
		t.Fatalf("got %d entries, want 3: %+v", len(es), es)
	}
	for i := range want {
		if es[i] != want[i] {
			t.Errorf("entry %d = %+v, want %+v", i, es[i], want[i])
		}
	}
}

func TestBanlistEmpty(t *testing.T) {
	es, err := Banlist(golden(t, "banlist_empty.txt"))
	if err != nil {
		t.Fatalf("Banlist empty: %v", err)
	}
	if len(es) != 0 {
		t.Errorf("want no entries, got %+v", es)
	}
}

func TestBanlistZeroHeader(t *testing.T) {
	// "0 ban entr(y/ies):" with no rows is a valid, if odd, reply.
	es, err := Banlist(golden(t, "banlist_zero_header.txt"))
	if err != nil {
		t.Fatalf("Banlist zero header: %v", err)
	}
	if len(es) != 0 {
		t.Errorf("want 0, got %+v", es)
	}
}

func TestRegionStats(t *testing.T) {
	rs, err := RegionStatsParse(golden(t, "region_stats_zero.txt"))
	if err != nil {
		t.Fatalf("RegionStats: %v", err)
	}
	if rs != (RegionStats{}) {
		t.Errorf("fresh server region-stats should be all zero, got %+v", rs)
	}
}

func TestAck(t *testing.T) {
	cases := []struct {
		in   string
		kind AckKind
	}{
		{"ok", AckOK},
		{"ok: kicked Griefer", AckOK},
		{"error: no player named 'x'", AckError},
		{"usage: op <name> <0..2>", AckUsage},
		{"REGION service since start:", AckUnknown},
	}
	for _, c := range cases {
		if k, _ := Ack([]byte(c.in)); k != c.kind {
			t.Errorf("Ack(%q) = %q, want %q", c.in, k, c.kind)
		}
	}
}

func TestSystemctlShow(t *testing.T) {
	u, err := UnitParse(golden(t, "systemctl_show_active.txt"))
	if err != nil {
		t.Fatalf("UnitParse: %v", err)
	}
	if u.ActiveState != "active" || u.MainPID != 48213 || u.LoadState != "loaded" {
		t.Errorf("unit = %+v", u)
	}
	// The inactive unit prints an empty ExecMainStartTimestamp= — must not error.
	u2, err := UnitParse(golden(t, "systemctl_show_inactive.txt"))
	if err != nil {
		t.Fatalf("UnitParse inactive: %v", err)
	}
	if u2.ActiveState != "inactive" || u2.MainPID != 0 || u2.StartTimestamp != "" {
		t.Errorf("inactive unit = %+v", u2)
	}
}

func TestDfPk(t *testing.T) {
	d, err := DfPk(golden(t, "df_pk.txt"))
	if err != nil {
		t.Fatalf("DfPk: %v", err)
	}
	if d.BlocksKiB != 41251136 || d.AvailKiB != 27100480 || d.CapacityPct != 31 {
		t.Errorf("disk = %+v", d)
	}
	if d.MountedOn != "/var/lib/edenserver" {
		t.Errorf("mount = %q", d.MountedOn)
	}
}

func TestJournal(t *testing.T) {
	js, err := Journal(golden(t, "journalctl_json.txt"))
	if err != nil {
		t.Fatalf("Journal: %v", err)
	}
	if len(js) != 4 {
		t.Fatalf("got %d lines, want 4", len(js))
	}
	if js[0].Message != "[Server] listening on 27015" || js[0].Priority != 6 {
		t.Errorf("line 0 = %+v", js[0])
	}
	if js[len(js)-1].Cursor == "" {
		t.Error("last cursor is empty — incremental poll would restart from the top")
	}
	// The array-encoded MESSAGE decodes to its bytes.
	if js[3].Message != "[Audit]" {
		t.Errorf("array message = %q, want %q", js[3].Message, "[Audit]")
	}
}

func TestImportSummaryOK(t *testing.T) {
	s, err := ImportSummaryParse(golden(t, "import_ok_dry.out.txt"), golden(t, "import_ok_dry.err.txt"), 0)
	if err != nil {
		t.Fatalf("ImportSummaryParse: %v", err)
	}
	if s.World != "BlockmapWIP" || s.Version != 4 || s.ZCeiling != 64 || s.Chunks != 38 {
		t.Errorf("world line = %+v", s)
	}
	if s.Cells != 3433 || s.Cap != 4_000_000 {
		t.Errorf("cells=%d cap=%d", s.Cells, s.Cap)
	}
	if s.WorstRegionRecords != 5439 || s.WorstRegionBox != [4]int{64704, 65167, 65648, 66111} {
		t.Errorf("worst region = %d %v", s.WorstRegionRecords, s.WorstRegionBox)
	}
	if !s.HasSpawn || s.SpawnSource != "header" {
		t.Errorf("spawn = %+v", s)
	}
	if !s.DryRun || s.Refused || len(s.Verdicts) != 0 {
		t.Errorf("dry=%v refused=%v verdicts=%+v", s.DryRun, s.Refused, s.Verdicts)
	}
	if s.BBox != [6]int{64714, 64815, 32, 53, 65655, 65757} {
		t.Errorf("bbox = %v", s.BBox)
	}
}

func TestImportSummaryReal(t *testing.T) {
	s, err := ImportSummaryParse(golden(t, "import_ok_real.out.txt"), golden(t, "import_ok_real.err.txt"), 0)
	if err != nil {
		t.Fatalf("ImportSummaryParse: %v", err)
	}
	if s.DryRun {
		t.Error("real run flagged as dry-run")
	}
	if len(s.Wrote) != 4 {
		t.Errorf("wrote %d files, want 4: %v", len(s.Wrote), s.Wrote)
	}
}

func TestImportSummaryOverCap(t *testing.T) {
	s, err := ImportSummaryParse(golden(t, "import_overcap_only.out.txt"), golden(t, "import_overcap_only.err.txt"), 1)
	if err != nil {
		t.Fatalf("ImportSummaryParse: %v", err)
	}
	if !s.Refused {
		t.Error("exit 1 must set Refused")
	}
	var kinds []string
	for _, v := range s.Verdicts {
		kinds = append(kinds, v.Kind)
	}
	if !contains(kinds, VerdictCellsOverCap) {
		t.Errorf("verdicts = %v, want a %s", kinds, VerdictCellsOverCap)
	}
}

func TestImportVerdictLowHeadroom(t *testing.T) {
	stderr := []byte("eden_import: warning: 3,900,000 cells leaves only 100,000 of the 4,000,000 cap" +
		" left for players to build. Start edenserver with --max-world-cells 4900000 or higher," +
		" or new blocks stop saving once that room is used.\n")
	vs := parseVerdicts(stderr)
	if len(vs) != 1 || vs[0].Level != "warning" || vs[0].Kind != VerdictCellsLowHeadroom {
		t.Errorf("low-headroom verdict = %+v", vs)
	}
}

func TestImportSummaryOverRegion(t *testing.T) {
	s, _ := ImportSummaryParse(golden(t, "import_overregion_only.out.txt"), golden(t, "import_overregion_only.err.txt"), 1)
	if !s.Refused || len(s.Verdicts) == 0 || s.Verdicts[0].Kind != VerdictRegionOverCeiling {
		t.Errorf("over-region verdict = %+v", s.Verdicts)
	}
}

func TestImportSummaryType255(t *testing.T) {
	s, _ := ImportSummaryParse(golden(t, "import_type255.out.txt"), golden(t, "import_type255.err.txt"), 1)
	if len(s.Verdicts) != 1 || s.Verdicts[0].Kind != VerdictType255 || s.Verdicts[0].Level != "error" {
		t.Fatalf("type-255 verdict = %+v", s.Verdicts)
	}
}

func TestImportSummaryOverridesAccepted(t *testing.T) {
	s, _ := ImportSummaryParse(golden(t, "import_ok_overrides.out.txt"), golden(t, "import_ok_overrides.err.txt"), 0)
	if s.Refused {
		t.Error("exit 0 must not be Refused")
	}
	// The residual "larger than any reply ever captured" note stays as a warning.
	if len(s.Verdicts) != 1 || s.Verdicts[0].Level != "warning" || s.Verdicts[0].Kind != VerdictRegionOverObserved {
		t.Errorf("verdicts = %+v", s.Verdicts)
	}
}

func TestParsersNeverPanic(t *testing.T) {
	junk := [][]byte{
		nil, {}, []byte("\n\n"), []byte("garbage"), []byte("  \t  "),
		[]byte("1 player(s):\n  "), []byte("ban entr(y/ies):"),
		[]byte("{"), []byte("Filesystem\n"),
	}
	for _, b := range junk {
		_, _ = Who(b)
		_, _ = Banlist(b)
		_, _ = RegionStatsParse(b)
		_, _ = SystemctlShow(b)
		_, _ = DfPk(b)
		_, _ = Journal(b)
		_, _ = ImportSummaryParse(b, b, 0)
		_, _ = Ack(b)
		_, _ = DuDirs(b)
		_ = AuditLines(b)
		_ = AuditLine(string(b))
	}
}

func TestAuditLines(t *testing.T) {
	es := AuditLines(golden(t, "audit_sample.txt"))
	if len(es) != 4 {
		t.Fatalf("got %d events, want 4: %+v", len(es), es)
	}
	if es[1] != (AuditEvent{
		Time:  "2026-09-09T14:03:19Z",
		Actor: "player:hagge",
		What:  "(level 1) //set: 512 cell(s)",
		Raw:   "[Audit] 2026-09-09T14:03:19Z player:hagge (level 1) //set: 512 cell(s)",
	}) {
		t.Errorf("event 1 = %+v", es[1])
	}
}

func TestAuditLinesFromJournalPlain(t *testing.T) {
	// journalctl -n output: syslog prefix ahead of the marker, non-audit noise
	// interleaved. Only the two [Audit] lines come back, prefix stripped.
	es := AuditLines(golden(t, "audit_journal_plain.txt"))
	if len(es) != 2 {
		t.Fatalf("got %d events, want 2: %+v", len(es), es)
	}
	if es[0].Actor != "control" || es[0].Time != "2026-09-09T12:01:15Z" ||
		es[0].What != "kick Griefer @ 10.0.0.9 (spawn camping)" {
		t.Errorf("event 0 = %+v", es[0])
	}
	if es[0].Raw != "[Audit] 2026-09-09T12:01:15Z control kick Griefer @ 10.0.0.9 (spawn camping)" {
		t.Errorf("event 0 Raw not prefix-stripped: %q", es[0].Raw)
	}
}

func TestAuditLineUnparsed(t *testing.T) {
	// A marker with too few trailing fields keeps Raw, leaves fields blank.
	ev := AuditLine("[Audit] partial")
	if ev.Time != "" || ev.Actor != "" || ev.What != "" || ev.Raw != "[Audit] partial" {
		t.Errorf("partial line = %+v", ev)
	}
	// A line with no marker at all is kept whole in Raw.
	ev = AuditLine("just a log line")
	if ev.Raw != "just a log line" {
		t.Errorf("no-marker line = %+v", ev)
	}
}

func TestDuDirs(t *testing.T) {
	es, err := DuDirs(golden(t, "du_backups.txt"))
	if err != nil {
		t.Fatalf("DuDirs: %v", err)
	}
	if len(es) != 4 {
		t.Fatalf("got %d entries, want 4: %+v", len(es), es)
	}
	if es[0] != (DuEntry{KBytes: 1240, Path: "/var/lib/edenserver/backups/20260908T140000Z"}) {
		t.Errorf("entry 0 = %+v", es[0])
	}
	// The trailing total line is still returned; the caller drops it by path.
	if es[3].Path != "/var/lib/edenserver/backups" || es[3].KBytes != 3420 {
		t.Errorf("total line = %+v", es[3])
	}
}

func TestDuDirsSpaceSeparated(t *testing.T) {
	// Some du builds pad with spaces instead of a tab.
	es, err := DuDirs([]byte("512   /b/one\n1024  /b\n"))
	if err != nil || len(es) != 2 || es[0].KBytes != 512 || es[0].Path != "/b/one" {
		t.Fatalf("space-separated du: %+v %v", es, err)
	}
}

func TestDuDirsJunk(t *testing.T) {
	if _, err := DuDirs([]byte("not du output at all\n")); err == nil {
		t.Error("expected an error on unparseable du output")
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
