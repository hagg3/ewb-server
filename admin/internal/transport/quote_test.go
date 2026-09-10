package transport

import (
	"os/exec"
	"strings"
	"testing"
)

func TestShq(t *testing.T) {
	cases := []struct {
		in, want string
	}{
		{"", "''"},
		{"plain", "plain"},
		{"with space", "'with space'"},
		{"a/b/c.sock", "a/b/c.sock"},
		{"--flag=value", "--flag=value"},
		{"127.0.0.1:27015", "127.0.0.1:27015"},
		{"it's", `'it'\''s'`},
		{"'", `''\'''`},
		{`"quoted"`, `'"quoted"'`},
		{"$HOME", `'$HOME'`},
		{"`cmd`", "'`cmd`'"},
		{"a\nb", "'a\nb'"},
		{"semi;colon", "'semi;colon'"},
		{"pipe|pipe", "'pipe|pipe'"},
		{"utf8 — café", "'utf8 — café'"},
		{"s3;j=abc=;x", "'s3;j=abc=;x'"}, // a journalctl cursor shape
	}
	for _, c := range cases {
		if got := Shq(c.in); got != c.want {
			t.Errorf("Shq(%q) = %q, want %q", c.in, got, c.want)
		}
	}
}

// TestShqRoundTripsThroughSh confirms that what Shq emits, /bin/sh parses back
// into exactly the original argument. This is the property that matters — the
// exact escaping is an implementation detail, byte-fidelity through the shell is
// the contract.
func TestShqRoundTripsThroughSh(t *testing.T) {
	inputs := []string{
		"", "plain", "with space", "it's", "'", `"q"`, "$HOME", "`cmd`",
		"tab\there", "semi;colon", "a|b", "new\nline", "utf8 — café",
		"lots '\" $ ` \\ of it", "--max-region-records", "65536:33:65540",
	}
	for _, in := range inputs {
		// printf %s so no trailing newline is added; read it back verbatim.
		script := "printf %s " + Shq(in)
		out, err := exec.Command("/bin/sh", "-c", script).Output()
		if err != nil {
			t.Fatalf("sh -c %q: %v", script, err)
		}
		if string(out) != in {
			t.Errorf("round trip of %q through sh gave %q", in, string(out))
		}
	}
}

func TestShqJoin(t *testing.T) {
	got := ShqJoin([]string{"edenctl", "-S", "/w/edenserver.sock", "say", "hello world"})
	want := "edenctl -S /w/edenserver.sock say 'hello world'"
	if got != want {
		t.Errorf("ShqJoin = %q, want %q", got, want)
	}
}

func TestShqJoinEmptyArg(t *testing.T) {
	// A deliberately-empty argument must survive as '' rather than vanish.
	got := ShqJoin([]string{"x", "", "y"})
	if !strings.Contains(got, " '' ") {
		t.Errorf("ShqJoin dropped an empty arg: %q", got)
	}
}
