package httpd

import (
	"archive/tar"
	"bytes"
	"context"
	"encoding/json"
	"io"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/hagg3/ewb-server/admin/internal/panels"
)

// newTestServer builds a Server whose handler we can drive with httptest,
// backed by a profiles.toml under a temp dir.
func newTestServer(t *testing.T) (*Server, http.Handler) {
	t.Helper()
	dir := t.TempDir()
	s := &Server{
		Token:       "test-token",
		ProfilePath: filepath.Join(dir, "profiles.toml"),
		rt:          panels.NewRuntime(),
	}
	h, err := s.handlerWithAllowed(map[string]bool{"127.0.0.1:7777": true, "localhost:7777": true})
	if err != nil {
		t.Fatal(err)
	}
	return s, h
}

// handlerWithAllowed mirrors handler() but with a fixed allowlist for tests
// (the real one derives the port from the live listener).
func (s *Server) handlerWithAllowed(allowed map[string]bool) (http.Handler, error) {
	assets, err := assetsHandler()
	if err != nil {
		return nil, err
	}
	mux := http.NewServeMux()
	s.registerRoutes(mux, assets)
	return &guard{token: s.Token, allowed: allowed, next: mux, openPath: isAssetPath}, nil
}

func do(t *testing.T, h http.Handler, method, path string, body any) (*httptest.ResponseRecorder, map[string]any) {
	t.Helper()
	var rdr io.Reader
	if body != nil {
		b, _ := json.Marshal(body)
		rdr = bytes.NewReader(b)
	}
	r := httptest.NewRequest(method, "http://127.0.0.1:7777"+path, rdr)
	r.RemoteAddr = "127.0.0.1:40000"
	r.Host = "127.0.0.1:7777"
	r.Header.Set("X-Edenadmin-Token", "test-token")
	if body != nil {
		r.Header.Set("Content-Type", "application/json")
	}
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	var env map[string]any
	if w.Body.Len() > 0 {
		_ = json.Unmarshal(w.Body.Bytes(), &env)
	}
	return w, env
}

func TestServerServesIndex(t *testing.T) {
	_, h := newTestServer(t)
	r := httptest.NewRequest(http.MethodGet, "http://127.0.0.1:7777/", nil)
	r.RemoteAddr = "127.0.0.1:40000"
	r.Host = "127.0.0.1:7777"
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	if w.Code != 200 || !bytes.Contains(w.Body.Bytes(), []byte("edenadmin")) {
		t.Fatalf("index not served: %d", w.Code)
	}
}

func TestProfileCRUDFlow(t *testing.T) {
	s, h := newTestServer(t)
	dir := t.TempDir()

	// Save a valid local profile. An explicit short socket: a t.TempDir() path
	// pushes <world_dir>/edenserver.sock past the 103-byte sun_path limit.
	p := map[string]any{
		"name": "dev", "kind": "local",
		"world_dir": dir, "world_root": dir,
		"socket": "/tmp/edenadmin-crud.sock",
	}
	w, env := do(t, h, http.MethodPost, "/api/profiles/save", p)
	if w.Code != 200 || env["ok"] != true {
		t.Fatalf("save failed: %d %v", w.Code, env)
	}
	// First profile becomes active automatically.
	if s.set.Active != "dev" {
		t.Fatalf("active = %q, want dev", s.set.Active)
	}
	// It persisted to disk.
	if _, err := os.Stat(s.ProfilePath); err != nil {
		t.Fatalf("profiles.toml not written: %v", err)
	}

	// An invalid profile is rejected.
	bad := map[string]any{"name": "x", "kind": "local", "world_dir": "relative"}
	w, _ = do(t, h, http.MethodPost, "/api/profiles/save", bad)
	if w.Code != http.StatusBadRequest {
		t.Fatalf("bad profile: code %d, want 400", w.Code)
	}

	// Add a second, then activate it.
	p2 := map[string]any{"name": "prod", "kind": "vps", "ssh_host": "eden-vps", "run_as": "edenserver", "world_dir": "/var/lib/edenserver/world"}
	do(t, h, http.MethodPost, "/api/profiles/save", p2)
	w, _ = do(t, h, http.MethodPost, "/api/profiles/activate", map[string]any{"name": "prod"})
	if w.Code != 200 || s.set.Active != "prod" {
		t.Fatalf("activate: %d active=%q", w.Code, s.set.Active)
	}

	// Delete the active one; active falls back to the remaining profile.
	w, _ = do(t, h, http.MethodPost, "/api/profiles/delete", map[string]any{"name": "prod"})
	if w.Code != 200 || s.set.Active != "dev" {
		t.Fatalf("delete/fallback: %d active=%q", w.Code, s.set.Active)
	}

	// state reflects it all.
	_, env = do(t, h, http.MethodGet, "/api/state", nil)
	profs := env["data"].(map[string]any)["profiles"].(map[string]any)
	if _, ok := profs["prod"]; ok {
		t.Fatal("prod still present after delete")
	}
}

