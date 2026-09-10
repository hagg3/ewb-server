package eden

import "github.com/hagg3/ewb-server/admin/internal/profile"

// Backup verbs (roadmap 6.5). Only the vps transport uses these: on a local
// profile edenadmin lists the backup directory, copies the world files and
// streams the restore swap in Go (the "local never shells out for information"
// rule, plan §1). Every builder here is written so the ssh path quotes it with
// no shell metacharacters of our own.

// BackupMembers is the world state a backup captures — the same set as a world
// bundle minus the .gitignore, which is repo bookkeeping, not world data.
var BackupMembers = []string{
	"eden_world.model",
	"eden_signs.txt",
	"eden_spawn.txt",
	"eden_players.txt",
}

// BackupMemberCandidates is every filename a backup directory may hold: each
// member both gzipped and plain. edenserverctl gzips by default (a world file is
// plaintext and deflates to roughly a sixth), but EDENSERVER_BACKUP_COMPRESS=0
// and any backup taken before that change store the plain name, so both forms
// have to be readable forever. Callers pair this with tar's
// --ignore-failed-read: only one of the two exists per member.
func BackupMemberCandidates() []string {
	names := make([]string, 0, len(BackupMembers)*2)
	for _, m := range BackupMembers {
		names = append(names, m+".gz", m)
	}
	return names
}

// BackupDiskUsage is `du -k --max-depth=1 <backup_dir>`: one "<kbytes>\t<path>"
// line per stamp directory plus a trailing total line for the directory itself.
// parse.DuDirs reads it. GNU du (the Linux host ops/INSTALL.md describes).
func BackupDiskUsage(backupDir string) []string {
	return []string{"du", "-k", "--max-depth=1", backupDir}
}

// BackupNow runs edenserverctl's own `backup` verb as the service user. The
// EDENSERVER_* overrides go through `env` because sudo scrubs the environment
// and `sudo -E` is not a habit to encode into a tool (plan §3.8) — the `env`
// form must be permitted by the sudoers rule. edenserverctl calls `edenctl
// save` itself before copying, so the backup is current rather than up to one
// autosave interval stale.
func BackupNow(p profile.Profile) []string {
	return []string{
		"sudo", "-n", "-u", p.RunAs, "env",
		"EDENSERVER_WORLD_DIR=" + p.WorldDir,
		"EDENSERVER_BACKUP_DIR=" + p.BackupDirPath(),
		p.EdenserverctlPath(), "backup",
	}
}

// BackupPack streams a tar of the named stamp directory's world files to
// stdout, as the service user. It asks for both the gzipped and the plain name
// of every member; --ignore-failed-read tolerates the absent one, and a backup
// that predates a member entirely. The caller decompresses the .gz entries
// before unpacking into a world dir — see panels.plainBackupTar.
func BackupPack(p profile.Profile, stampDir string) []string {
	argv := runAs(p, "tar", "-C", stampDir, "--ignore-failed-read", "-cf", "-")
	return append(argv, BackupMemberCandidates()...)
}
