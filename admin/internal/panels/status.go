package panels

import (
	"context"
	"syscall"

	"github.com/hagg3/ewb-server/admin/internal/eden"
	"github.com/hagg3/ewb-server/admin/internal/parse"
	"github.com/hagg3/ewb-server/admin/internal/profile"
)

// StatusResult is the Status panel payload, polled at 3 s by the browser.
type StatusResult struct {
	Profile string    `json:"profile"`
	Kind    string    `json:"kind"`
	World   WorldArgs `json:"world"`

	Running   bool   `json:"running"`
	PID       int    `json:"pid,omitempty"`
	StartedAt string `json:"started_at,omitempty"`
	ProcNote  string `json:"proc_note,omitempty"`

	Players     int    `json:"players"`
	PlayersNote string `json:"players_note,omitempty"` // set when `who` could not be read

	Disk *DiskUsage `json:"disk,omitempty"`

	Region *parse.RegionStats `json:"region,omitempty"`
}

// DiskUsage is the free space on the world's filesystem.
type DiskUsage struct {
	Path       string `json:"path"`
	TotalBytes uint64 `json:"total_bytes"`
	FreeBytes  uint64 `json:"free_bytes"`
	UsedPct    int    `json:"used_pct"`
}

// Status assembles the Status panel for a local profile. Per plan §1 the local
// transport never shells out for information it can read in-process: liveness
// and disk come from the supervisor and syscall.Statfs; only the player count
// needs edenctl.
func (rt *Runtime) Status(ctx context.Context, p profile.Profile) StatusResult {
	res := StatusResult{
		Profile: p.Name,
		Kind:    string(p.Kind),
		World:   parseWorldArgs(p.Args),
	}

	if sup := rt.Supervisor(p); sup != nil {
		st := sup.Status()
		res.Running = st.Running
		res.PID = st.PID
		res.StartedAt = st.StartedAt
		res.ProcNote = st.Detail
	}

	if du, err := statfs(p.WorldDir); err == nil {
		res.Disk = du
	}

	if res.Running {
		if out, err := rt.runCtl(ctx, p, eden.Who(p)); err != nil {
			res.PlayersNote = "could not read `who`: " + err.Error()
		} else if ps, err := parse.Who(out); err != nil {
			res.PlayersNote = err.Error()
		} else {
			res.Players = len(ps)
		}
		if out, err := rt.runCtl(ctx, p, eden.RegionStats(p)); err == nil {
			if rs, err := parse.RegionStatsParse(out); err == nil {
				res.Region = &rs
			}
		}
	}
	return res
}

func statfs(path string) (*DiskUsage, error) {
	var st syscall.Statfs_t
	if err := syscall.Statfs(path, &st); err != nil {
		return nil, err
	}
	bs := uint64(st.Bsize)
	total := st.Blocks * bs
	free := st.Bavail * bs
	du := &DiskUsage{Path: path, TotalBytes: total, FreeBytes: free}
	if total > 0 {
		du.UsedPct = int(float64(total-free) / float64(total) * 100)
	}
	return du, nil
}
