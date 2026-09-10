package panels

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"context"
	"fmt"
	"io"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"

	"github.com/hagg3/ewb-server/admin/internal/eden"
	"github.com/hagg3/ewb-server/admin/internal/parse"
	"github.com/hagg3/ewb-server/admin/internal/profile"
	"github.com/hagg3/ewb-server/admin/internal/transport"
)

// Backups panel (roadmap 6.5). List / create / restore of the world files under
// a profile's backup_dir. Restore is the most dangerous action in edenadmin: it
// takes a mandatory safety copy first, only swaps files with the server
// confirmed stopped, and rolls back from the safety copy if a swap fails
// half-way (plan §3.8).

// backupStampRE matches the timestamp edenserverctl and this panel use for a
// backup directory name: YYYYMMDDTHHMMSSZ.
var backupStampRE = regexp.MustCompile(`^\d{8}T\d{6}Z$`)

const preRestoreSuffix = "-prerestore"

// BackupEntry is one directory under backup_dir.
type BackupEntry struct {
	Stamp      string `json:"stamp"`       // the directory name
	Bytes      int64  `json:"bytes"`       // total size of the regular files in it, -1 if unknown
	IsStamp    bool   `json:"is_stamp"`    // the name is a well-formed UTC stamp
	PreRestore bool   `json:"pre_restore"` // a "<stamp>-prerestore" safety copy this panel wrote
	HasModel   bool   `json:"has_model"`   // contains eden_world.model, so it can be restored (local only; a vps listing assumes true)
}

// BackupsResult is the Backups panel payload.
type BackupsResult struct {
	Profile string        `json:"profile"`
	Kind    string        `json:"kind"`
	Dir     string        `json:"dir"`
	Backups []BackupEntry `json:"backups"`
	Note    string        `json:"note,omitempty"`
}

// Backups lists the backups under the profile's backup_dir. A local profile
// reads the directory in Go; a vps profile runs `du -k --max-depth=1` over ssh.
func (rt *Runtime) Backups(ctx context.Context, p profile.Profile) BackupsResult {
	res := BackupsResult{Profile: p.Name, Kind: string(p.Kind), Dir: p.BackupDirPath()}
	switch p.Kind {
	case profile.Local:
		rt.backupsLocal(&res, p)
	case profile.VPS:
		rt.backupsVPS(ctx, &res, p)
	default:
		res.Note = "unknown profile kind"
	}
	return res
}

func (rt *Runtime) backupsLocal(res *BackupsResult, p profile.Profile) {
	dir := res.Dir
	if dir == "" {
		res.Note = "this profile has no backup_dir and no world_dir to derive one"
		return
	}
	ents, err := os.ReadDir(dir)
	if os.IsNotExist(err) {
		res.Note = "no backups yet — " + dir + " does not exist"
		return
	}
	if err != nil {
		res.Note = "cannot read backup_dir: " + err.Error()
		return
	}
	for _, e := range ents {
		if !e.IsDir() {
			continue
		}
		be := classifyBackupName(e.Name())
		full := filepath.Join(dir, e.Name())
		be.Bytes = dirSize(full)
		be.HasModel = backupHasMember(full, "eden_world.model")
		res.Backups = append(res.Backups, be)
	}
	sortBackupsDesc(res.Backups)
	if len(res.Backups) == 0 {
		res.Note = "no backups under " + dir
	}
}

func (rt *Runtime) backupsVPS(ctx context.Context, res *BackupsResult, p profile.Profile) {
	dir := res.Dir
	t, err := rt.mkTransport(p)
	if err != nil {
		res.Note = err.Error()
		return
	}
	r, err := t.Run(ctx, transport.Command{Argv: eden.BackupDiskUsage(dir), Timeout: 20 * time.Second})
	if err != nil {
		res.Note = "could not list backups: " + err.Error()
		return
	}
	if r.ExitCode != 0 {
		msg := strings.TrimSpace(string(r.Stderr))
		if strings.Contains(msg, "No such file") {
			res.Note = "no backups yet — " + dir + " does not exist"
			return
		}
		res.Note = orElse(msg, fmt.Sprintf("du exited %d", r.ExitCode))
		return
	}
	entries, perr := parse.DuDirs(r.Stdout)
	if perr != nil {
		res.Note = perr.Error()
		return
	}
	want := strings.TrimRight(dir, "/")
	for _, de := range entries {
		if strings.TrimRight(de.Path, "/") == want {
			continue // the trailing total line for backup_dir itself
		}
		be := classifyBackupName(path.Base(de.Path))
		be.Bytes = de.KBytes * 1024
		be.HasModel = true // a remote stat per entry is not worth an ssh round trip; restore re-checks
		res.Backups = append(res.Backups, be)
	}
	sortBackupsDesc(res.Backups)
	if len(res.Backups) == 0 {
		res.Note = "no backups under " + dir
	}
}

