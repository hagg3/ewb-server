package eden

import "github.com/hagg3/ewb-server/admin/internal/profile"

// eden_import (Phase 5) is an offline .eden -> worlds/<name>/ converter that
// build_server.sh builds by default. The Worlds panel drives it in two passes:
// once with --dry-run to render the projection the operator confirms against,
// once for real with the same strategy/spawn/name flags. --yes is on both
// passes for the same reason `ssh -o BatchMode=yes` is: the GUI must never risk
// a blocking TTY read. See docs/import.md and the plan §3.7.

// ImportOptions is the operator's choices for one import. A zero value is a
// valid "all defaults" import (diff strategy, header spawn, no overwrite).
type ImportOptions struct {
	Src    string // path to the uploaded .eden file
	OutDir string // --out target (worlds/<name>/)

	AirFill string // "", "diff", "solid", "full" ("" -> tool default)
	Spawn   string // "", "header", "home", "none", or "x,y,z"
	Force   bool   // --force (overwrite an existing OutDir)
	Strict  bool   // --strict

	NoSigns   bool
	SignsFile string // --signs FILE

	// Budget overrides. Left at their zero values they are omitted and the
	// tool's own defaults apply. A non-zero value is an operator acknowledging
	// a warning or a refusal.
	MaxWorldCells    int // --max-world-cells N   (0 -> omit)
	MaxRegionRecords int // --max-region-records N (-1 -> omit; 0 is a real value that disables the check)
	RegionRadius     int // --region-radius N     (0 -> omit)
}

// RecommendedMaxWorldCells mirrors hardening.h's recommended_max_world_cells: the
// server cap a world of `cells` should run under — a quarter again, never less
// than a million, rounded up to a whole 100,000. A server whose cap equals its
// world's cell count refuses every new block players place (edits to existing
// cells still save), so the cap written for an import must leave room above it.
func RecommendedMaxWorldCells(cells int64) int {
	if cells < 0 {
		cells = 0
	}
	headroom := cells / 4
	if headroom < 1000000 {
		headroom = 1000000
	}
	want := cells + headroom
	return int((want + 99999) / 100000 * 100000)
}

// NewImportOptions returns options with MaxRegionRecords set to the "omit"
// sentinel (-1), since 0 is a meaningful value for that flag.
func NewImportOptions() ImportOptions {
	return ImportOptions{MaxRegionRecords: -1}
}

func importPrefix(p profile.Profile) []string {
	bin := p.EdenImportPath()
	if p.Kind == profile.VPS {
		return []string{"sudo", "-n", "-u", p.RunAs, bin}
	}
	return []string{bin}
}

func importArgv(p profile.Profile, o ImportOptions, dryRun bool) []string {
	argv := append(importPrefix(p), o.Src, "--out", o.OutDir, "--yes")
	if dryRun {
		argv = append(argv, "--dry-run")
	}
	if o.Force {
		argv = append(argv, "--force")
	}
	if o.Strict {
		argv = append(argv, "--strict")
	}
	if o.AirFill != "" {
		argv = append(argv, "--air-fill", o.AirFill)
	}
	if o.Spawn != "" {
		argv = append(argv, "--spawn", o.Spawn)
	}
	if o.NoSigns {
		argv = append(argv, "--no-signs")
	} else if o.SignsFile != "" {
		argv = append(argv, "--signs", o.SignsFile)
	}
	if o.MaxWorldCells > 0 {
		argv = append(argv, "--max-world-cells", itoa(o.MaxWorldCells))
	}
	if o.MaxRegionRecords >= 0 {
		argv = append(argv, "--max-region-records", itoa(o.MaxRegionRecords))
	}
	if o.RegionRadius > 0 {
		argv = append(argv, "--region-radius", itoa(o.RegionRadius))
	}
	return argv
}

// ImportDryRun projects the import and writes nothing.
func ImportDryRun(p profile.Profile, o ImportOptions) []string { return importArgv(p, o, true) }

// ImportRun performs the import.
func ImportRun(p profile.Profile, o ImportOptions) []string { return importArgv(p, o, false) }

// ImportHelp is the Connection probe: exit 0 means eden_import is present and
// runnable (as the service user, on a vps profile).
func ImportHelp(p profile.Profile) []string {
	return append(importPrefix(p), "-h")
}
