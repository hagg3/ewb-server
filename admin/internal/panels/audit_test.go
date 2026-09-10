package panels

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/hagg3/ewb-server/admin/internal/profile"
)

func anyContains(ss []string, sub string) bool {
	for _, s := range ss {
		if strings.Contains(s, sub) {
			return true
		}
	}
	return false
}

func TestAuditLocalFallbackToLog(t *testing.T) {
	p := localProfile(t)
	rt := rtWithFake(&fakeTransport{})

	// No log yet: an explicit note, not a blank panel, and no crash.
	res := rt.Audit(context.Background(), p)
	if res.Enabled || !res.Fallback || res.Note == "" || len(res.Events) != 0 {
		t.Fatalf("missing-log audit = %+v", res)
	}

	os.WriteFile(LogFilePath(p), []byte(
		"[Server] listening on 27099\n"+
			"[Control] say: hi\n"+
			"[Audit] 2026-09-09T14:03:19Z player:hagge //set: 512 cell(s)\n"+
			"[Audit] 2026-09-09T14:04:02Z control save\n"), 0o644)

	res = rt.Audit(context.Background(), p)
	if res.Enabled || !res.Fallback {
		t.Fatalf("want fallback, got %+v", res)
	}
	if len(res.Events) != 2 || res.Events[0].Actor != "player:hagge" || res.Events[1].What != "save" {
		t.Fatalf("events = %+v", res.Events)
	}
	if res.Note != "" {
		t.Errorf("unexpected note with events present: %q", res.Note)
	}
}

func TestAuditLocalDedicatedFile(t *testing.T) {
	dir := t.TempDir()
	auditPath := filepath.Join(dir, "audit.log")
	p := profile.Profile{
		Name: "dev", Kind: profile.Local, WorldDir: dir,
		Args: []string{"--name", "Dev", "--audit-file", auditPath},
	}
	rt := rtWithFake(&fakeTransport{})

	// Configured but not created yet: Enabled, an explanatory note, no events.
	res := rt.Audit(context.Background(), p)
	if !res.Enabled || res.Fallback || res.Source != auditPath || len(res.Events) != 0 || res.Note == "" {
		t.Fatalf("uncreated audit file = %+v", res)
	}

	os.WriteFile(auditPath, []byte("[Audit] 2026-09-09T14:05:02Z control stop requested\n"), 0o644)
	res = rt.Audit(context.Background(), p)
	if !res.Enabled || len(res.Events) != 1 || res.Events[0].What != "stop requested" {
		t.Fatalf("dedicated audit file = %+v", res)
	}
}

func TestAuditVPSJournalFallback(t *testing.T) {
	f := &fakeTransport{replies: []fakeReply{
		// No conf file => auditFileFromConf returns "".
		{match: "/etc/edenserver.conf", stderr: "cat: /etc/edenserver.conf: No such file or directory", exit: 1},
		{match: "journalctl", stdout: "" +
			"Sep 09 12:01:15 vps edenserver[1]: [Server] up\n" +
			"Sep 09 12:01:20 vps edenserver[1]: [Audit] 2026-09-09T12:01:20Z control ban 10.0.0.9\n"},
	}}
	rt := rtWithFake(f)

	res := rt.Audit(context.Background(), vpsProf)
	if res.Enabled || !res.Fallback {
		t.Fatalf("want journal fallback, got %+v", res)
	}
	if len(res.Events) != 1 || res.Events[0].What != "ban 10.0.0.9" {
		t.Fatalf("events = %+v", res.Events)
	}
}

func TestAuditVPSDedicatedFileFromConf(t *testing.T) {
	auditPath := "/var/log/edenserver/audit.log"
	f := &fakeTransport{replies: []fakeReply{
		{match: "/etc/edenserver.conf", stdout: "EDEN_PORT=27015\nEDEN_EXTRA_ARGS=--default-level 1 --audit-file " + auditPath + "\n"},
		{match: "tail", stdout: "[Audit] 2026-09-09T12:02:00Z player:alice //paint: 88 cell(s)\n"},
	}}
	rt := rtWithFake(f)

	res := rt.Audit(context.Background(), vpsProf)
	if !res.Enabled || res.Fallback || res.Source != auditPath {
		t.Fatalf("want dedicated audit file, got %+v", res)
	}
	if len(res.Events) != 1 || res.Events[0].Actor != "player:alice" {
		t.Fatalf("events = %+v", res.Events)
	}
	// The tail must run as the service user.
	if !anyContains(f.calls, "sudo -n -u edenserver tail") {
		t.Errorf("audit-file tail not sudo-wrapped: %v", f.calls)
	}
}

func TestAuditVPSSilentJournal(t *testing.T) {
	f := &fakeTransport{replies: []fakeReply{
		{match: "/etc/edenserver.conf", stderr: "No such file or directory", exit: 1},
		{match: "journalctl", stdout: "   \n"}, // exit 0 but empty — the journal-group gap
	}}
	rt := rtWithFake(f)

	res := rt.Audit(context.Background(), vpsProf)
	if len(res.Events) != 0 || !strings.Contains(res.Note, "systemd-journal") {
		t.Fatalf("silent journal should note the group gap: %+v", res)
	}
}
