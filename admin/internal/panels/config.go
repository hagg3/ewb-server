package panels

import (
	"context"
	"fmt"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/hagg3/ewb-server/admin/internal/eden"
	"github.com/hagg3/ewb-server/admin/internal/profile"
	"github.com/hagg3/ewb-server/admin/internal/transport"
)

// Config panel (roadmap 6.6). For a vps profile it reads and rewrites the
// systemd EnvironmentFile (/etc/edenserver.conf) through the validating
// ops/edenserver-writeconf wrapper; for a local profile it edits the managed
// flags in the profile's own `args`. Either way: a typed form, a line diff, an
// explicit confirm in the UI, then write + optional graceful restart.

// The managed EDEN_* keys. Every other line in the conf file is preserved
// verbatim on a write.
const (
	confPort     = "EDEN_PORT"
	confName     = "EDEN_NAME"
	confWorldDir = "EDEN_WORLD_DIR"
	confMaxCells = "EDEN_MAX_WORLD_CELLS"
	confPassword = "EDEN_PASSWORD"
	confExtra    = "EDEN_EXTRA_ARGS"
)

const (
	defaultPort     = 27015
	defaultMaxCells = 4000000
)

// ConfigSettings is the typed form. It is the same shape read and written; the
// browser round-trips it as JSON.
type ConfigSettings struct {
	Port          int    `json:"port"`
	Name          string `json:"name"`
	WorldDir      string `json:"world_dir"`
	MaxWorldCells int    `json:"max_world_cells"`
	Password      string `json:"password"`
	ExtraArgs     string `json:"extra_args"`
}

// ConfigResult is the Config panel payload.
type ConfigResult struct {
	Profile  string         `json:"profile"`
	Kind     string         `json:"kind"`
	Source   string         `json:"source"` // "/etc/edenserver.conf" | "profile args"
	Raw      string         `json:"raw"`
	Settings ConfigSettings `json:"settings"`
	Editable []string       `json:"editable"` // which fields a write on this transport touches
	Note     string         `json:"note,omitempty"`
}

// Config reads the current configuration for p.
func (rt *Runtime) Config(ctx context.Context, p profile.Profile) ConfigResult {
	switch p.Kind {
	case profile.Local:
		return rt.configLocal(p)
	case profile.VPS:
		return rt.configVPS(ctx, p)
	}
	return ConfigResult{Profile: p.Name, Kind: string(p.Kind), Note: "unknown profile kind"}
}

func (rt *Runtime) configLocal(p profile.Profile) ConfigResult {
	res := ConfigResult{
		Profile:  p.Name,
		Kind:     string(p.Kind),
		Source:   "profile args",
		Raw:      strings.Join(p.Args, " "),
		Settings: settingsFromArgs(p),
		Editable: []string{"port", "name", "max_world_cells"},
		Note: "a local profile is configured by its `args` — this form writes --port / --name / " +
			"--max-world-cells back to the profile. The active world is set in the Worlds panel; " +
			"extra args are shown read-only, edit them in the profile editor.",
	}
	return res
}

func (rt *Runtime) configVPS(ctx context.Context, p profile.Profile) ConfigResult {
	res := ConfigResult{
		Profile:  p.Name,
		Kind:     string(p.Kind),
		Source:   eden.ConfPath(p),
		Editable: []string{"port", "name", "world_dir", "max_world_cells", "password", "extra_args"},
	}
	body, note := rt.readConf(ctx, p)
	if note != "" {
		res.Note = note
	}
	res.Raw = string(body)
	c := ParseConf(body)
	res.Settings = settingsFromConf(c, p)
	if len(body) == 0 {
		res.Note = "no " + eden.ConfPath(p) + " yet — Save writes one from the values below " +
			"(copy ops/edenserver.conf.example first if you want the comments)."
	}
	return res
}

// readConf fetches the EnvironmentFile: plain `cat` first, `sudo -n cat` on
// failure. An empty body with no error means the file is absent.
func (rt *Runtime) readConf(ctx context.Context, p profile.Profile) ([]byte, string) {
	t, err := rt.mkTransport(p)
	if err != nil {
		return nil, err.Error()
	}
	r, err := t.Run(ctx, transport.Command{Argv: eden.ConfRead(p), Timeout: 15 * time.Second})
	if err == nil && r.ExitCode == 0 {
		return r.Stdout, ""
	}
	rs, err := t.Run(ctx, transport.Command{Argv: eden.ConfReadSudo(p), Timeout: 15 * time.Second})
	if err != nil {
		return nil, "could not read " + eden.ConfPath(p) + ": " + err.Error()
	}
	if rs.ExitCode != 0 {
		msg := strings.TrimSpace(string(rs.Stderr))
		if strings.Contains(msg, "No such file") {
			return nil, ""
		}
		if transport.SudoDenied(rs.Stderr) {
			return nil, "sudo -n cat " + eden.ConfPath(p) + " was denied — add the sudoers rule (ops/sudoers.d/edenadmin)"
		}
		return nil, orElse(msg, "could not read "+eden.ConfPath(p))
	}
	return rs.Stdout, ""
}