func TestWriteActionRoutesGuardedAndShaped(t *testing.T) {
	_, h := newTestServer(t)
	dir := t.TempDir()
	p := map[string]any{
		"name": "dev", "kind": "local",
		"world_dir": dir, "world_root": dir,
		"socket": "/tmp/edenadmin-wa.sock",
	}
	if w, env := do(t, h, http.MethodPost, "/api/profiles/save", p); w.Code != 200 || env["ok"] != true {
		t.Fatalf("save: %d %v", w.Code, env)
	}

	// A GET on a write route is rejected as method-not-allowed (CSRF surface).
	if w, _ := do(t, h, http.MethodGet, "/api/power?profile=dev", nil); w.Code != http.StatusMethodNotAllowed {
		t.Fatalf("GET /api/power = %d, want 405", w.Code)
	}
	if w, _ := do(t, h, http.MethodGet, "/api/players/action?profile=dev", nil); w.Code != http.StatusMethodNotAllowed {
		t.Fatalf("GET /api/players/action = %d, want 405", w.Code)
	}

	// An unknown power action is a 400, not a spawn attempt.
	if w, _ := do(t, h, http.MethodPost, "/api/power?profile=dev", map[string]any{"action": "vaporize"}); w.Code != http.StatusBadRequest {
		t.Fatalf("bad power action = %d, want 400", w.Code)
	}

	// A player action against a stopped server comes back as a clean rejection.
	w, env := do(t, h, http.MethodPost, "/api/players/action?profile=dev", map[string]any{"verb": "say", "text": "hi"})
	if w.Code != 200 || env["ok"] != true {
		t.Fatalf("players/action envelope: %d %v", w.Code, env)
	}
	data := env["data"].(map[string]any)
	if data["ok"] != false || data["kind"] != "rejected" {
		t.Fatalf("expected a rejection for a stopped server: %v", data)
	}

	// GET /api/bans is read-only and returns the offline note.
	w, env = do(t, h, http.MethodGet, "/api/bans?profile=dev", nil)
	if w.Code != 200 || env["ok"] != true {
		t.Fatalf("bans: %d %v", w.Code, env)
	}
	if env["data"].(map[string]any)["note"] == nil {
		t.Errorf("expected an offline note: %v", env["data"])
	}
}

func TestConnectionTestLocalProfile(t *testing.T) {
	_, h := newTestServer(t)
	dir := t.TempDir()
	// A world dir and an executable stand-in for eden_import / edenctl.
	binDir := filepath.Join(dir, "bin")
	os.MkdirAll(binDir, 0o755)
	for _, name := range []string{"edenserver", "edenctl", "eden_import"} {
		p := filepath.Join(binDir, name)
		os.WriteFile(p, []byte("#!/bin/sh\n"), 0o755)
	}
	worldDir := filepath.Join(dir, "worlds", "dev")
	os.MkdirAll(worldDir, 0o755)

	prof := map[string]any{
		"name": "dev", "kind": "local",
		"world_dir": worldDir, "world_root": filepath.Join(dir, "worlds"),
		"server_bin": filepath.Join(binDir, "edenserver"),
		"socket":     "/tmp/edenadmin-conn.sock",
	}
	do(t, h, http.MethodPost, "/api/profiles/save", prof)

	w, env := do(t, h, http.MethodPost, "/api/connection/test", map[string]any{"name": "dev"})
	if w.Code != 200 {
		t.Fatalf("code %d", w.Code)
	}
	data := env["data"].(map[string]any)
	// world dir + eden_import + edenctl all resolve => overall OK despite the
	// (soft) missing control socket.
	if data["ok"] != true {
		probes, _ := json.Marshal(data["probes"])
		t.Fatalf("expected overall OK, got probes: %s", probes)
	}
}