// --- create --------------------------------------------------------------

// BackupCreateResult reports one "Backup now".
type BackupCreateResult struct {
	OK      bool   `json:"ok"`
	Stamp   string `json:"stamp,omitempty"`
	Message string `json:"message"`
	Command string `json:"command,omitempty"`
}

// BackupCreate makes a new backup. local: `edenctl save` (best effort) then copy
// the world files into backup_dir/<stamp>/ in Go. vps: `edenserverctl backup`
// as the service user, which does the save itself.
func (rt *Runtime) BackupCreate(ctx context.Context, p profile.Profile) BackupCreateResult {
	switch p.Kind {
	case profile.Local:
		return rt.backupCreateLocal(ctx, p)
	case profile.VPS:
		return rt.backupCreateVPS(ctx, p)
	}
	return BackupCreateResult{Message: "unknown profile kind"}
}

func (rt *Runtime) backupCreateLocal(ctx context.Context, p profile.Profile) BackupCreateResult {
	dir := p.BackupDirPath()
	if dir == "" {
		return BackupCreateResult{Message: "this profile has no backup_dir and no world_dir to derive one"}
	}
	worldDir := activeWorldDirFor(p)
	if worldDir == "" || !fileExists(filepath.Join(worldDir, "eden_world.model")) {
		return BackupCreateResult{Message: "no world to back up — " + worldDir + " has no eden_world.model"}
	}
	// Flush first if the server is up, so the copy is current rather than up to
	// one autosave interval stale. Best effort — a stopped server writes its
	// files atomically anyway (rename(2)), so a plain copy is still consistent.
	if sup := rt.Supervisor(p); sup != nil && sup.Status().Running {
		_, _ = rt.runCtl(ctx, p, eden.Save(p))
	}

	stamp := time.Now().UTC().Format("20060102T150405Z")
	dest := filepath.Join(dir, stamp)
	if _, err := os.Stat(dest); err == nil {
		return BackupCreateResult{Message: "a backup named " + stamp + " already exists — wait a second and retry"}
	}
	copied, err := compressBackupMembers(worldDir, dest)
	if err != nil {
		return BackupCreateResult{Message: "backup failed after " + fmt.Sprint(copied) + " file(s): " + err.Error()}
	}
	if copied == 0 {
		_ = os.Remove(dest)
		return BackupCreateResult{Message: "no world files found in " + worldDir}
	}
	return BackupCreateResult{OK: true, Stamp: stamp, Message: fmt.Sprintf("backed up %d file(s) to %s", copied, dest)}
}

func (rt *Runtime) backupCreateVPS(ctx context.Context, p profile.Profile) BackupCreateResult {
	t, err := rt.mkTransport(p)
	if err != nil {
		return BackupCreateResult{Message: err.Error()}
	}
	argv := eden.BackupNow(p)
	res := BackupCreateResult{Command: t.Describe(transport.Command{Argv: argv})}
	r, err := t.Run(ctx, transport.Command{Argv: argv, Timeout: 90 * time.Second})
	if err != nil {
		res.Message = err.Error()
		return res
	}
	if r.ExitCode != 0 {
		if transport.SudoDenied(r.Stderr) {
			res.Message = "sudo -n was denied — the sudoers rule for `env … edenserverctl backup` is missing"
			return res
		}
		res.Message = orElse(strings.TrimSpace(string(r.Stderr)), fmt.Sprintf("edenserverctl backup exited %d", r.ExitCode))
		return res
	}
	res.OK = true
	res.Message = orElse(firstLine(string(r.Stdout)), "backup complete")
	res.Stamp = stampFromBackupOutput(string(r.Stdout))
	return res
}

