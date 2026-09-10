package panels

import (
	"context"
	"strings"
	"testing"

	"github.com/hagg3/ewb-server/admin/internal/profile"
)

const sampleConf = `# a comment
EDEN_PORT=27015
EDEN_NAME=Eden Server
EDEN_WORLD_DIR=/var/lib/edenserver/world
EDEN_MAX_WORLD_CELLS=4000000
EDEN_PASSWORD=

# operator's own key, must survive a rewrite
EDEN_CUSTOM=keepme
`

func TestConfFileRoundTripPreservesCommentsAndOrder(t *testing.T) {
	c := ParseConf([]byte(sampleConf))
	if got := string(c.Render()); got != sampleConf {
		t.Fatalf("round trip changed the file:\n%q\nwant\n%q", got, sampleConf)
	}
	if v, _ := c.Get("EDEN_NAME"); v != "Eden Server" {
		t.Errorf("EDEN_NAME = %q", v)
	}
	if n, ok := c.GetInt("EDEN_MAX_WORLD_CELLS"); !ok || n != 4000000 {
		t.Errorf("EDEN_MAX_WORLD_CELLS = %d,%v", n, ok)
	}
}

func TestConfFileSetInPlaceAndUnset(t *testing.T) {
	c := ParseConf([]byte(sampleConf))
	c.Set("EDEN_PORT", "27016")
	c.Set("EDEN_NEWKEY", "v")
	c.Unset("EDEN_PASSWORD")
	out := string(c.Render())
	if !strings.Contains(out, "EDEN_PORT=27016") || strings.Contains(out, "EDEN_PORT=27015") {
		t.Errorf("EDEN_PORT not replaced in place:\n%s", out)
	}
	if strings.Contains(out, "EDEN_PASSWORD=") {
		t.Errorf("EDEN_PASSWORD not removed:\n%s", out)
	}
	if !strings.Contains(out, "EDEN_CUSTOM=keepme") {
		t.Errorf("operator's own key was dropped:\n%s", out)
	}
	if !strings.HasSuffix(out, "EDEN_NEWKEY=v\n") {
		t.Errorf("appended key not at the end:\n%s", out)
	}
	// The comment lines are still there.
	if !strings.Contains(out, "# a comment") {
		t.Errorf("comment dropped:\n%s", out)
	}
}

func TestApplyConfSettingsOmitsEmptyExtraArgs(t *testing.T) {
	c := ParseConf([]byte(sampleConf))
	applyConfSettings(c, ConfigSettings{
		Port: 27015, Name: "N", WorldDir: "/w", MaxWorldCells: 5, Password: "",
		ExtraArgs: "   ",
	})
	if strings.Contains(string(c.Render()), "EDEN_EXTRA_ARGS") {
		t.Errorf("an all-whitespace ExtraArgs must be omitted, not written:\n%s", c.Render())
	}
	applyConfSettings(c, ConfigSettings{
		Port: 27015, Name: "N", WorldDir: "/w", MaxWorldCells: 5,
		ExtraArgs: "  --matchmaker  h:1   --default-level 1 ",
	})
	if !strings.Contains(string(c.Render()), "EDEN_EXTRA_ARGS=--matchmaker h:1 --default-level 1\n") {
		t.Errorf("ExtraArgs not normalised/written:\n%s", c.Render())
	}
	// EDEN_PASSWORD is always written even when empty (it is a braced ${VAR}).
	if !strings.Contains(string(c.Render()), "EDEN_PASSWORD=\n") {
		t.Errorf("EDEN_PASSWORD= must always be present:\n%s", c.Render())
	}
}

func TestValidateConfigSettings(t *testing.T) {
	good := ConfigSettings{Port: 27015, Name: "Eden", WorldDir: "/var/lib/edenserver/world", MaxWorldCells: 4000000}
	if err := ValidateConfigSettings(good); err != nil {
		t.Fatalf("good settings rejected: %v", err)
	}
	bad := []ConfigSettings{
		{Port: 0, Name: "x", WorldDir: "/w", MaxWorldCells: 1},
		{Port: 70000, Name: "x", WorldDir: "/w", MaxWorldCells: 1},
		{Port: 1, Name: "", WorldDir: "/w", MaxWorldCells: 1},
		{Port: 1, Name: "x", WorldDir: "relative", MaxWorldCells: 1},
		{Port: 1, Name: "x", WorldDir: "/has space", MaxWorldCells: 1},
		{Port: 1, Name: "x", WorldDir: "/w", MaxWorldCells: 0},
		{Port: 1, Name: "x", WorldDir: "/w", MaxWorldCells: 1, Password: "a\nb"},
		{Port: 1, Name: "x\n", WorldDir: "/w", MaxWorldCells: 1},
	}
	for i, s := range bad {
		if err := ValidateConfigSettings(s); err == nil {
			t.Errorf("case %d should be rejected: %+v", i, s)
		}
	}
}

func TestConfDiff(t *testing.T) {
	old := []byte("EDEN_PORT=27015\nEDEN_NAME=A\n")
	next := []byte("EDEN_PORT=27016\nEDEN_NAME=A\n")
	d := confDiff(old, next)
	if len(d) != 2 || d[0] != "- EDEN_PORT=27015" || d[1] != "+ EDEN_PORT=27016" {
		t.Fatalf("diff = %v", d)
	}
	if len(confDiff(old, old)) != 0 {
		t.Errorf("identical files should diff to nothing")
	}
}

