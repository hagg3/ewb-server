package panels

import (
	"regexp"
	"strconv"
	"strings"
)

// conffile.go — a minimal reader/writer for /etc/edenserver.conf: `KEY=value`
// lines with `#` comments and blank lines preserved, so a rewrite is a minimal
// diff an operator can eyeball in the Config panel. It is not a shell parser —
// values are taken literally to end of line (systemd's EnvironmentFile does the
// same for an unquoted value), and only `EDEN_*` keys are treated as settings.

var confKeyRE = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]*=`)

type confLine struct {
	raw string // verbatim original, used for passthrough of comments / unknown lines
	key string // "" for a comment or blank line
	val string
}

// ConfFile is a parsed EnvironmentFile.
type ConfFile struct {
	lines []confLine
}

// ParseConf reads b. It never errors: a line it does not recognise as
// `KEY=value` is kept verbatim and ignored as a setting.
func ParseConf(b []byte) *ConfFile {
	c := &ConfFile{}
	s := strings.ReplaceAll(string(b), "\r\n", "\n")
	s = strings.TrimSuffix(s, "\n")
	if s == "" {
		return c
	}
	for _, raw := range strings.Split(s, "\n") {
		t := strings.TrimSpace(raw)
		if t == "" || strings.HasPrefix(t, "#") || !confKeyRE.MatchString(t) {
			c.lines = append(c.lines, confLine{raw: raw})
			continue
		}
		k, v, _ := strings.Cut(t, "=")
		c.lines = append(c.lines, confLine{raw: raw, key: k, val: v})
	}
	return c
}

// Get returns the last value set for key (systemd's EnvironmentFile semantics:
// a later line wins).
func (c *ConfFile) Get(key string) (string, bool) {
	v, ok := "", false
	for _, l := range c.lines {
		if l.key == key {
			v, ok = l.val, true
		}
	}
	return v, ok
}

// GetInt is Get parsed as an int; missing / unparseable => (0, false).
func (c *ConfFile) GetInt(key string) (int, bool) {
	if v, ok := c.Get(key); ok {
		if n, err := strconv.Atoi(strings.TrimSpace(v)); err == nil {
			return n, true
		}
	}
	return 0, false
}

// Set updates the first line for key in place (and drops any later duplicates),
// or appends `key=val` if the key is absent.
func (c *ConfFile) Set(key, val string) {
	seen := false
	out := c.lines[:0]
	for _, l := range c.lines {
		if l.key == key {
			if seen {
				continue // collapse duplicates
			}
			seen = true
			l.raw = key + "=" + val
			l.val = val
		}
		out = append(out, l)
	}
	c.lines = out
	if !seen {
		c.lines = append(c.lines, confLine{raw: key + "=" + val, key: key, val: val})
	}
}

// Unset removes every line for key.
func (c *ConfFile) Unset(key string) {
	out := c.lines[:0]
	for _, l := range c.lines {
		if l.key == key {
			continue
		}
		out = append(out, l)
	}
	c.lines = out
}

// Render serialises back to bytes with a trailing newline.
func (c *ConfFile) Render() []byte {
	if len(c.lines) == 0 {
		return nil
	}
	var b strings.Builder
	for _, l := range c.lines {
		b.WriteString(l.raw)
		b.WriteByte('\n')
	}
	return []byte(b.String())
}

// confDiff is a per-line +/- of two rendered conf blobs. Order-sensitive and
// intentionally dumb — the Config panel shows it verbatim as the confirm gate.
func confDiff(oldB, newB []byte) []string {
	oldL := splitKeep(string(oldB))
	newL := splitKeep(string(newB))
	oldSet := map[string]int{}
	for _, l := range oldL {
		oldSet[l]++
	}
	newSet := map[string]int{}
	for _, l := range newL {
		newSet[l]++
	}
	var diff []string
	for _, l := range oldL {
		if newSet[l] == 0 && strings.TrimSpace(l) != "" {
			diff = append(diff, "- "+l)
		}
	}
	for _, l := range newL {
		if oldSet[l] == 0 && strings.TrimSpace(l) != "" {
			diff = append(diff, "+ "+l)
		}
	}
	return diff
}

func splitKeep(s string) []string {
	s = strings.ReplaceAll(s, "\r\n", "\n")
	s = strings.TrimSuffix(s, "\n")
	if s == "" {
		return nil
	}
	return strings.Split(s, "\n")
}
