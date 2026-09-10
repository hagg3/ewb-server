package panels

import (
	"context"
	"strings"
	"testing"
)

func TestPlayerActionRejectsNewlineBeforeDispatch(t *testing.T) {
	p := localProfile(t)
	f := &fakeTransport{}
	rt := rtWithFake(f)
	writeSelfPidfile(t, rt.Supervisor(p))

	res := rt.PlayerAction(context.Background(), p, PlayerActionReq{
		Verb: "say", Text: "hello\nban Someone",
	})
	if res.OK || res.Kind != "rejected" || !strings.Contains(res.Message, "newline") {
		t.Fatalf("expected a pre-dispatch rejection, got %+v", res)
	}
	if len(f.calls) != 0 {
		t.Fatalf("nothing should have been dispatched, ran: %v", f.calls)
	}
}

func TestPlayerActionSayOK(t *testing.T) {
	p := localProfile(t)
	f := &fakeTransport{replies: []fakeReply{{match: "say", stdout: "ok\n"}}}
	rt := rtWithFake(f)
	writeSelfPidfile(t, rt.Supervisor(p))

	res := rt.PlayerAction(context.Background(), p, PlayerActionReq{Verb: "say", Text: "server going down in 5"})
	if !res.OK || res.Kind != "ok" {
		t.Fatalf("say: %+v", res)
	}
	if len(f.calls) != 1 || !strings.Contains(f.calls[0], "say") {
		t.Fatalf("dispatched calls = %v", f.calls)
	}
}

func TestPlayerActionKickErrorReplyIsFailure(t *testing.T) {
	p := localProfile(t)
	f := &fakeTransport{replies: []fakeReply{{match: "kick", stdout: "error: no player named 'ghost'\n"}}}
	rt := rtWithFake(f)
	writeSelfPidfile(t, rt.Supervisor(p))

	res := rt.PlayerAction(context.Background(), p, PlayerActionReq{Verb: "kick", Name: "ghost"})
	if res.OK || res.Kind != "error" || !strings.Contains(res.Message, "no player") {
		t.Fatalf("kick error: %+v", res)
	}
}

func TestPlayerActionServerDownIsRejected(t *testing.T) {
	p := localProfile(t)
	f := &fakeTransport{}
	rt := rtWithFake(f)

	res := rt.PlayerAction(context.Background(), p, PlayerActionReq{Verb: "deop", Name: "Alice"})
	if res.OK || res.Kind != "rejected" || !strings.Contains(res.Message, "not running") {
		t.Fatalf("server-down: %+v", res)
	}
	if len(f.calls) != 0 {
		t.Fatalf("nothing should be dispatched to a stopped server: %v", f.calls)
	}
}

func TestPlayerActionOpLevelBounds(t *testing.T) {
	p := localProfile(t)
	rt := rtWithFake(&fakeTransport{})
	writeSelfPidfile(t, rt.Supervisor(p))
	for _, lvl := range []int{-1, 3, 99} {
		res := rt.PlayerAction(context.Background(), p, PlayerActionReq{Verb: "op", Name: "Alice", Level: lvl})
		if res.Kind != "rejected" {
			t.Errorf("op level %d should be rejected, got %+v", lvl, res)
		}
	}
}

func TestBansPanel(t *testing.T) {
	p := localProfile(t)
	f := &fakeTransport{replies: []fakeReply{{match: "banlist", stdout: "2 ban entr(y/ies):\n  10.0.0.9  (ip)\n  Griefer  (name)\n"}}}
	rt := rtWithFake(f)
	writeSelfPidfile(t, rt.Supervisor(p))

	res := rt.Bans(context.Background(), p)
	if !res.Online || len(res.Entries) != 2 {
		t.Fatalf("bans: %+v", res)
	}
	if res.Entries[0].Kind != "ip" || res.Entries[1].Token != "Griefer" {
		t.Errorf("entries = %+v", res.Entries)
	}
}

func TestBansPanelServerDown(t *testing.T) {
	p := localProfile(t)
	rt := rtWithFake(&fakeTransport{})
	res := rt.Bans(context.Background(), p)
	if res.Online || res.Note == "" {
		t.Fatalf("expected an offline note: %+v", res)
	}
}