// --- stage 6.4: Worlds + .eden import ---

func mkWorld(t *testing.T, root, name string) {
	t.Helper()
	d := filepath.Join(root, name)
	if err := os.MkdirAll(d, 0o755); err != nil {
		t.Fatal(err)
	}
	os.WriteFile(filepath.Join(d, "eden_world.model"), []byte("65536:33:65536:1:0\n"), 0o644)
	os.WriteFile(filepath.Join(d, ".gitignore"), []byte("*\n!.gitignore\n"), 0o644)
}

func doMultipart(t *testing.T, h http.Handler, path string, fields map[string]string, fileField, fileName string, fileData []byte) (*httptest.ResponseRecorder, map[string]any) {
	t.Helper()
	var buf bytes.Buffer
	mw := multipart.NewWriter(&buf)
	for k, v := range fields {
		_ = mw.WriteField(k, v)
	}
	if fileField != "" {
		fw, _ := mw.CreateFormFile(fileField, fileName)
		fw.Write(fileData)
	}
	mw.Close()

	r := httptest.NewRequest(http.MethodPost, "http://127.0.0.1:7777"+path, &buf)
	r.RemoteAddr = "127.0.0.1:40000"
	r.Host = "127.0.0.1:7777"
	r.Header.Set("X-Edenadmin-Token", "test-token")
	r.Header.Set("Content-Type", mw.FormDataContentType())
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	var env map[string]any
	if w.Body.Len() > 0 {
		_ = json.Unmarshal(w.Body.Bytes(), &env)
	}
	return w, env
}

func TestWorldsListAndSetActive(t *testing.T) {
	s, h := newTestServer(t)
	root := t.TempDir()
	mkWorld(t, root, "alpha")
	mkWorld(t, root, "beta")

	p := map[string]any{
		"name": "dev", "kind": "local",
		"world_dir": filepath.Join(root, "alpha"), "world_root": root,
		"socket": "/tmp/edenadmin-w6.sock",
		"args":   []string{"--world", filepath.Join(root, "alpha", "eden_world.model"), "--name", "A"},
	}
	if w, env := do(t, h, http.MethodPost, "/api/profiles/save", p); w.Code != 200 || env["ok"] != true {
		t.Fatalf("save: %d %v", w.Code, env)
	}

	// List: two worlds, alpha active.
	_, env := do(t, h, http.MethodGet, "/api/worlds?profile=dev", nil)
	worlds := env["data"].(map[string]any)["worlds"].([]any)
	if len(worlds) != 2 {
		t.Fatalf("want 2 worlds, got %v", worlds)
	}

	// A GET on the activate route is method-not-allowed.
	if w, _ := do(t, h, http.MethodGet, "/api/worlds/activate?profile=dev", nil); w.Code != http.StatusMethodNotAllowed {
		t.Fatalf("GET /api/worlds/activate = %d, want 405", w.Code)
	}

	// Activate beta.
	w, env := do(t, h, http.MethodPost, "/api/worlds/activate?profile=dev", map[string]any{"world": "beta"})
	if w.Code != 200 || env["ok"] != true {
		t.Fatalf("activate: %d %v", w.Code, env)
	}

	// The profile's args and world_dir moved and persisted.
	_, env = do(t, h, http.MethodGet, "/api/state", nil)
	prof := env["data"].(map[string]any)["profiles"].(map[string]any)["dev"].(map[string]any)
	if prof["world_dir"] != filepath.Join(root, "beta") {
		t.Errorf("world_dir = %v, want .../beta", prof["world_dir"])
	}
	args := prof["args"].([]any)
	joined := ""
	for _, a := range args {
		joined += a.(string) + " "
	}
	if !strings.Contains(joined, filepath.Join(root, "beta", "eden_world.model")) || !strings.Contains(joined, "--name A") {
		t.Errorf("args not rewritten cleanly: %q", joined)
	}

	// Bundle download for beta is a tar containing .gitignore.
	r := httptest.NewRequest(http.MethodGet, "http://127.0.0.1:7777/api/worlds/bundle?profile=dev&world=beta", nil)
	r.RemoteAddr = "127.0.0.1:40000"
	r.Host = "127.0.0.1:7777"
	r.Header.Set("X-Edenadmin-Token", "test-token")
	wr := httptest.NewRecorder()
	h.ServeHTTP(wr, r)
	if wr.Code != 200 || wr.Header().Get("Content-Type") != "application/x-tar" {
		t.Fatalf("bundle download: %d %q", wr.Code, wr.Header().Get("Content-Type"))
	}
	sawGitignore := false
	tr := tar.NewReader(bytes.NewReader(wr.Body.Bytes()))
	for {
		hd, err := tr.Next()
		if err != nil {
			break
		}
		if hd.Name == ".gitignore" {
			sawGitignore = true
		}
	}
	if !sawGitignore {
		t.Error("bundle tar did not carry .gitignore")
	}
	_ = s
}

