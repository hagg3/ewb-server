package httpd

import (
	"context"
	"net/http"
	"strings"
	"time"

	"github.com/hagg3/ewb-server/admin/internal/panels"
	"github.com/hagg3/ewb-server/admin/internal/profile"
)

// handleConfig: GET renders the typed form + raw source; POST validates, writes
// (the EnvironmentFile via edenserver-writeconf on vps, the profile args on
// local) and optionally does a graceful restart.
func (s *Server) handleConfig(w http.ResponseWriter, r *http.Request) {
	p, exists := s.resolveProfile(r)
	if !exists {
		fail(w, http.StatusBadRequest, "no such profile")
		return
	}

	if r.Method == http.MethodGet {
		ctx, cancel := context.WithTimeout(r.Context(), 20*time.Second)
		defer cancel()
		ok(w, s.rt.Config(ctx, p))
		return
	}

	var body struct {
		panels.ConfigSettings
		Restart bool `json:"restart"`
	}
	if err := decodeBody(r, &body); err != nil {
		fail(w, http.StatusBadRequest, "bad config JSON: "+err.Error())
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 2*time.Minute)
	defer cancel()

	if p.Kind == profile.VPS {
		// Full validation: a vps write rebuilds the whole EnvironmentFile.
		if err := panels.ValidateConfigSettings(body.ConfigSettings); err != nil {
			fail(w, http.StatusBadRequest, err.Error())
			return
		}
		ok(w, s.rt.ConfigWriteVPS(ctx, p, body.ConfigSettings, body.Restart))
		return
	}
	// A local write only touches --port / --name / --max-world-cells in the
	// profile args, so only those need to be sane.
	if err := panels.ValidateLocalConfigSettings(body.ConfigSettings); err != nil {
		fail(w, http.StatusBadRequest, err.Error())
		return
	}
	ok(w, s.writeLocalConfig(ctx, p, body.ConfigSettings, body.Restart))
}

// writeLocalConfig rewrites the managed flags in a local profile's args, under
// the server mutex, then persists and optionally restarts the supervised
// process.
func (s *Server) writeLocalConfig(ctx context.Context, p profile.Profile, cs panels.ConfigSettings, restart bool) panels.ConfigWriteResult {
	var res panels.ConfigWriteResult

	s.mu.Lock()
	np, exists := s.set.Profiles[p.Name]
	if !exists {
		s.mu.Unlock()
		res.Message = "profile " + p.Name + " vanished"
		return res
	}
	np.Name = p.Name
	before := strings.Join(np.Args, " ")
	np.Args = panels.ArgsWithConfigSettings(np.Args, cs)
	after := strings.Join(np.Args, " ")
	s.set.Profiles[p.Name] = np
	err := s.persist()
	s.mu.Unlock()
	if err != nil {
		res.Message = err.Error()
		return res
	}

	res.OK = true
	res.Applied = true
	if before != after {
		res.Diff = []string{"- " + before, "+ " + after}
	}
	res.Message = "profile args updated"
	if restart {
		pr := s.rt.Power(ctx, np, "restart")
		res.Restarted = pr.OK
		res.Message += " — restart: " + pr.Message
		if !pr.OK {
			res.OK = false
		}
	}
	return res
}
