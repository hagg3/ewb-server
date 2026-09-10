package eden

import "github.com/hagg3/ewb-server/admin/internal/profile"

// World-directory verbs for the Worlds panel (roadmap 6.4). Only the vps
// transport uses these: on a local profile edenadmin lists directories, counts
// lines and packs the bundle tar in Go (the "local never shells out for
// information" rule, plan §1). Every builder here is written so the ssh path
// quotes it with no shell metacharacters of our own.

// BundleMembers is the canonical file set of a world directory, in the order a
// bundle lists them. eden_signs.txt / eden_spawn.txt / eden_players.txt are each
// optional — a freshly imported or never-joined world lacks some — so a packer
// takes only the members that exist (or, on vps, lets tar skip the missing ones
// with --ignore-failed-read). .gitignore is carried too: eden_import writes it
// and `worlds/` is tracked (docs/import.md), so a round trip must not drop it.
var BundleMembers = []string{
	".gitignore",
	"eden_world.model",
	"eden_signs.txt",
	"eden_spawn.txt",
	"eden_players.txt",
}

// runAs wraps a host command in `sudo -n -u <run_as>` for a vps profile (the
// world files are service-user-owned), and leaves it alone for local.
func runAs(p profile.Profile, args ...string) []string {
	if p.Kind == profile.VPS {
		return append([]string{"sudo", "-n", "-u", p.RunAs}, args...)
	}
	return args
}

// WorldList finds every `<root>/<name>/eden_world.model` two levels down and
// prints the containing directory. `-printf '%h\n'` is GNU find; the vps host is
// the Linux box ops/INSTALL.md describes.
func WorldList(root string) []string {
	return []string{
		"find", root, "-mindepth", "2", "-maxdepth", "2",
		"-name", "eden_world.model", "-printf", "%h\n",
	}
}

// WorldBlockCount is `wc -l <dir>/eden_world.model`; the reply is "<n> <path>".
func WorldBlockCount(dir string) []string {
	return []string{"wc", "-l", dir + "/eden_world.model"}
}

// WorldDirFiles lists the regular files directly in a world directory, so the
// caller can pack only the ones that exist. Run as the service user because the
// directory mode may be 0750.
func WorldDirFiles(p profile.Profile, dir string) []string {
	return runAs(p, "find", dir, "-maxdepth", "1", "-type", "f", "-printf", "%f\n")
}

// WorldBundlePack streams a tar of the world directory to stdout. --ignore-failed-read
// keeps a legitimately-absent eden_players.txt / eden_signs.txt from failing the
// whole command; the member list is still explicit so nothing else (the socket,
// a log) is swept in.
func WorldBundlePack(p profile.Profile, dir string) []string {
	argv := runAs(p, "tar", "-C", dir, "--ignore-failed-read", "-cf", "-")
	return append(argv, BundleMembers...)
}

// WorldBundleUnpack extracts a tar from stdin into the world directory, as the
// service user so the written files carry its ownership.
func WorldBundleUnpack(p profile.Profile, dir string) []string {
	return runAs(p, "tar", "-C", dir, "-xf", "-")
}

// TmpWrite writes stdin to an absolute temp path as the service user, so a
// subsequently sudo-'d eden_import can read it. `dd` avoids a shell redirect.
func TmpWrite(p profile.Profile, path string) []string {
	return runAs(p, "dd", "of="+path, "status=none")
}

// TmpRemove deletes the import temp file. Called on every path — success,
// parse failure, or refusal (plan §3.7: "rm -f it in the same panel action").
func TmpRemove(p profile.Profile, path string) []string {
	return runAs(p, "rm", "-f", path)
}

// Mkdir -p's a directory as the service user (the vps upload target for a
// brand-new world).
func Mkdir(p profile.Profile, dir string) []string {
	return runAs(p, "mkdir", "-p", dir)
}
