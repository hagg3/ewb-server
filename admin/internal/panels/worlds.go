package panels

import (
	"archive/tar"
	"bytes"
	"context"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/hagg3/ewb-server/admin/internal/eden"
	"github.com/hagg3/ewb-server/admin/internal/parse"
	"github.com/hagg3/ewb-server/admin/internal/profile"
	"github.com/hagg3/ewb-server/admin/internal/transport"
)

// worldNameRE is the allowed shape of a world-directory name — a single path
// component, no separators, no leading dot. eden_import slugs world names to
// this same alphabet.
var worldNameRE = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]*$`)

// ValidWorldName reports whether name is safe to use as a `worlds/<name>`
// directory component (no traversal, no separator, non-empty).
func ValidWorldName(name string) bool {
	return len(name) <= 64 && worldNameRE.MatchString(name) && name != "." && name != ".."
}

// --- list ------------------------------------------------------------------

// WorldEntry is one hostable world directory.
type WorldEntry struct {
	Name     string `json:"name"`
	Dir      string `json:"dir"`
	Blocks   int64  `json:"blocks"` // lines of eden_world.model, -1 if unknown
	Active   bool   `json:"active"`
	HasSigns bool   `json:"has_signs"`
	HasSpawn bool   `json:"has_spawn"`
}

// WorldsResult is the Worlds panel payload.
type WorldsResult struct {
	Profile string       `json:"profile"`
	Kind    string       `json:"kind"`
	Root    string       `json:"root"`
	Worlds  []WorldEntry `json:"worlds"`
	Note    string       `json:"note,omitempty"`
}

// Worlds lists the hostable worlds under the profile's world_root. A local
// profile reads the directory in Go; a vps profile runs `find` + `wc -l` over
// ssh. With no world_root set, the single configured world_dir is the only
// entry.
func (rt *Runtime) Worlds(ctx context.Context, p profile.Profile) WorldsResult {
	res := WorldsResult{Profile: p.Name, Kind: string(p.Kind), Root: p.WorldRoot}
	switch p.Kind {
	case profile.Local:
		rt.worldsLocal(&res, p)
	case profile.VPS:
		rt.worldsVPS(ctx, &res, p)
	default:
		res.Note = "unknown profile kind"
	}
	return res
}

func (rt *Runtime) worldsLocal(res *WorldsResult, p profile.Profile) {
	active := activeWorldDir(p)
	add := func(dir string) {
		model := filepath.Join(dir, "eden_world.model")
		if !fileExists(model) {
			return
		}
		res.Worlds = append(res.Worlds, WorldEntry{
			Name:     filepath.Base(dir),
			Dir:      dir,
			Blocks:   countLines(model),
			Active:   filepath.Clean(dir) == active,
			HasSigns: fileExists(filepath.Join(dir, "eden_signs.txt")),
			HasSpawn: fileExists(filepath.Join(dir, "eden_spawn.txt")),
		})
	}
	if p.WorldRoot == "" {
		if p.WorldDir != "" {
			add(p.WorldDir)
		} else {
			res.Note = "this profile has no world_root or world_dir"
		}
		return
	}
	ents, err := os.ReadDir(p.WorldRoot)
	if err != nil {
		res.Note = "cannot read world_root: " + err.Error()
		return
	}
	for _, e := range ents {
		if e.IsDir() {
			add(filepath.Join(p.WorldRoot, e.Name()))
		}
	}
	if len(res.Worlds) == 0 {
		res.Note = "no worlds/<name>/eden_world.model under " + p.WorldRoot
	}
}

func (rt *Runtime) worldsVPS(ctx context.Context, res *WorldsResult, p profile.Profile) {
	if p.WorldRoot == "" {
		res.Note = "set world_root on the profile to list multiple worlds; the active world is " + p.WorldDir
		if p.WorldDir != "" {
			res.Worlds = append(res.Worlds, WorldEntry{Name: filepath.Base(p.WorldDir), Dir: p.WorldDir, Blocks: -1, Active: true})
		}
		return
	}
	t, err := rt.mkTransport(p)
	if err != nil {
		res.Note = err.Error()
		return
	}
	r, err := t.Run(ctx, transport.Command{Argv: eden.WorldList(p.WorldRoot)})
	if err != nil {
		res.Note = "could not list worlds: " + err.Error()
		return
	}
	active := filepath.Clean(p.WorldDir) // multi-world set-active is stage 6.6
	for _, dir := range splitLines(string(r.Stdout)) {
		dir = strings.TrimSpace(dir)
		if dir == "" {
			continue
		}
		e := WorldEntry{Name: filepath.Base(dir), Dir: dir, Blocks: -1, Active: filepath.Clean(dir) == active}
		if c, err := t.Run(ctx, transport.Command{Argv: eden.WorldBlockCount(dir)}); err == nil {
			if f := strings.Fields(string(c.Stdout)); len(f) > 0 {
				if n, err := strconv.ParseInt(f[0], 10, 64); err == nil {
					e.Blocks = n
				}
			}
		}
		res.Worlds = append(res.Worlds, e)
	}
	if len(res.Worlds) == 0 {
		res.Note = "no worlds found under " + p.WorldRoot
	}
}

// --- bundle download / upload --------------------------------------------

// WorldBundle returns a tar of the world's files (the members of
// eden.BundleMembers that exist) plus a suggested download filename.
func (rt *Runtime) WorldBundle(ctx context.Context, p profile.Profile, world string) ([]byte, string, error) {
	dir, err := rt.worldDir(p, world)
	if err != nil {
		return nil, "", err
	}
	name := filepath.Base(dir) + "-" + time.Now().UTC().Format("20060102T150405Z") + ".tar"
	switch p.Kind {
	case profile.Local:
		b, err := packLocalBundle(dir)
		return b, name, err
	case profile.VPS:
		t, err := rt.mkTransport(p)
		if err != nil {
			return nil, "", err
		}
		r, err := t.Run(ctx, transport.Command{Argv: eden.WorldBundlePack(p, dir), Timeout: 60 * time.Second})
		if err != nil {
			return nil, "", err
		}
		if r.ExitCode != 0 {
			return nil, "", fmt.Errorf("tar exited %d: %s", r.ExitCode, strings.TrimSpace(string(r.Stderr)))
		}
		if len(r.Stdout) == 0 {
			return nil, "", fmt.Errorf("bundle is empty — %s may not be a world directory", dir)
		}
		return r.Stdout, name, nil
	}
	return nil, "", fmt.Errorf("unknown profile kind")
}

// packLocalBundle builds the tar in Go — the local transport never shells out.
func packLocalBundle(dir string) ([]byte, error) {
	if !fileExists(filepath.Join(dir, "eden_world.model")) {
		return nil, fmt.Errorf("%s has no eden_world.model", dir)
	}
	var buf bytes.Buffer
	tw := tar.NewWriter(&buf)
	for _, member := range eden.BundleMembers {
		full := filepath.Join(dir, member)
		fi, err := os.Stat(full)
		if err != nil || !fi.Mode().IsRegular() {
			continue
		}
		data, err := os.ReadFile(full)
		if err != nil {
			return nil, err
		}
		hdr := &tar.Header{Name: member, Mode: 0o644, Size: int64(len(data)), ModTime: fi.ModTime()}
		if err := tw.WriteHeader(hdr); err != nil {
			return nil, err
		}
		if _, err := tw.Write(data); err != nil {
			return nil, err
		}
	}
	if err := tw.Close(); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

// UploadBundle writes a bundle tar into worlds/<world>/. It refuses while the
// server is running: the process holds the world in memory and rewrites
// eden_world.model wholesale on every autosave, so an uploaded file would be
// silently overwritten within seconds (plan §3.7).
func (rt *Runtime) UploadBundle(ctx context.Context, p profile.Profile, world string, tarBytes []byte) error {
	if !ValidWorldName(world) {
		return fmt.Errorf("bad world name %q", world)
	}
	if err := rt.requireStopped(ctx, p); err != nil {
		return err
	}
	if err := validateBundle(tarBytes); err != nil {
		return err
	}
	dir, err := rt.worldDir(p, world)
	if err != nil {
		return err
	}
	switch p.Kind {
	case profile.Local:
		return extractLocalBundle(dir, tarBytes)
	case profile.VPS:
		t, err := rt.mkTransport(p)
		if err != nil {
			return err
		}
		if r, err := t.Run(ctx, transport.Command{Argv: eden.Mkdir(p, dir), Timeout: 20 * time.Second}); err != nil {
			return err
		} else if r.ExitCode != 0 {
			return fmt.Errorf("mkdir %s: %s", dir, strings.TrimSpace(string(r.Stderr)))
		}
		r, err := t.Run(ctx, transport.Command{Argv: eden.WorldBundleUnpack(p, dir), Stdin: tarBytes, Timeout: 60 * time.Second})
		if err != nil {
			return err
		}
		if r.ExitCode != 0 {
			return fmt.Errorf("tar -x exited %d: %s", r.ExitCode, strings.TrimSpace(string(r.Stderr)))
		}
		return nil
	}
	return fmt.Errorf("unknown profile kind")
}

// validateBundle checks a tar carries only expected member names (no traversal,
// no separators) and includes eden_world.model.
func validateBundle(tarBytes []byte) error {
	allowed := map[string]bool{}
	for _, m := range eden.BundleMembers {
		allowed[m] = true
	}
	tr := tar.NewReader(bytes.NewReader(tarBytes))
	sawModel := false
	for {
		hdr, err := tr.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			return fmt.Errorf("not a readable tar: %w", err)
		}
		name := path.Clean(hdr.Name)
		if !allowed[name] {
			return fmt.Errorf("bundle has an unexpected entry %q (allowed: %s)", hdr.Name, strings.Join(eden.BundleMembers, ", "))
		}
		if hdr.Typeflag != tar.TypeReg {
			return fmt.Errorf("bundle entry %q is not a regular file", hdr.Name)
		}
		if name == "eden_world.model" {
			sawModel = true
		}
	}
	if !sawModel {
		return fmt.Errorf("bundle has no eden_world.model")
	}
	return nil
}

func extractLocalBundle(dir string, tarBytes []byte) error {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	tr := tar.NewReader(bytes.NewReader(tarBytes))
	for {
		hdr, err := tr.Next()
		if err == io.EOF {
			return nil
		}
		if err != nil {
			return err
		}
		name := path.Clean(hdr.Name)
		if hdr.Typeflag != tar.TypeReg {
			continue
		}
		data, err := io.ReadAll(tr)
		if err != nil {
			return err
		}
		if err := writeAtomic(filepath.Join(dir, name), data); err != nil {
			return err
		}
	}
}

// --- .eden import --------------------------------------------------------

// ImportResult is one eden_import invocation (dry-run projection or real write),
// with the exact command shown for auditability and the parsed summary the
// Worlds panel renders as its confirm gate.
type ImportResult struct {
	Mode      string              `json:"mode"` // "project" | "write"
	Command   string              `json:"command"`
	OK        bool                `json:"ok"` // parsed, and not refused
	Summary   parse.ImportSummary `json:"summary"`
	Error     string              `json:"error,omitempty"`
	SetActive any                 `json:"set_active,omitempty"`
}

// ImportProject runs `eden_import --dry-run` on the uploaded bytes and returns
// the projection. The temp file is removed before this returns on every path.
func (rt *Runtime) ImportProject(ctx context.Context, p profile.Profile, edenBytes []byte, o eden.ImportOptions) ImportResult {
	return rt.runImport(ctx, p, edenBytes, o, true)
}

// ImportWrite runs the real conversion with the same flags the projection was
// approved against.
func (rt *Runtime) ImportWrite(ctx context.Context, p profile.Profile, edenBytes []byte, o eden.ImportOptions) ImportResult {
	return rt.runImport(ctx, p, edenBytes, o, false)
}

func (rt *Runtime) runImport(ctx context.Context, p profile.Profile, edenBytes []byte, o eden.ImportOptions, dryRun bool) ImportResult {
	res := ImportResult{Mode: "write"}
	if dryRun {
		res.Mode = "project"
	}

	t, err := rt.mkTransport(p)
	if err != nil {
		res.Error = err.Error()
		return res
	}

	// Stage the uploaded .eden. Local: a Go temp file. vps: a /tmp path written
	// as the service user. Either way it is removed before we return.
	var argv []string
	switch p.Kind {
	case profile.Local:
		f, err := os.CreateTemp("", "edenadmin-*.eden")
		if err != nil {
			res.Error = "could not stage the upload: " + err.Error()
			return res
		}
		tmp := f.Name()
		defer os.Remove(tmp)
		_, wErr := f.Write(edenBytes)
		f.Close()
		if wErr != nil {
			res.Error = "could not write the staged upload: " + wErr.Error()
			return res
		}
		o.Src = tmp
	case profile.VPS:
		tmp := "/tmp/edenadmin-" + randHex(8) + ".eden"
		defer func() {
			cctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
			defer cancel()
			_, _ = t.Run(cctx, transport.Command{Argv: eden.TmpRemove(p, tmp)})
		}()
		wr, err := t.Run(ctx, transport.Command{Argv: eden.TmpWrite(p, tmp), Stdin: edenBytes, Timeout: 60 * time.Second})
		if err != nil {
			res.Error = "could not stage the upload on the host: " + err.Error()
			return res
		}
		if wr.ExitCode != 0 {
			res.Error = "staging the upload failed: " + strings.TrimSpace(string(wr.Stderr))
			return res
		}
		o.Src = tmp
	default:
		res.Error = "unknown profile kind"
		return res
	}

	if dryRun {
		argv = eden.ImportDryRun(p, o)
	} else {
		argv = eden.ImportRun(p, o)
	}
	res.Command = t.Describe(transport.Command{Argv: argv})

	r, err := t.Run(ctx, transport.Command{Argv: argv, Timeout: 5 * time.Minute})
	if err != nil {
		res.Error = err.Error()
		return res
	}
	summary, perr := parse.ImportSummaryParse(r.Stdout, r.Stderr, r.ExitCode)
	res.Summary = summary
	if perr != nil {
		res.Error = perr.Error()
		return res
	}
	res.OK = !summary.Refused
	return res
}

// --- helpers ------------------------------------------------------------

// worldDir resolves a world name to its directory. An empty name means "the one
// configured world_dir".
func (rt *Runtime) worldDir(p profile.Profile, world string) (string, error) {
	if world == "" {
		if p.WorldDir == "" {
			return "", fmt.Errorf("this profile has no world_dir")
		}
		return p.WorldDir, nil
	}
	if !ValidWorldName(world) {
		return "", fmt.Errorf("bad world name %q", world)
	}
	if p.WorldRoot == "" {
		return "", fmt.Errorf("this profile has no world_root")
	}
	return filepath.Join(p.WorldRoot, world), nil
}

// requireStopped returns an error unless the server for p is confirmed not
// running. It is deliberately conservative: an inconclusive check refuses.
func (rt *Runtime) requireStopped(ctx context.Context, p profile.Profile) error {
	switch p.Kind {
	case profile.Local:
		if sup := rt.Supervisor(p); sup != nil && sup.Status().Running {
			return fmt.Errorf("the server is running — stop it first (an upload into a live world is overwritten on the next autosave)")
		}
		return nil
	case profile.VPS:
		t, err := rt.mkTransport(p)
		if err != nil {
			return err
		}
		r, err := t.Run(ctx, transport.Command{Argv: eden.SystemctlShow(p, "ActiveState"), Timeout: 15 * time.Second})
		if err != nil {
			return fmt.Errorf("could not confirm the server is stopped: %v", err)
		}
		if strings.Contains(string(r.Stdout), "ActiveState=active") {
			return fmt.Errorf("the server is active — stop it first")
		}
		return nil
	}
	return fmt.Errorf("unknown profile kind")
}

// activeWorldDir is the directory the local profile's args currently host.
func activeWorldDir(p profile.Profile) string {
	if w := parseWorldArgs(p.Args).World; w != "" {
		return filepath.Clean(filepath.Dir(w))
	}
	if p.WorldDir != "" {
		return filepath.Clean(p.WorldDir)
	}
	return ""
}

func fileExists(path string) bool {
	fi, err := os.Stat(path)
	return err == nil && fi.Mode().IsRegular()
}

// countLines counts '\n' in a file (eden_world.model is one cell per line). It
// streams so a multi-megabyte model does not land in memory.
func countLines(path string) int64 {
	f, err := os.Open(path)
	if err != nil {
		return -1
	}
	defer f.Close()
	var total int64
	buf := make([]byte, 128*1024)
	for {
		n, err := f.Read(buf)
		total += int64(bytes.Count(buf[:n], []byte{'\n'}))
		if err != nil {
			break
		}
	}
	return total
}

func writeAtomic(dst string, data []byte) error {
	tmp, err := os.CreateTemp(filepath.Dir(dst), ".edenadmin-*")
	if err != nil {
		return err
	}
	tmpName := tmp.Name()
	defer os.Remove(tmpName)
	if _, err := tmp.Write(data); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	return os.Rename(tmpName, dst)
}

func splitLines(s string) []string {
	s = strings.ReplaceAll(s, "\r\n", "\n")
	return strings.Split(s, "\n")
}

func randHex(n int) string {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		return strconv.FormatInt(time.Now().UnixNano(), 16)
	}
	return hex.EncodeToString(b)
}