// --- restore -------------------------------------------------------------

// BackupRestoreResult is the trace of a restore attempt. Steps is the ordered
// human-readable log the panel shows; SafetyCopy names the pre-restore backup
// that a mid-swap failure leaves behind.
type BackupRestoreResult struct {
	OK         bool     `json:"ok"`
	Stamp      string   `json:"stamp"`
	SafetyCopy string   `json:"safety_copy,omitempty"`
	Steps      []string `json:"steps"`
	Message    string   `json:"message"`
	ServerNote string   `json:"server_note,omitempty"`
}

func (r *BackupRestoreResult) step(format string, a ...any) {
	r.Steps = append(r.Steps, fmt.Sprintf(format, a...))
}

// BackupRestore replaces the live world files with those from a backup. The
// sequence is: safety-copy the current world, stop the server, confirm it is
// dead, swap the files (rolling back from the safety copy on a mid-swap error),
// then start the server again if it had been running. The swap never runs while
// the process is live — a running server rewrites eden_world.model wholesale on
// every autosave (roadmap 6.5 exit criterion).
func (rt *Runtime) BackupRestore(ctx context.Context, p profile.Profile, stamp string) BackupRestoreResult {
	res := BackupRestoreResult{Stamp: stamp}

	if stamp == "" || strings.ContainsAny(stamp, "/\\") || stamp == "." || stamp == ".." {
		res.Message = "bad backup name"
		return res
	}
	dir := p.BackupDirPath()
	if dir == "" {
		res.Message = "this profile has no backup_dir"
		return res
	}
	srcDir := filepath.Join(dir, stamp)
	worldDir := activeWorldDirFor(p)
	if worldDir == "" {
		res.Message = "this profile has no world to restore into"
		return res
	}

	switch p.Kind {
	case profile.Local:
		return rt.backupRestoreLocal(ctx, &res, p, srcDir, worldDir)
	case profile.VPS:
		return rt.backupRestoreVPS(ctx, &res, p, srcDir, worldDir)
	}
	res.Message = "unknown profile kind"
	return res
}

func (rt *Runtime) backupRestoreLocal(ctx context.Context, res *BackupRestoreResult, p profile.Profile, srcDir, worldDir string) BackupRestoreResult {
	if !backupHasMember(srcDir, "eden_world.model") {
		res.Message = "backup " + res.Stamp + " has no eden_world.model"
		return *res
	}

	// 1. Safety copy of the current world, before anything is touched.
	pre := time.Now().UTC().Format("20060102T150405Z") + preRestoreSuffix
	preDir := filepath.Join(p.BackupDirPath(), pre)
	if _, err := compressBackupMembers(worldDir, preDir); err != nil {
		res.Message = "aborted — could not take the safety copy: " + err.Error()
		return *res
	}
	res.SafetyCopy = pre
	res.step("safety copy of the current world → %s", pre)

	// 2. Stop the server and 3. confirm it is dead before any file is swapped.
	sup := rt.Supervisor(p)
	wasRunning := sup != nil && sup.Status().Running
	if wasRunning {
		if _, err := sup.Stop(ctx); err != nil {
			res.Message = "aborted — could not stop the server (" + err.Error() + "); nothing was swapped. Safety copy at " + pre
			return *res
		}
		res.step("stopped the server")
	}
	if sup != nil && sup.Status().Running {
		res.Message = "aborted — the server is still running after a stop; nothing was swapped. Safety copy at " + pre
		return *res
	}

	// 4. Swap. On the first failure, roll back from the safety copy.
	if copied, err := restoreBackupMembers(srcDir, worldDir); err != nil {
		res.step("swap failed after %d file(s): %v", copied, err)
		if _, rbErr := restoreBackupMembers(preDir, worldDir); rbErr != nil {
			res.Message = "SWAP FAILED AND ROLLBACK FAILED (" + rbErr.Error() + "). The world may be inconsistent — restore by hand from " + preDir
			return *res
		}
		res.step("rolled back from the safety copy")
		res.Message = "restore failed mid-swap; rolled back to the pre-restore state. Safety copy kept at " + pre
		return *res
	}
	res.step("swapped in the backup files")

	// 5. Start again if it had been running.
	if wasRunning && sup != nil {
		if st, err := sup.Start(ctx); err != nil {
			res.ServerNote = "the world was restored but the server did not start again: " + err.Error()
		} else {
			res.ServerNote = fmt.Sprintf("server restarted (pid %d)", st.PID)
		}
	}

	res.OK = true
	res.Message = "restored " + res.Stamp + " (safety copy of the previous world at " + pre + ")"
	return *res
}

