package panels

import (
	"context"
	"os"
	"strconv"
	"testing"

	"github.com/hagg3/ewb-server/admin/internal/profile"
	"github.com/hagg3/ewb-server/admin/internal/supervise"
)

func TestParseWorldArgs(t *testing.T) {
	cases := []struct {
		argv []string
		want WorldArgs
	}{
		{nil, WorldArgs{Port: 27015}},
		{[]string{"27099"}, WorldArgs{Port: 27099}},
		{[]string{"--port", "27016", "--name", "My World", "--world", "w/eden_world.model"},
			WorldArgs{Port: 27016, Name: "My World", World: "w/eden_world.model"}},
		// A flag value that is a bare number must not be read as the port.
		{[]string{"--default-level", "2", "--port", "30000"}, WorldArgs{Port: 30000}},
		{[]string{"--max-world-cells", "6000000"}, WorldArgs{Port: 27015}},
	}
	for _, c := range cases {
		if got := parseWorldArgs(c.argv); got != c.want {
			t.Errorf("parseWorldArgs(%v) = %+v, want %+v", c.argv, got, c.want)
		}
	}
}

func TestLooksLikeIP(t *testing.T) {
	yes := []string{"10", "10.0.0.9", "203.0.113.9", "::1"}
	no := []string{"", "Griefer", "Player10", "10.a"}
	for _, s := range yes {
		if !looksLikeIP(s) {
			t.Errorf("looksLikeIP(%q) = false, want true", s)
		}
	}
	for _, s := range no {
		if looksLikeIP(s) {
			t.Errorf("looksLikeIP(%q) = true, want false", s)
		}
	}
}

func localProfile(t *testing.T) profile.Profile {
	t.Helper()
	dir := t.TempDir()
	return profile.Profile{
		Name:      "dev",
		Kind:      profile.Local,
		WorldDir:  dir,
		ServerBin: "/bin/sleep",
		Args:      []string{"27099", "--name", "Dev World"},
	}
}

// rtWithFake returns a Runtime whose ctl calls hit a scripted fake and whose
// supervisor reports "running" without a real process.
func rtWithFake(f *fakeTransport) *Runtime {
	rt := NewRuntime()
	rt.mkTransport = fakeFactory(f)
	return rt
}

func TestPlayersReadOnly(t *testing.T) {
	p := localProfile(t)
	f := &fakeTransport{replies: []fakeReply{
		{match: "who", stdout: "2 player(s):\n" +
			"  Alice (T0) 127.0.0.1  @ 65540,34,65530  level 2\n" +
			"  10 (T1) 127.0.0.1  @ 65500,40,65600  level 0\n"},
	}}
	rt := rtWithFake(f)
	// Force the supervisor to look "running" by pointing its pidfile at ourselves.
	sup := rt.Supervisor(p)
	writeSelfPidfile(t, sup)

	res := rt.Players(context.Background(), p)
	if !res.Online || len(res.Players) != 2 {
		t.Fatalf("players = %+v", res)
	}
	if res.Players[0].Name != "Alice" || res.Players[0].NameLooksLikeIP {
		t.Errorf("row 0 = %+v", res.Players[0])
	}
	// "10" is an IP-shaped username — the panel must flag it.
	if !res.Players[1].NameLooksLikeIP {
		t.Errorf("row 1 should be flagged IP-shaped: %+v", res.Players[1])
	}
}

func TestPlayersServerDown(t *testing.T) {
	p := localProfile(t)
	rt := rtWithFake(&fakeTransport{})
	res := rt.Players(context.Background(), p)
	if res.Online || res.Note == "" {
		t.Errorf("expected an offline note, got %+v", res)
	}
}

func TestStatusServerDown(t *testing.T) {
	p := localProfile(t)
	rt := rtWithFake(&fakeTransport{})
	res := rt.Status(context.Background(), p)
	if res.Running {
		t.Error("status says running with no process")
	}
	if res.World.Port != 27099 || res.World.Name != "Dev World" {
		t.Errorf("world args = %+v", res.World)
	}
	if res.Disk == nil || res.Disk.TotalBytes == 0 {
		t.Errorf("disk not populated: %+v", res.Disk)
	}
}

func TestStatusRunningReadsWho(t *testing.T) {
	p := localProfile(t)
	f := &fakeTransport{replies: []fakeReply{
		{match: "who", stdout: "1 player(s):\n  Zed (T2) 127.0.0.1  @ 1,2,3  level 0\n"},
		{match: "region-stats", stdout: "REGION service since start:\n  requests served : 4\n"},
	}}
	rt := rtWithFake(f)
	writeSelfPidfile(t, rt.Supervisor(p))

	res := rt.Status(context.Background(), p)
	if !res.Running || res.Players != 1 {
		t.Fatalf("status = %+v", res)
	}
	if res.Region == nil || res.Region.RequestsServed != 4 {
		t.Errorf("region stats not read: %+v", res.Region)
	}
}

func TestLogs(t *testing.T) {
	p := localProfile(t)
	rt := rtWithFake(&fakeTransport{})
	path := LogFilePath(p)

	// Missing file: not an error, Missing flag set.
	r, err := rt.Logs(p, false)
	if err != nil || !r.Missing {
		t.Fatalf("missing logs = %+v, %v", r, err)
	}

	os.WriteFile(path, []byte("[Server] listening on 27099\n[Control] say: hi\nplain line\n"), 0o644)
	r, err = rt.Logs(p, true)
	if err != nil {
		t.Fatal(err)
	}
	if len(r.Lines) != 3 {
		t.Fatalf("lines = %+v", r.Lines)
	}
	if r.Lines[0].Tag != "Server" || r.Lines[1].Tag != "Control" || r.Lines[2].Tag != "" {
		t.Errorf("tags = %+v", r.Lines)
	}

	// Second poll with no new bytes returns nothing.
	r, _ = rt.Logs(p, false)
	if len(r.Lines) != 0 {
		t.Errorf("expected no new lines, got %+v", r.Lines)
	}
}

func writeSelfPidfile(t *testing.T, sup *supervise.Supervisor) {
	t.Helper()
	// Point the supervisor at this test process (alive) with a blank signature
	// line, so Status() skips the pid-reuse check and reports Running. Enough to
	// exercise the read-only panels without a real edenserver.
	line := strconv.Itoa(os.Getpid()) + "\n\n"
	if err := os.WriteFile(sup.PidFile, []byte(line), 0o644); err != nil {
		t.Fatal(err)
	}
}
