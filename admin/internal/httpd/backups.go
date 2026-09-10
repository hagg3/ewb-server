package httpd

import (
	"context"
	"net/http"
	"time"
)

// handleBackups lists the backups under the profile's backup_dir.
func (s *Server) handleBackups(w http.ResponseWriter, r *http.Request) {
	p, exists := s.resolveProfile(r)
	if !exists {
		fail(w, http.StatusBadRequest, "no such profile")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 30*time.Second)
	defer cancel()
	ok(w, s.rt.Backups(ctx, p))
}

// handleBackupCreate makes a new backup ("Backup now").
func (s *Server) handleBackupCreate(w http.ResponseWriter, r *http.Request) {
	p, exists := s.resolveProfile(r)
	if !exists {
		fail(w, http.StatusBadRequest, "no such profile")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 2*time.Minute)
	defer cancel()
	ok(w, s.rt.BackupCreate(ctx, p))
}

// handleBackupRestore swaps the live world files for those of a backup. The
// structured result carries the step-by-step trace and the ok/failed verdict;
// only a missing profile or an empty stamp is an out-of-band 400.
func (s *Server) handleBackupRestore(w http.ResponseWriter, r *http.Request) {
	p, exists := s.resolveProfile(r)
	if !exists {
		fail(w, http.StatusBadRequest, "no such profile")
		return
	}
	var body struct {
		Stamp string `json:"stamp"`
	}
	if err := decodeBody(r, &body); err != nil {
		fail(w, http.StatusBadRequest, err.Error())
		return
	}
	if body.Stamp == "" {
		fail(w, http.StatusBadRequest, "which backup? `stamp` is required")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 3*time.Minute)
	defer cancel()
	ok(w, s.rt.BackupRestore(ctx, p, body.Stamp))
}