func (rt *Runtime) backupRestoreVPS(ctx context.Context, res *BackupRestoreResult, p profile.Profile, srcDir, worldDir string) BackupRestoreResult {
	t, err := rt.mkTransport(p)
	if err != nil {
		res.Message = err.Error()
		return *res
	}

	// 1. Safety copy — a normal backup run, plus an in-memory tar of the current
	//    world for a fast rollback.
	sc := rt.backupCreateVPS(ctx, p)
	if !sc.OK {
		res.Message = "aborted — could not take the safety copy: " + sc.Message
		return *res
	}
	res.SafetyCopy = sc.Stamp
	res.step("safety copy of the current world → %s", orElse(sc.Stamp, "(a backup run)"))

	cur, err := t.Run(ctx, transport.Command{Argv: eden.WorldBundlePack(p, worldDir), Timeout: 60 * time.Second})
	if err != nil || cur.ExitCode != 0 {
		res.Message = "aborted — could not snapshot the current world for rollback"
		return *res
	}

	// 2. Graceful stop (never systemctl stop — no SIGTERM handler), unless the
	//    unit is already inactive, and 3. confirm it is inactive before any swap.
	wasActive := true
	if st, err := t.Run(ctx, transport.Command{Argv: eden.SystemctlShow(p, "ActiveState"), Timeout: 15 * time.Second}); err == nil && strings.Contains(string(st.Stdout), "ActiveState=inactive") {
		wasActive = false
		res.step("server already stopped")
	}
	if wasActive {
		if ok, line := rt.gracefulStop(ctx, p); !ok {
			res.Message = "aborted — " + line + "; nothing was swapped"
			return *res
		}
		res.step("graceful stop requested (world saved)")
		rt.waitVPSInactive(ctx, p, 20*time.Second)
		if st, err := t.Run(ctx, transport.Command{Argv: eden.SystemctlShow(p, "ActiveState"), Timeout: 15 * time.Second}); err != nil || !strings.Contains(string(st.Stdout), "ActiveState=inactive") {
			res.Message = "aborted — the unit did not reach `inactive` after a graceful stop; nothing was swapped. Safety copy at " + res.SafetyCopy
			return *res
		}
	}

	// 4. Swap: stream the backup's files into the world dir. Roll back from the
	//    in-memory snapshot on failure.
	packed, err := t.Run(ctx, transport.Command{Argv: eden.BackupPack(p, srcDir), Timeout: 60 * time.Second})
	if err != nil || packed.ExitCode != 0 || len(packed.Stdout) == 0 {
		res.Message = "aborted before the swap — could not read backup " + res.Stamp + "; the server is stopped. Safety copy at " + res.SafetyCopy
		return *res
	}
	// The backup's members are gzipped on disk; the world dir wants them plain.
	// Decompressing here rather than on the host keeps the swap a single
	// `tar -xf -` — no shell pipeline to quote, and no new sudoers verb.
	plain, err := plainBackupTar(packed.Stdout)
	if err != nil {
		res.Message = "aborted before the swap — backup " + res.Stamp + " is unreadable (" + err.Error() + "); the server is stopped. Safety copy at " + res.SafetyCopy
		return *res
	}
	if r, err := t.Run(ctx, transport.Command{Argv: eden.WorldBundleUnpack(p, worldDir), Stdin: plain, Timeout: 60 * time.Second}); err != nil || r.ExitCode != 0 {
		res.step("swap failed — rolling back")
		if rb, rbErr := t.Run(ctx, transport.Command{Argv: eden.WorldBundleUnpack(p, worldDir), Stdin: cur.Stdout, Timeout: 60 * time.Second}); rbErr != nil || rb.ExitCode != 0 {
			res.Message = "SWAP FAILED AND ROLLBACK FAILED. Restore by hand on the host from " + filepath.Join(p.BackupDirPath(), res.SafetyCopy)
			return *res
		}
		res.step("rolled back from the safety copy")
		res.Message = "restore failed mid-swap; rolled back. Safety copy kept at " + res.SafetyCopy
		return *res
	}
	res.step("swapped in the backup files")

	// 5. Start again if it had been running.
	if wasActive {
		if ok, msg := rt.runHost(ctx, p, eden.Start(p), "server started"); !ok {
			res.ServerNote = "the world was restored but the server did not start again: " + msg
		} else {
			res.ServerNote = msg
		}
	}

	res.OK = true
	res.Message = "restored " + res.Stamp + " (safety copy of the previous world at " + res.SafetyCopy + ")"
	return *res
}