func TestWorldImportWriteChainsSetActive(t *testing.T) {
	_, h := newTestServer(t)
	root := t.TempDir()
	binDir := t.TempDir()
	imp := filepath.Join(binDir, "eden_import")
	stub := "#!/bin/sh\n" +
		"out=\"\"; prev=\"\"\n" +
		"for a in \"$@\"; do [ \"$prev\" = \"--out\" ] && out=\"$a\"; prev=\"$a\"; done\n" +
		"mkdir -p \"$out\"\n" +
		"printf '65536:33:65536:1:0\\n' > \"$out/eden_world.model\"\n" +
		"printf '*\\n!.gitignore\\n' > \"$out/.gitignore\"\n" +
		"cat <<EOF\n" +
		"world \"Stub\"  (version 4, 64z, 1 saved chunks, 0 signs from none)\n" +
		"  cells         1  of 4,000,000 cap  (0.0%)\n" +
		"  spawn         65536.00, 33.92, 65536.00  (header)\n" +
		"EOF\n" +
		"exit 0\n"
	if err := os.WriteFile(imp, []byte(stub), 0o755); err != nil {
		t.Fatal(err)
	}

	mkWorld(t, root, "orig")
	p := map[string]any{
		"name": "dev", "kind": "local",
		"world_dir": filepath.Join(root, "orig"), "world_root": root,
		"eden_import": imp, "socket": "/tmp/edenadmin-imp6.sock",
		"args": []string{"--world", filepath.Join(root, "orig", "eden_world.model")},
	}
	if w, env := do(t, h, http.MethodPost, "/api/profiles/save", p); w.Code != 200 || env["ok"] != true {
		t.Fatalf("save: %d %v", w.Code, env)
	}

	w, env := doMultipart(t, h, "/api/worlds/import?profile=dev&mode=write",
		map[string]string{"name": "fresh", "set_active": "true"},
		"eden", "fresh.eden", []byte("PRETEND-EDEN"))
	if w.Code != 200 || env["ok"] != true {
		t.Fatalf("import write: %d %v", w.Code, env)
	}
	data := env["data"].(map[string]any)
	if data["ok"] != true {
		t.Fatalf("import result not ok: %v", data)
	}
	if _, err := os.Stat(filepath.Join(root, "fresh", "eden_world.model")); err != nil {
		t.Fatalf("import did not write the world: %v", err)
	}
	sa, isMap := data["set_active"].(map[string]any)
	if !isMap || sa["world"] != "fresh" {
		t.Fatalf("set_active did not chain: %v", data["set_active"])
	}

	// Profile now points at the imported world.
	_, env = do(t, h, http.MethodGet, "/api/state", nil)
	prof := env["data"].(map[string]any)["profiles"].(map[string]any)["dev"].(map[string]any)
	if prof["world_dir"] != filepath.Join(root, "fresh") {
		t.Errorf("world_dir = %v, want .../fresh", prof["world_dir"])
	}
}