// --- write --------------------------------------------------------------

// ConfigWriteResult is the outcome of a Save.
type ConfigWriteResult struct {
	OK        bool     `json:"ok"`
	Message   string   `json:"message"`
	Diff      []string `json:"diff"`
	Applied   bool     `json:"applied"`
	Restarted bool     `json:"restarted"`
	Command   string   `json:"command,omitempty"`
}

// ConfigWriteVPS validates s, rewrites the EnvironmentFile through
// edenserver-writeconf, and (when restart) does a graceful stop + start.
func (rt *Runtime) ConfigWriteVPS(ctx context.Context, p profile.Profile, s ConfigSettings, restart bool) ConfigWriteResult {
	var res ConfigWriteResult
	if err := ValidateConfigSettings(s); err != nil {
		res.Message = err.Error()
		return res
	}
	cur, note := rt.readConf(ctx, p)
	if note != "" && len(cur) == 0 && !strings.Contains(note, "no ") {
		res.Message = note
		return res
	}
	c := ParseConf(cur)
	if len(c.lines) == 0 {
		c = ParseConf(defaultConfTemplate())
	}
	applyConfSettings(c, s)
	next := c.Render()
	res.Diff = confDiff(cur, next)

	t, err := rt.mkTransport(p)
	if err != nil {
		res.Message = err.Error()
		return res
	}
	argv := eden.ConfWrite(p)
	res.Command = t.Describe(transport.Command{Argv: argv})
	r, err := t.Run(ctx, transport.Command{Argv: argv, Stdin: next, Timeout: 20 * time.Second})
	if err != nil {
		res.Message = "writeconf failed: " + err.Error()
		return res
	}
	if r.ExitCode != 0 {
		if transport.SudoDenied(r.Stderr) {
			res.Message = "sudo -n edenserver-writeconf was denied — add the sudoers rule (ops/sudoers.d/edenadmin)"
			return res
		}
		res.Message = orElse(strings.TrimSpace(string(r.Stderr)), fmt.Sprintf("edenserver-writeconf exited %d", r.ExitCode))
		return res
	}
	res.Applied = true
	res.OK = true
	res.Message = orElse(firstLine(string(r.Stdout)), "wrote "+eden.ConfPath(p))

	if restart {
		pr := rt.Power(ctx, p, "restart")
		res.Restarted = pr.OK
		res.Message += " — restart: " + pr.Message
		if !pr.OK {
			res.OK = false
		}
	}
	return res
}

// SetConfWorldDir points EDEN_WORLD_DIR at dir (and, when maxCells > 0, raises
// EDEN_MAX_WORLD_CELLS to match an import), then writes and restarts. It is the
// vps set-active path the Worlds panel calls (roadmap 6.4's deferred item).
func (rt *Runtime) SetConfWorldDir(ctx context.Context, p profile.Profile, dir string, maxCells int, restart bool) ConfigWriteResult {
	cur := rt.configVPS(ctx, p)
	s := cur.Settings
	s.WorldDir = dir
	if maxCells > s.MaxWorldCells {
		s.MaxWorldCells = maxCells
	}
	return rt.ConfigWriteVPS(ctx, p, s, restart)
}

// --- projections -------------------------------------------------------

func settingsFromConf(c *ConfFile, p profile.Profile) ConfigSettings {
	s := ConfigSettings{Port: defaultPort, MaxWorldCells: defaultMaxCells, WorldDir: p.WorldDir}
	if v, ok := c.GetInt(confPort); ok {
		s.Port = v
	}
	if v, ok := c.Get(confName); ok {
		s.Name = v
	}
	if v, ok := c.Get(confWorldDir); ok && v != "" {
		s.WorldDir = v
	}
	if v, ok := c.GetInt(confMaxCells); ok {
		s.MaxWorldCells = v
	}
	if v, ok := c.Get(confPassword); ok {
		s.Password = v
	}
	if v, ok := c.Get(confExtra); ok {
		s.ExtraArgs = v
	}
	return s
}

func settingsFromArgs(p profile.Profile) ConfigSettings {
	wa := parseWorldArgs(p.Args)
	s := ConfigSettings{
		Port:          wa.Port,
		Name:          wa.Name,
		MaxWorldCells: defaultMaxCells,
		WorldDir:      activeWorldDirFor(p),
		Password:      flagValue(p.Args, "--password"),
	}
	if v := flagValue(p.Args, "--max-world-cells"); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			s.MaxWorldCells = n
		}
	}
	s.ExtraArgs = strings.Join(argsWithoutManaged(p.Args), " ")
	return s
}

