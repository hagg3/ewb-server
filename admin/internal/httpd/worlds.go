package httpd

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"time"

	"github.com/hagg3/ewb-server/admin/internal/eden"
	"github.com/hagg3/ewb-server/admin/internal/panels"
	"github.com/hagg3/ewb-server/admin/internal/profile"
)

// maxUpload bounds a .eden file or a world bundle. A .eden world is tens of MB;
// this is generous and still keeps a runaway request from exhausting memory.
const maxUpload = 256 << 20

func (s *Server) handleWorlds(w http.ResponseWriter, r *http.Request) {
	p, exists := s.resolveProfile(r)
	if !exists {
		fail(w, http.StatusBadRequest, "no such profile")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 30*time.Second)
	defer cancel()
	ok(w, s.rt.Worlds(ctx, p))
}

// handleWorldBundle downloads (GET) or uploads (POST) a world bundle tar.
func (s *Server) handleWorldBundle(w http.ResponseWriter, r *http.Request) {
	p, exists := s.resolveProfile(r)
	if !exists {
		fail(w, http.StatusBadRequest, "no such profile")
		return
	}
	world := r.URL.Query().Get("world")
	ctx, cancel := context.WithTimeout(r.Context(), 2*time.Minute)
	defer cancel()

	if r.Method == http.MethodPost {
		r.Body = http.MaxBytesReader(w, r.Body, maxUpload)
		if err := r.ParseMultipartForm(16 << 20); err != nil {
			fail(w, http.StatusBadRequest, "bad multipart upload: "+err.Error())
			return
		}
		file, _, err := r.FormFile("bundle")
		if err != nil {
			fail(w, http.StatusBadRequest, "missing `bundle` file field")
			return
		}
		defer file.Close()
		data, err := io.ReadAll(file)
		if err != nil {
			fail(w, http.StatusBadRequest, err.Error())
			return
		}
		if err := s.rt.UploadBundle(ctx, p, world, data); err != nil {
			fail(w, http.StatusBadRequest, err.Error())
			return
		}
		ok(w, map[string]any{"world": world, "bytes": len(data)})
		return
	}

	data, fname, err := s.rt.WorldBundle(ctx, p, world)
	if err != nil {
		fail(w, http.StatusBadRequest, err.Error())
		return
	}
	w.Header().Set("Content-Type", "application/x-tar")
	w.Header().Set("Content-Disposition", `attachment; filename="`+fname+`"`)
	w.Header().Set("Content-Length", strconv.Itoa(len(data)))
	_, _ = w.Write(data)
}

func (s *Server) handleWorldActivate(w http.ResponseWriter, r *http.Request) {
	p, exists := s.resolveProfile(r)
	if !exists {
		fail(w, http.StatusBadRequest, "no such profile")
		return
	}
	var body struct {
		World   string `json:"world"`
		Restart bool   `json:"restart"`
	}
	if err := decodeBody(r, &body); err != nil {
		fail(w, http.StatusBadRequest, err.Error())
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 90*time.Second)
	defer cancel()
	res, err := s.setActiveWorld(ctx, p, body.World, body.Restart)
	if err != nil {
		fail(w, http.StatusBadRequest, err.Error())
		return
	}
	ok(w, res)
}

