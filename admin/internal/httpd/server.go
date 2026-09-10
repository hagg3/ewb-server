package httpd

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"sync"
	"time"

	"github.com/hagg3/ewb-server/admin/internal/panels"
	"github.com/hagg3/ewb-server/admin/internal/profile"
)

// Server is the loopback HTTP shell. It owns the profile set (guarded by a
// mutex) and the per-run token.
type Server struct {
	Token       string
	ProfilePath string

	mu  sync.Mutex
	set profile.Set

	rt *panels.Runtime

	httpSrv *http.Server
	ln      net.Listener
}

// New builds a Server bound to 127.0.0.1:port (port 0 => an ephemeral port).
// The returned Server is not yet serving; call Serve.
func New(port int, profilePath string) (*Server, error) {
	set, err := profile.Load(profilePath)
	if err != nil {
		return nil, err
	}
	ln, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", port))
	if err != nil {
		return nil, err
	}
	s := &Server{
		Token:       NewToken(),
		ProfilePath: profilePath,
		set:         set,
		rt:          panels.NewRuntime(),
		ln:          ln,
	}
	h, err := s.handler()
	if err != nil {
		ln.Close()
		return nil, err
	}
	s.httpSrv = &http.Server{
		Handler:           h,
		ReadHeaderTimeout: 5 * time.Second,
	}
	return s, nil
}

// Addr is the bound address, e.g. "127.0.0.1:52001".
func (s *Server) Addr() string { return s.ln.Addr().String() }

// URL is the launch URL including the token fragment.
func (s *Server) URL() string {
	return "http://" + s.Addr() + "/#t=" + s.Token
}

// Serve blocks until the context is cancelled, then shuts down cleanly.
func (s *Server) Serve(ctx context.Context) error {
	errCh := make(chan error, 1)
	go func() { errCh <- s.httpSrv.Serve(s.ln) }()
	select {
	case <-ctx.Done():
		shutCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		_ = s.httpSrv.Shutdown(shutCtx)
		return nil
	case err := <-errCh:
		if err == http.ErrServerClosed {
			return nil
		}
		return err
	}
}

// registerRoutes wires every route onto mux. It is shared by the live handler
// and the test handler so the two can never drift.
func (s *Server) registerRoutes(mux *http.ServeMux, assets http.Handler) {
	mux.Handle("/", assets)
	mux.HandleFunc("/api/state", s.handleState)
	mux.HandleFunc("/api/profiles/save", s.handleProfileSave)
	mux.HandleFunc("/api/profiles/activate", s.handleProfileActivate)
	mux.HandleFunc("/api/profiles/delete", s.handleProfileDelete)
	mux.HandleFunc("/api/connection/test", s.handleConnectionTest)
	mux.HandleFunc("/api/status", s.handleStatus)
	mux.HandleFunc("/api/players", s.handlePlayers)
	mux.HandleFunc("/api/logs", s.handleLogs)
	mux.HandleFunc("/api/audit", s.handleAudit)
	mux.HandleFunc("/api/power", s.handlePower)
	mux.HandleFunc("/api/players/action", s.handlePlayerAction)
	mux.HandleFunc("/api/bans", s.handleBans)
	mux.HandleFunc("/api/worlds", s.handleWorlds)
	mux.HandleFunc("/api/worlds/bundle", s.handleWorldBundle)
	mux.HandleFunc("/api/worlds/activate", s.handleWorldActivate)
	mux.HandleFunc("/api/worlds/import", s.handleWorldImport)
	mux.HandleFunc("/api/config", s.handleConfig)
	mux.HandleFunc("/api/backups", s.handleBackups)
	mux.HandleFunc("/api/backups/create", s.handleBackupCreate)
	mux.HandleFunc("/api/backups/restore", s.handleBackupRestore)
}

func (s *Server) handler() (http.Handler, error) {
	assets, err := assetsHandler()
	if err != nil {
		return nil, err
	}
	mux := http.NewServeMux()
	s.registerRoutes(mux, assets)

	_, port, _ := net.SplitHostPort(s.Addr())
	allowed := map[string]bool{
		"127.0.0.1:" + port: true,
		"localhost:" + port: true,
	}
	g := &guard{
		token:    s.Token,
		allowed:  allowed,
		next:     mux,
		openPath: isAssetPath,
	}
	return g, nil
}