// ArgsWithConfigSettings rewrites the managed flags (--port, --name,
// --max-world-cells) in args from s, leaving everything else untouched. Used by
// the httpd layer for a local-profile Save (which then persists the profile).
func ArgsWithConfigSettings(args []string, s ConfigSettings) []string {
	out := setFlag(args, "--port", strconv.Itoa(s.Port))
	if strings.TrimSpace(s.Name) != "" {
		out = setFlag(out, "--name", s.Name)
	}
	if s.MaxWorldCells > 0 && s.MaxWorldCells != defaultMaxCells {
		out = setFlag(out, "--max-world-cells", strconv.Itoa(s.MaxWorldCells))
	} else {
		out = removeFlag(out, "--max-world-cells")
	}
	return out
}

// --- helpers ----------------------------------------------------------

func applyConfSettings(c *ConfFile, s ConfigSettings) {
	c.Set(confPort, strconv.Itoa(s.Port))
	c.Set(confName, s.Name)
	c.Set(confWorldDir, s.WorldDir)
	c.Set(confMaxCells, strconv.Itoa(s.MaxWorldCells))
	c.Set(confPassword, s.Password)
	// The empty-optional rule: EDEN_EXTRA_ARGS is a bare $VAR tail, so an empty
	// value is omitted from the file entirely rather than written as `=`.
	if strings.TrimSpace(s.ExtraArgs) == "" {
		c.Unset(confExtra)
	} else {
		c.Set(confExtra, strings.Join(strings.Fields(s.ExtraArgs), " "))
	}
}

// ValidateConfigSettings rejects a form that would produce a broken conf file or
// a broken ExecStart. Shared by the vps and local write paths.
func ValidateConfigSettings(s ConfigSettings) error {
	if s.Port < 1 || s.Port > 65535 {
		return fmt.Errorf("port %d is out of range 1..65535", s.Port)
	}
	if strings.TrimSpace(s.Name) == "" {
		return fmt.Errorf("server name must not be empty")
	}
	if hasControlChar(s.Name) {
		return fmt.Errorf("server name has a newline")
	}
	if !filepath.IsAbs(s.WorldDir) {
		return fmt.Errorf("world dir %q must be an absolute path", s.WorldDir)
	}
	if hasControlChar(s.WorldDir) || strings.ContainsAny(s.WorldDir, " \t") {
		return fmt.Errorf("world dir %q must not contain whitespace", s.WorldDir)
	}
	if s.MaxWorldCells < 1 {
		return fmt.Errorf("max world cells must be >= 1")
	}
	if hasControlChar(s.Password) {
		return fmt.Errorf("password has a newline — the conf line is newline-framed, so this would corrupt the file")
	}
	if hasControlChar(s.ExtraArgs) {
		return fmt.Errorf("extra args have a newline")
	}
	return nil
}

// ValidateLocalConfigSettings checks only the fields a local-profile write
// touches (--port, --name, --max-world-cells) — world_dir / password / extra
// args are read-only in that form.
func ValidateLocalConfigSettings(s ConfigSettings) error {
	if s.Port < 1 || s.Port > 65535 {
		return fmt.Errorf("port %d is out of range 1..65535", s.Port)
	}
	if hasControlChar(s.Name) {
		return fmt.Errorf("server name has a newline")
	}
	if s.MaxWorldCells < 0 {
		return fmt.Errorf("max world cells must not be negative")
	}
	return nil
}

func flagValue(args []string, flag string) string {
	for i := 0; i < len(args)-1; i++ {
		if args[i] == flag {
			return args[i+1]
		}
	}
	return ""
}

// argsWithoutManaged drops --port/--name/--world/--signs/--max-world-cells (and
// their values) so what's left is the "extra args" the local Config form shows
// read-only.
func argsWithoutManaged(args []string) []string {
	managed := map[string]bool{
		"--port": true, "--name": true, "--world": true,
		"--signs": true, "--max-world-cells": true,
	}
	out := make([]string, 0, len(args))
	for i := 0; i < len(args); i++ {
		if managed[args[i]] {
			i++ // skip the value
			continue
		}
		out = append(out, args[i])
	}
	return out
}

func defaultConfTemplate() []byte {
	return []byte(strings.Join([]string{
		"# /etc/edenserver.conf — written by edenadmin. See ops/edenserver.conf.example",
		"# for the ${VAR} vs $VAR notes.",
		confPort + "=" + strconv.Itoa(defaultPort),
		confName + "=Eden Server",
		confWorldDir + "=/var/lib/edenserver/world",
		confMaxCells + "=" + strconv.Itoa(defaultMaxCells),
		confPassword + "=",
	}, "\n") + "\n")
}