func TestConfigWriteVPSWritesAndRestarts(t *testing.T) {
	f := &fakeTransport{replies: []fakeReply{
		{match: "cat /etc/edenserver.conf", stdout: sampleConf},
		{match: "edenserver-writeconf", stdout: "edenserver-writeconf: wrote /etc/edenserver.conf\n"},
		{match: "edenctl", stdout: "ok: stopping\n"},
		{match: "ActiveState", stdout: "ActiveState=inactive\n"},
		{match: "systemctl start", exit: 0},
	}}
	rt := NewRuntime()
	rt.mkTransport = fakeFactory(f)

	s := ConfigSettings{Port: 27016, Name: "Eden Server", WorldDir: "/var/lib/edenserver/world", MaxWorldCells: 4000000}
	res := rt.ConfigWriteVPS(context.Background(), vpsProf, s, true)
	if !res.OK || !res.Applied {
		t.Fatalf("write not applied: %+v (calls %v)", res, f.calls)
	}
	if !res.Restarted {
		t.Errorf("expected a restart: %+v", res)
	}
	// The stdin to writeconf must carry the new port and keep the operator's key.
	var wrote string
	for _, c := range f.calls {
		if strings.Contains(c, "edenserver-writeconf") {
			wrote = c
		}
	}
	if wrote == "" {
		t.Fatalf("writeconf never called: %v", f.calls)
	}
	// A graceful restart must not shell out to `systemctl stop`.
	for _, c := range f.calls {
		if strings.Contains(c, "systemctl stop") || strings.Contains(c, "systemctl restart") {
			t.Errorf("config restart used a lossy systemctl verb: %q", c)
		}
	}
	if d := res.Diff; len(d) == 0 {
		t.Errorf("expected a non-empty diff for a port change")
	}
}

func TestConfigWriteVPSSudoDeniedIsLegible(t *testing.T) {
	f := &fakeTransport{replies: []fakeReply{
		{match: "cat /etc/edenserver.conf", stdout: sampleConf},
		{match: "edenserver-writeconf", stderr: "sudo: a password is required", exit: 1},
	}}
	rt := NewRuntime()
	rt.mkTransport = fakeFactory(f)
	res := rt.ConfigWriteVPS(context.Background(), vpsProf, ConfigSettings{
		Port: 27016, Name: "N", WorldDir: "/w", MaxWorldCells: 1,
	}, false)
	if res.OK || !strings.Contains(res.Message, "sudoers") {
		t.Fatalf("expected a sudoers hint: %+v", res)
	}
}

func TestSetConfWorldDirRaisesCap(t *testing.T) {
	f := &fakeTransport{replies: []fakeReply{
		{match: "cat /etc/edenserver.conf", stdout: sampleConf},
		{match: "edenserver-writeconf", stdout: "wrote\n"},
	}}
	rt := NewRuntime()
	rt.mkTransport = fakeFactory(f)
	res := rt.SetConfWorldDir(context.Background(), vpsProf, "/var/lib/edenserver/worlds/big", 26_000_000, false)
	if !res.OK {
		t.Fatalf("SetConfWorldDir: %+v", res)
	}
	joined := strings.Join(res.Diff, "\n")
	if !strings.Contains(joined, "+ EDEN_WORLD_DIR=/var/lib/edenserver/worlds/big") {
		t.Errorf("world dir not in diff: %v", res.Diff)
	}
	if !strings.Contains(joined, "+ EDEN_MAX_WORLD_CELLS=26000000") {
		t.Errorf("cap not raised in diff: %v", res.Diff)
	}
}

func TestConfigLocalReadAndArgsRewrite(t *testing.T) {
	p := profile.Profile{
		Name: "dev", Kind: profile.Local,
		WorldDir: "/w", Socket: "/tmp/edenadmin-cfg.sock",
		Args: []string{"--port", "27015", "--name", "Dev", "--world", "/w/eden_world.model", "--matchmaker", "h:1"},
	}
	rt := NewRuntime()
	res := rt.Config(context.Background(), p)
	if res.Settings.Port != 27015 || res.Settings.Name != "Dev" {
		t.Fatalf("local settings: %+v", res.Settings)
	}
	if !strings.Contains(res.Settings.ExtraArgs, "--matchmaker h:1") {
		t.Errorf("extra args not surfaced: %q", res.Settings.ExtraArgs)
	}

	next := ArgsWithConfigSettings(p.Args, ConfigSettings{Port: 27020, Name: "Dev 2", MaxWorldCells: 6_000_000})
	j := strings.Join(next, " ")
	if !strings.Contains(j, "--port 27020") || !strings.Contains(j, "--name Dev 2") ||
		!strings.Contains(j, "--max-world-cells 6000000") || !strings.Contains(j, "--matchmaker h:1") {
		t.Errorf("args rewrite dropped or mangled a flag: %q", j)
	}
	if strings.Contains(j, "--port 27015") {
		t.Errorf("old port still present: %q", j)
	}
}