// --- envelope ---

type envelope struct {
	OK    bool   `json:"ok"`
	Data  any    `json:"data,omitempty"`
	Error string `json:"error,omitempty"`
}

func writeJSON(w http.ResponseWriter, status int, e envelope) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(e)
}

func ok(w http.ResponseWriter, data any) { writeJSON(w, http.StatusOK, envelope{OK: true, Data: data}) }
func fail(w http.ResponseWriter, code int, msg string) {
	writeJSON(w, code, envelope{OK: false, Error: msg})
}

func decodeBody(r *http.Request, v any) error {
	dec := json.NewDecoder(http.MaxBytesReader(nil, r.Body, 1<<20))
	dec.DisallowUnknownFields()
	return dec.Decode(v)
}

// --- handlers ---

type stateReply struct {
	Active   string                     `json:"active"`
	Profiles map[string]profile.Profile `json:"profiles"`
}

func (s *Server) handleState(w http.ResponseWriter, r *http.Request) {
	s.mu.Lock()
	defer s.mu.Unlock()
	ok(w, stateReply{Active: s.set.Active, Profiles: s.set.Profiles})
}

func (s *Server) handleProfileSave(w http.ResponseWriter, r *http.Request) {
	var p profile.Profile
	if err := decodeBody(r, &p); err != nil {
		fail(w, http.StatusBadRequest, "bad profile JSON: "+err.Error())
		return
	}
	if err := p.Validate(); err != nil {
		fail(w, http.StatusBadRequest, err.Error())
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.set.Profiles == nil {
		s.set.Profiles = map[string]profile.Profile{}
	}
	s.set.Profiles[p.Name] = p
	if s.set.Active == "" {
		s.set.Active = p.Name
	}
	if err := s.persist(); err != nil {
		fail(w, http.StatusInternalServerError, err.Error())
		return
	}
	ok(w, stateReply{Active: s.set.Active, Profiles: s.set.Profiles})
}

func (s *Server) handleProfileActivate(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Name string `json:"name"`
	}
	if err := decodeBody(r, &body); err != nil {
		fail(w, http.StatusBadRequest, err.Error())
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, ok2 := s.set.Profiles[body.Name]; !ok2 {
		fail(w, http.StatusBadRequest, "no such profile: "+body.Name)
		return
	}
	s.set.Active = body.Name
	if err := s.persist(); err != nil {
		fail(w, http.StatusInternalServerError, err.Error())
		return
	}
	ok(w, stateReply{Active: s.set.Active, Profiles: s.set.Profiles})
}

func (s *Server) handleProfileDelete(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Name string `json:"name"`
	}
	if err := decodeBody(r, &body); err != nil {
		fail(w, http.StatusBadRequest, err.Error())
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, ok2 := s.set.Profiles[body.Name]; !ok2 {
		fail(w, http.StatusBadRequest, "no such profile: "+body.Name)
		return
	}
	delete(s.set.Profiles, body.Name)
	if s.set.Active == body.Name {
		s.set.Active = ""
		for n := range s.set.Profiles {
			s.set.Active = n
			break
		}
	}
	if err := s.persist(); err != nil {
		fail(w, http.StatusInternalServerError, err.Error())
		return
	}
	ok(w, stateReply{Active: s.set.Active, Profiles: s.set.Profiles})
}

func (s *Server) handleConnectionTest(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Name string `json:"name"`
	}
	if err := decodeBody(r, &body); err != nil {
		fail(w, http.StatusBadRequest, err.Error())
		return
	}
	s.mu.Lock()
	p, exists := s.set.Profiles[body.Name]
	s.mu.Unlock()
	if !exists {
		fail(w, http.StatusBadRequest, "no such profile: "+body.Name)
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 30*time.Second)
	defer cancel()
	ok(w, panels.TestConnection(ctx, p))
}

