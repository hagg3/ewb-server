package httpd

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func testGuard(token string) *guard {
	return &guard{
		token:   token,
		allowed: map[string]bool{"127.0.0.1:7777": true, "localhost:7777": true},
		next: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			ok(w, "reached")
		}),
		openPath: isAssetPath,
	}
}

func req(method, path string) *http.Request {
	r := httptest.NewRequest(method, "http://127.0.0.1:7777"+path, nil)
	r.RemoteAddr = "127.0.0.1:51000"
	r.Host = "127.0.0.1:7777"
	return r
}

func TestGuardRejectsNonLoopback(t *testing.T) {
	g := testGuard("tok")
	r := req(http.MethodGet, "/api/state")
	r.RemoteAddr = "10.0.0.5:4000"
	r.Header.Set("X-Edenadmin-Token", "tok")
	w := httptest.NewRecorder()
	g.ServeHTTP(w, r)
	if w.Code != http.StatusForbidden {
		t.Fatalf("code = %d, want 403", w.Code)
	}
}

func TestGuardRejectsMissingToken(t *testing.T) {
	g := testGuard("tok")
	w := httptest.NewRecorder()
	g.ServeHTTP(w, req(http.MethodGet, "/api/state"))
	if w.Code != http.StatusUnauthorized {
		t.Fatalf("code = %d, want 401", w.Code)
	}
}

func TestGuardRejectsWrongToken(t *testing.T) {
	g := testGuard("tok")
	r := req(http.MethodGet, "/api/state")
	r.Header.Set("X-Edenadmin-Token", "WRONG")
	w := httptest.NewRecorder()
	g.ServeHTTP(w, r)
	if w.Code != http.StatusUnauthorized {
		t.Fatalf("code = %d, want 401", w.Code)
	}
}

func TestGuardRejectsForeignOrigin(t *testing.T) {
	g := testGuard("tok")
	r := req(http.MethodPost, "/api/connection/test")
	r.Header.Set("X-Edenadmin-Token", "tok")
	r.Header.Set("Origin", "http://evil.example")
	w := httptest.NewRecorder()
	g.ServeHTTP(w, r)
	if w.Code != http.StatusForbidden {
		t.Fatalf("code = %d, want 403", w.Code)
	}
}

func TestGuardRejectsForeignHost(t *testing.T) {
	g := testGuard("tok")
	r := req(http.MethodGet, "/api/state")
	r.Header.Set("X-Edenadmin-Token", "tok")
	r.Host = "attacker.test"
	w := httptest.NewRecorder()
	g.ServeHTTP(w, r)
	if w.Code != http.StatusForbidden {
		t.Fatalf("code = %d, want 403", w.Code)
	}
}

func TestGuardRejectsGETOnMutatingRoute(t *testing.T) {
	g := testGuard("tok")
	r := req(http.MethodGet, "/api/connection/test")
	r.Header.Set("X-Edenadmin-Token", "tok")
	w := httptest.NewRecorder()
	g.ServeHTTP(w, r)
	if w.Code != http.StatusMethodNotAllowed {
		t.Fatalf("code = %d, want 405", w.Code)
	}
}

func TestGuardAllowsGoodRequest(t *testing.T) {
	g := testGuard("tok")
	r := req(http.MethodPost, "/api/connection/test")
	r.Header.Set("X-Edenadmin-Token", "tok")
	r.Header.Set("Origin", "http://127.0.0.1:7777")
	w := httptest.NewRecorder()
	g.ServeHTTP(w, r)
	if w.Code != http.StatusOK {
		t.Fatalf("code = %d, want 200 (body %s)", w.Code, w.Body)
	}
}

func TestGuardServesRootWithoutToken(t *testing.T) {
	g := testGuard("tok")
	w := httptest.NewRecorder()
	g.ServeHTTP(w, req(http.MethodGet, "/"))
	if w.Code != http.StatusOK {
		t.Fatalf("root should be open, code = %d", w.Code)
	}
}

func TestNewTokenIsUniqueAndURLSafe(t *testing.T) {
	seen := map[string]bool{}
	for i := 0; i < 100; i++ {
		tok := NewToken()
		if seen[tok] {
			t.Fatal("NewToken produced a duplicate")
		}
		seen[tok] = true
		if strings.ContainsAny(tok, "+/=") {
			t.Fatalf("token is not URL-safe base64: %q", tok)
		}
	}
}

func TestEnvelopeShape(t *testing.T) {
	w := httptest.NewRecorder()
	ok(w, map[string]int{"n": 1})
	var env struct {
		OK   bool           `json:"ok"`
		Data map[string]int `json:"data"`
	}
	if err := json.NewDecoder(w.Body).Decode(&env); err != nil {
		t.Fatal(err)
	}
	if !env.OK || env.Data["n"] != 1 {
		t.Fatalf("bad envelope: %+v", env)
	}

	w2 := httptest.NewRecorder()
	fail(w2, http.StatusBadRequest, "nope")
	body, _ := io.ReadAll(w2.Body)
	if !strings.Contains(string(body), `"ok":false`) || !strings.Contains(string(body), `"error":"nope"`) {
		t.Fatalf("bad failure envelope: %s", body)
	}
}