func TestBackupRoutesGuardedAndShaped(t *testing.T) {
	_, h := newTestServer(t)
	root := t.TempDir()
	world := filepath.Join(root, "world")
	if err := os.MkdirAll(world, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(world, "eden_world.model"), []byte("x\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	p := map[string]any{
		"name": "dev", "kind": "local",
		"world_dir": world, "backup_dir": filepath.Join(root, "backups"),
		"socket": "/tmp/edenadmin-bk.sock",
	}
	if w, env := do(t, h, http.MethodPost, "/api/profiles/save", p); w.Code != 200 || env["ok"] != true {
		t.Fatalf("save: %d %v", w.Code, env)
	}

	// GET lists (read-only, allowlisted); empty at first.
	w, env := do(t, h, http.MethodGet, "/api/backups?profile=dev", nil)
	if w.Code != 200 || env["ok"] != true {
		t.Fatalf("GET /api/backups: %d %v", w.Code, env)
	}

	// GET on a mutating route is method-not-allowed.
	if w, _ := do(t, h, http.MethodGet, "/api/backups/create?profile=dev", nil); w.Code != http.StatusMethodNotAllowed {
		t.Fatalf("GET /api/backups/create = %d, want 405", w.Code)
	}

	// Create makes a backup and it shows up in the listing.
	w, env = do(t, h, http.MethodPost, "/api/backups/create?profile=dev", map[string]any{})
	if w.Code != 200 || env["data"].(map[string]any)["ok"] != true {
		t.Fatalf("create: %d %v", w.Code, env)
	}
	stamp := env["data"].(map[string]any)["stamp"].(string)

	w, env = do(t, h, http.MethodGet, "/api/backups?profile=dev", nil)
	list := env["data"].(map[string]any)["backups"].([]any)
	if len(list) != 1 || list[0].(map[string]any)["stamp"] != stamp {
		t.Fatalf("backup not listed after create: %v", env["data"])
	}

	// Restore with no stamp is a 400.
	if w, _ := do(t, h, http.MethodPost, "/api/backups/restore?profile=dev", map[string]any{}); w.Code != http.StatusBadRequest {
		t.Fatalf("restore without a stamp = %d, want 400", w.Code)
	}

	// Restore of the just-made backup succeeds (server not running).
	w, env = do(t, h, http.MethodPost, "/api/backups/restore?profile=dev", map[string]any{"stamp": stamp})
	if w.Code != 200 || env["data"].(map[string]any)["ok"] != true {
		t.Fatalf("restore: %d %v", w.Code, env)
	}
}

// --- stage 6.6: Config ---

func TestConfigRoutesGuardedAndShaped(t *testing.T) {
	s, h := newTestServer(t)
	dir := t.TempDir()
	p := map[string]any{
		"name": "dev", "kind": "local",
		"world_dir": dir, "world_root": dir,
		"socket": "/tmp/edenadmin-cfg6.sock",
		"args":   []string{"--port", "27015", "--name", "Dev", "--matchmaker", "h:1"},
	}
	if w, env := do(t, h, http.MethodPost, "/api/profiles/save", p); w.Code != 200 || env["ok"] != true {
		t.Fatalf("save: %d %v", w.Code, env)
	}

	// GET renders the typed form (allowlisted read).
	w, env := do(t, h, http.MethodGet, "/api/config?profile=dev", nil)
	if w.Code != 200 || env["ok"] != true {
		t.Fatalf("GET /api/config: %d %v", w.Code, env)
	}
	settings := env["data"].(map[string]any)["settings"].(map[string]any)
	if settings["port"].(float64) != 27015 || settings["name"] != "Dev" {
		t.Fatalf("settings not surfaced: %v", settings)
	}

	// An out-of-range port is a 400, not a write.
	if w, _ := do(t, h, http.MethodPost, "/api/config?profile=dev", map[string]any{
		"port": 0, "name": "Dev", "world_dir": "/w", "max_world_cells": 1,
	}); w.Code != http.StatusBadRequest {
		t.Fatalf("bad port = %d, want 400", w.Code)
	}

	// A valid POST rewrites the managed flags in the profile args and persists.
	w, env = do(t, h, http.MethodPost, "/api/config?profile=dev", map[string]any{
		"port": 27099, "name": "Dev", "world_dir": dir, "max_world_cells": 6000000, "restart": false,
	})
	if w.Code != 200 || env["data"].(map[string]any)["ok"] != true {
		t.Fatalf("config write: %d %v", w.Code, env)
	}
	_, env = do(t, h, http.MethodGet, "/api/state", nil)
	args := env["data"].(map[string]any)["profiles"].(map[string]any)["dev"].(map[string]any)["args"].([]any)
	joined := ""
	for _, a := range args {
		joined += a.(string) + " "
	}
	if !strings.Contains(joined, "--port 27099") || !strings.Contains(joined, "--max-world-cells 6000000") ||
		!strings.Contains(joined, "--matchmaker h:1") {
		t.Fatalf("args not rewritten: %q", joined)
	}
	_ = s
}

func TestServeShutsDownOnContextCancel(t *testing.T) {
	dir := t.TempDir()
	s, err := New(0, filepath.Join(dir, "profiles.toml"))
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- s.Serve(ctx) }()
	time.Sleep(20 * time.Millisecond)
	cancel()
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("Serve returned %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("Serve did not return after cancel")
	}
}