// resolveProfile picks the profile named by ?profile=, or the active one when
// that is blank.
func (s *Server) resolveProfile(r *http.Request) (profile.Profile, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	name := r.URL.Query().Get("profile")
	if name == "" {
		name = s.set.Active
	}
	p, ok := s.set.Profiles[name]
	p.Name = name
	return p, ok
}

func (s *Server) handleStatus(w http.ResponseWriter, r *http.Request) {
	p, exists := s.resolveProfile(r)
	if !exists {
		fail(w, http.StatusBadRequest, "no such profile")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 15*time.Second)
	defer cancel()
	ok(w, s.rt.Status(ctx, p))
}

func (s *Server) handlePlayers(w http.ResponseWriter, r *http.Request) {
	p, exists := s.resolveProfile(r)
	if !exists {
		fail(w, http.StatusBadRequest, "no such profile")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 15*time.Second)
	defer cancel()
	ok(w, s.rt.Players(ctx, p))
}

func (s *Server) handleLogs(w http.ResponseWriter, r *http.Request) {
	p, exists := s.resolveProfile(r)
	if !exists {
		fail(w, http.StatusBadRequest, "no such profile")
		return
	}
	fromStart := r.URL.Query().Get("from") == "start"
	res, err := s.rt.Logs(p, fromStart)
	if err != nil {
		fail(w, http.StatusInternalServerError, err.Error())
		return
	}
	ok(w, res)
}

func (s *Server) handleAudit(w http.ResponseWriter, r *http.Request) {
	p, exists := s.resolveProfile(r)
	if !exists {
		fail(w, http.StatusBadRequest, "no such profile")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 30*time.Second)
	defer cancel()
	ok(w, s.rt.Audit(ctx, p))
}

// --- write actions (stage 6.3) ---

func (s *Server) handlePower(w http.ResponseWriter, r *http.Request) {
	p, exists := s.resolveProfile(r)
	if !exists {
		fail(w, http.StatusBadRequest, "no such profile")
		return
	}
	var body struct {
		Action string `json:"action"`
	}
	if err := decodeBody(r, &body); err != nil {
		fail(w, http.StatusBadRequest, err.Error())
		return
	}
	if !panels.PowerActions[body.Action] {
		fail(w, http.StatusBadRequest, "action must be start | stop | restart | kill")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 90*time.Second)
	defer cancel()
	ok(w, s.rt.Power(ctx, p, body.Action))
}

func (s *Server) handlePlayerAction(w http.ResponseWriter, r *http.Request) {
	p, exists := s.resolveProfile(r)
	if !exists {
		fail(w, http.StatusBadRequest, "no such profile")
		return
	}
	var body panels.PlayerActionReq
	if err := decodeBody(r, &body); err != nil {
		fail(w, http.StatusBadRequest, err.Error())
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 15*time.Second)
	defer cancel()
	ok(w, s.rt.PlayerAction(ctx, p, body))
}

func (s *Server) handleBans(w http.ResponseWriter, r *http.Request) {
	p, exists := s.resolveProfile(r)
	if !exists {
		fail(w, http.StatusBadRequest, "no such profile")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 15*time.Second)
	defer cancel()
	if r.Method == http.MethodGet {
		ok(w, s.rt.Bans(ctx, p))
		return
	}
	var body struct {
		Action string `json:"action"`
		Token  string `json:"token"`
	}
	if err := decodeBody(r, &body); err != nil {
		fail(w, http.StatusBadRequest, err.Error())
		return
	}
	var verb string
	switch body.Action {
	case "add":
		verb = "ban"
	case "remove":
		verb = "unban"
	default:
		fail(w, http.StatusBadRequest, "action must be add | remove")
		return
	}
	ok(w, s.rt.PlayerAction(ctx, p, panels.PlayerActionReq{Verb: verb, Name: body.Token}))
}

// persist writes the current set to disk. Caller holds s.mu.
func (s *Server) persist() error {
	if s.ProfilePath == "" {
		return nil // in-memory only (tests)
	}
	return profile.Save(s.ProfilePath, s.set)
}