// handleWorldImport drives eden_import. ?mode=project renders the dry-run
// projection; ?mode=write performs the conversion with the same flags and,
// when set_active=true, chains into the set-active path.
func (s *Server) handleWorldImport(w http.ResponseWriter, r *http.Request) {
	p, exists := s.resolveProfile(r)
	if !exists {
		fail(w, http.StatusBadRequest, "no such profile")
		return
	}
	if p.WorldRoot == "" {
		fail(w, http.StatusBadRequest, "set world_root on the profile before importing")
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, maxUpload)
	if err := r.ParseMultipartForm(16 << 20); err != nil {
		fail(w, http.StatusBadRequest, "bad multipart upload: "+err.Error())
		return
	}
	file, _, err := r.FormFile("eden")
	if err != nil {
		fail(w, http.StatusBadRequest, "missing `eden` file field")
		return
	}
	defer file.Close()
	edenBytes, err := io.ReadAll(file)
	if err != nil {
		fail(w, http.StatusBadRequest, err.Error())
		return
	}

	name := r.FormValue("name")
	if !panels.ValidWorldName(name) {
		fail(w, http.StatusBadRequest, fmt.Sprintf("bad world name %q — letters, digits, . _ - only", name))
		return
	}

	o := eden.NewImportOptions()
	o.OutDir = filepath.Join(p.WorldRoot, name)
	o.AirFill = r.FormValue("air_fill")
	o.Spawn = r.FormValue("spawn")
	o.Force = r.FormValue("force") == "true"
	o.Strict = r.FormValue("strict") == "true"
	o.NoSigns = r.FormValue("no_signs") == "true"
	if v, err := strconv.Atoi(r.FormValue("max_world_cells")); err == nil && v > 0 {
		o.MaxWorldCells = v
	}
	if v, err := strconv.Atoi(r.FormValue("max_region_records")); err == nil && v >= 0 {
		o.MaxRegionRecords = v
	}
	if v, err := strconv.Atoi(r.FormValue("region_radius")); err == nil && v > 0 {
		o.RegionRadius = v
	}

	ctx, cancel := context.WithTimeout(r.Context(), 6*time.Minute)
	defer cancel()

	if r.URL.Query().Get("mode") != "write" {
		ok(w, s.rt.ImportProject(ctx, p, edenBytes, o))
		return
	}

	// A collision must be an explicit operator choice, mirroring eden_import's
	// own overwrite confirm rather than relying on the always-present --yes.
	if !o.Force && p.Kind == profile.Local {
		if _, statErr := os.Stat(filepath.Join(o.OutDir, "eden_world.model")); statErr == nil {
			fail(w, http.StatusConflict, "worlds/"+name+" already exists — tick overwrite to replace it")
			return
		}
	}

	res := s.rt.ImportWrite(ctx, p, edenBytes, o)
	if res.OK && r.FormValue("set_active") == "true" {
		// Size the server cap for the world just imported: on vps the set-active
		// step raises EDEN_MAX_WORLD_CELLS to this when it is higher (plan §3.7).
		// It must leave room *above* the cell count — a cap equal to it refuses
		// every new block players place — so it is the recommended cap for the
		// imported cells, or the acknowledged import override if that is larger.
		maxCells := o.MaxWorldCells
		if res.Summary.Cells > 0 {
			if rec := eden.RecommendedMaxWorldCells(res.Summary.Cells); rec > maxCells {
				maxCells = rec
			}
		}
		sa, saErr := s.setActiveWorldCells(ctx, p, name, r.FormValue("restart") == "true", maxCells)
		if saErr != nil {
			res.SetActive = map[string]any{"ok": false, "error": saErr.Error()}
		} else {
			res.SetActive = sa
		}
	}
	ok(w, res)
}

// setActiveWorld points a profile at a different world. See setActiveWorldCells.
func (s *Server) setActiveWorld(ctx context.Context, p profile.Profile, world string, restart bool) (any, error) {
	return s.setActiveWorldCells(ctx, p, world, restart, 0)
}

// setActiveWorldCells points a profile at worlds/<world>/. For a local profile
// it rewrites `--world` (and `--signs`) in the stored args and moves world_dir,
// then optionally restarts the supervised process. For a vps profile it rewrites
// EDEN_WORLD_DIR in the systemd EnvironmentFile through edenserver-writeconf
// (roadmap 6.6), raising EDEN_MAX_WORLD_CELLS to maxCells when that is higher,
// moves the profile's world_dir pointer so socket / backup / restore follow, and
// restarts. maxCells 0 means "leave the cap alone".
func (s *Server) setActiveWorldCells(ctx context.Context, p profile.Profile, world string, restart bool, maxCells int) (any, error) {
	if !panels.ValidWorldName(world) {
		return nil, fmt.Errorf("bad world name %q", world)
	}
	if p.WorldRoot == "" {
		return nil, errors.New("this profile has no world_root")
	}
	dir := filepath.Join(p.WorldRoot, world)

	if p.Kind == profile.VPS {
		res := s.rt.SetConfWorldDir(ctx, p, dir, maxCells, restart)
		s.mu.Lock()
		if np, exists := s.set.Profiles[p.Name]; exists {
			np.Name = p.Name
			np.WorldDir = dir
			s.set.Profiles[p.Name] = np
			_ = s.persist()
		}
		s.mu.Unlock()
		out := map[string]any{
			"ok": res.OK, "world": world, "world_dir": dir,
			"restarted": res.Restarted, "restart_message": res.Message, "diff": res.Diff,
		}
		if !res.OK {
			return out, errors.New(res.Message)
		}
		return out, nil
	}
	if _, err := os.Stat(filepath.Join(dir, "eden_world.model")); err != nil {
		return nil, fmt.Errorf("%s has no eden_world.model", dir)
	}
	hasSigns := false
	if fi, err := os.Stat(filepath.Join(dir, "eden_signs.txt")); err == nil && fi.Mode().IsRegular() {
		hasSigns = true
	}

	s.mu.Lock()
	np, exists := s.set.Profiles[p.Name]
	if !exists {
		s.mu.Unlock()
		return nil, fmt.Errorf("profile %q vanished", p.Name)
	}
	np.Name = p.Name
	np.Args = panels.WithActiveWorld(np.Args, dir, hasSigns)
	np.WorldDir = dir
	s.set.Profiles[p.Name] = np
	err := s.persist()
	s.mu.Unlock()
	if err != nil {
		return nil, err
	}

	out := map[string]any{
		"ok":        true,
		"world":     world,
		"world_dir": dir,
		"args":      np.Args,
		"restarted": false,
	}
	if restart {
		pr := s.rt.Power(ctx, np, "restart")
		out["restarted"] = pr.OK
		out["restart_message"] = pr.Message
	}
	return out, nil
}