// --- helpers ------------------------------------------------------------

// activeWorldDirFor is the directory whose world files a backup/restore acts on.
// For a local profile that is the world the args currently host; for vps it is
// the single configured world_dir (multi-world set-active is deferred).
func activeWorldDirFor(p profile.Profile) string {
	if p.Kind == profile.Local {
		if d := activeWorldDir(p); d != "" {
			return d
		}
	}
	return filepath.Clean(p.WorldDir)
}

// backupGzipLevel matches edenserverctl's default. On a 40 MB world level 9 buys
// about 4% more for roughly six times the CPU, which is the same trade the Eden
// editor declined for its own .bak.zip snapshots.
const backupGzipLevel = gzip.DefaultCompression

// compressBackupMembers gzips every eden.BackupMembers file that exists in the
// world directory srcDir into dstDir (created if needed) as "<member>.gz",
// atomically per file. Same output shape as `edenserverctl backup`, so the two
// producers stay interchangeable. Returns the count written, stopping at the
// first error so the caller can roll back.
func compressBackupMembers(srcDir, dstDir string) (int, error) {
	if err := os.MkdirAll(dstDir, 0o755); err != nil {
		return 0, err
	}
	written := 0
	for _, m := range eden.BackupMembers {
		data, err := os.ReadFile(filepath.Join(srcDir, m))
		if err != nil {
			if os.IsNotExist(err) {
				continue
			}
			return written, err
		}
		packed, err := gzipBytes(data)
		if err != nil {
			return written, err
		}
		if err := writeAtomic(filepath.Join(dstDir, m+".gz"), packed); err != nil {
			return written, err
		}
		written++
	}
	return written, nil
}

// restoreBackupMembers is the inverse: it writes every member of the backup
// directory srcDir into the world directory dstDir under its plain name,
// decompressing "<member>.gz" and copying a bare "<member>" as-is. The plain
// form is what a pre-compression backup and an EDENSERVER_BACKUP_COMPRESS=0 run
// leave behind, so both are accepted; the gzipped one wins if somehow both
// exist, being the form the current code writes.
func restoreBackupMembers(srcDir, dstDir string) (int, error) {
	if err := os.MkdirAll(dstDir, 0o755); err != nil {
		return 0, err
	}
	restored := 0
	for _, m := range eden.BackupMembers {
		data, err := readBackupMember(srcDir, m)
		if err != nil {
			if os.IsNotExist(err) {
				continue
			}
			return restored, err
		}
		if err := writeAtomic(filepath.Join(dstDir, m), data); err != nil {
			return restored, err
		}
		restored++
	}
	return restored, nil
}

// readBackupMember reads one member out of a backup directory in whichever form
// it is stored, returning its plain bytes. os.ErrNotExist if neither form is
// there.
func readBackupMember(dir, member string) ([]byte, error) {
	if data, err := os.ReadFile(filepath.Join(dir, member+".gz")); err == nil {
		return gunzipBytes(data)
	} else if !os.IsNotExist(err) {
		return nil, err
	}
	return os.ReadFile(filepath.Join(dir, member))
}

// backupHasMember reports whether a backup directory holds member in either
// form. Used for the "can this be restored?" check, which must not reject a
// gzipped backup for lacking the plain name.
func backupHasMember(dir, member string) bool {
	return fileExists(filepath.Join(dir, member+".gz")) || fileExists(filepath.Join(dir, member))
}