func TestReadOnlyRoutesGuardedAndShaped(t *testing.T) {
	_, h := newTestServer(t)
	dir := t.TempDir()
	p := map[string]any{
		"name": "dev", "kind": "local",
		"world_dir": dir, "world_root": dir,
		"socket": "/tmp/edenadmin-ro.sock",
		"args":   []string{"27099", "--name", "RO World"},
	}
	if w, env := do(t, h, http.MethodPost, "/api/profiles/save", p); w.Code != 200 || env["ok"] != true {
		t.Fatalf("save: %d %v", w.Code, env)
	}

	// A missing token is rejected before the handler runs.
	r := httptest.NewRequest(http.MethodGet, "http://127.0.0.1:7777/api/status?profile=dev", nil)
	r.RemoteAddr = "127.0.0.1:40000"
	r.Host = "127.0.0.1:7777"
	wNoTok := httptest.NewRecorder()
	h.ServeHTTP(wNoTok, r)
	if wNoTok.Code != http.StatusUnauthorized {
		t.Fatalf("status without token = %d, want 401", wNoTok.Code)
	}

	w, env := do(t, h, http.MethodGet, "/api/status?profile=dev", nil)
	if w.Code != 200 || env["ok"] != true {
		t.Fatalf("status: %d %v", w.Code, env)
	}
	data := env["data"].(map[string]any)
	if data["running"] != false {
		t.Errorf("expected running=false with no supervised process: %v", data)
	}
	world := data["world"].(map[string]any)
	if world["port"].(float64) != 27099 || world["name"] != "RO World" {
		t.Errorf("world args not surfaced: %v", world)
	}

	w, env = do(t, h, http.MethodGet, "/api/players?profile=dev", nil)
	if w.Code != 200 || env["ok"] != true {
		t.Fatalf("players: %d %v", w.Code, env)
	}

	w, env = do(t, h, http.MethodGet, "/api/logs?profile=dev&from=start", nil)
	if w.Code != 200 || env["ok"] != true {
		t.Fatalf("logs: %d %v", w.Code, env)
	}
	if env["data"].(map[string]any)["missing"] != true {
		t.Errorf("expected missing log file: %v", env["data"])
	}

	w, env = do(t, h, http.MethodGet, "/api/audit?profile=dev", nil)
	if w.Code != 200 || env["ok"] != true {
		t.Fatalf("audit: %d %v", w.Code, env)
	}
	data = env["data"].(map[string]any)
	// No --audit-file in the args and no log yet: the fallback source with an
	// explanatory note, never a blank panel.
	if data["fallback"] != true || data["enabled"] != false || data["note"] == "" {
		t.Errorf("audit fallback shape wrong: %v", data)
	}

	// An unknown profile is a 400, not a panic.
	if w, _ := do(t, h, http.MethodGet, "/api/status?profile=nope", nil); w.Code != http.StatusBadRequest {
		t.Errorf("unknown profile status = %d, want 400", w.Code)
	}
}