// plainBackupTar rewrites a tar streamed by eden.BackupPack so every member
// carries its plain world-file name and uncompressed bytes: a "<member>.gz"
// entry is inflated and renamed, a bare "<member>" passes through untouched
// (pre-compression backups, and EDENSERVER_BACKUP_COMPRESS=0 hosts). Anything
// else — a directory, a name outside eden.BackupMembers — is dropped, so a
// hand-edited backup dir can't smuggle a path into the world directory.
func plainBackupTar(data []byte) ([]byte, error) {
	want := make(map[string]bool, len(eden.BackupMembers))
	for _, m := range eden.BackupMembers {
		want[m] = true
	}

	var out bytes.Buffer
	tw := tar.NewWriter(&out)
	tr := tar.NewReader(bytes.NewReader(data))
	kept := 0
	for {
		hdr, err := tr.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			return nil, err
		}
		if hdr.Typeflag != tar.TypeReg {
			continue
		}
		name := path.Base(hdr.Name)
		body, err := io.ReadAll(tr)
		if err != nil {
			return nil, err
		}
		if strings.HasSuffix(name, ".gz") {
			name = strings.TrimSuffix(name, ".gz")
			if body, err = gunzipBytes(body); err != nil {
				return nil, fmt.Errorf("%s: %w", hdr.Name, err)
			}
		}
		if !want[name] {
			continue
		}
		nh := &tar.Header{
			Name:     name,
			Mode:     0o644,
			Size:     int64(len(body)),
			ModTime:  hdr.ModTime,
			Typeflag: tar.TypeReg,
		}
		if err := tw.WriteHeader(nh); err != nil {
			return nil, err
		}
		if _, err := tw.Write(body); err != nil {
			return nil, err
		}
		kept++
	}
	if err := tw.Close(); err != nil {
		return nil, err
	}
	if kept == 0 {
		return nil, fmt.Errorf("no world files in the backup")
	}
	return out.Bytes(), nil
}

func gzipBytes(data []byte) ([]byte, error) {
	var buf bytes.Buffer
	zw, err := gzip.NewWriterLevel(&buf, backupGzipLevel)
	if err != nil {
		return nil, err
	}
	if _, err := zw.Write(data); err != nil {
		return nil, err
	}
	if err := zw.Close(); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

func gunzipBytes(data []byte) ([]byte, error) {
	zr, err := gzip.NewReader(bytes.NewReader(data))
	if err != nil {
		return nil, err
	}
	defer zr.Close()
	return io.ReadAll(zr)
}

// dirSize sums the regular files directly in dir. -1 if the directory can't be
// read.
func dirSize(dir string) int64 {
	ents, err := os.ReadDir(dir)
	if err != nil {
		return -1
	}
	var total int64
	for _, e := range ents {
		if info, err := e.Info(); err == nil && info.Mode().IsRegular() {
			total += info.Size()
		}
	}
	return total
}

func classifyBackupName(name string) BackupEntry {
	be := BackupEntry{Stamp: name, Bytes: -1}
	base := strings.TrimSuffix(name, preRestoreSuffix)
	be.PreRestore = base != name
	be.IsStamp = backupStampRE.MatchString(base)
	return be
}

func sortBackupsDesc(b []BackupEntry) {
	sort.Slice(b, func(i, j int) bool { return b[i].Stamp > b[j].Stamp })
}

func firstLine(s string) string {
	s = strings.ReplaceAll(s, "\r\n", "\n")
	if i := strings.IndexByte(s, '\n'); i >= 0 {
		return strings.TrimSpace(s[:i])
	}
	return strings.TrimSpace(s)
}

// stampFromBackupOutput pulls the stamp out of edenserverctl's
// "Backed up N file(s) to <dir>/<stamp>" line.
func stampFromBackupOutput(out string) string {
	for _, ln := range strings.Split(strings.ReplaceAll(out, "\r\n", "\n"), "\n") {
		if i := strings.LastIndex(ln, " to "); i >= 0 {
			cand := path.Base(strings.TrimSpace(ln[i+4:]))
			if backupStampRE.MatchString(cand) {
				return cand
			}
		}
	}
	return ""
}
