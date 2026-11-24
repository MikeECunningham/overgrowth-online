"""The live Arena scoreboard and each local player's gameplay stats stay separate."""

from .campaign_scaling_suite import RENDER_KEYS
from .mode_settings_suite import ARENA, ARENA_MAP
from .newcampaign_local_suite import _code, _join_pad, _ready, _rows
from .scenarios import script_errors


def arena_stats_layout(ctx, small=False):
    width, height, name, port = ((640, 480, "arena_stats_small_host", "28749") if small else
                                 (960, 600, "arena_stats_layout_host", "28729"))
    host = ctx.instance(name, width=width, height=height, keys={
        **RENDER_KEYS, "mp_port": port, "oo_arena_random": "false", "player_invincible": "true"})
    bridge = host.bridge

    def menu(code):
        _code(host, code, "menu")

    def begin():
        seats = _rows(host)
        menu(f'LobbySetSelection("{ARENA}","{ARENA}","{ARENA_MAP}",false);LobbySetForceTeams(true);'
             + "".join(f'LobbySetPlayerTeam({seat["id"]},{1+i%2});' for i, seat in enumerate(seats))
             + 'LobbySetModeSetting("progression",1);LobbyStart();')
        _ready(host, ARENA_MAP)

    def separate(a, b):
        return a[0] >= b[2] or a[2] <= b[0] or a[1] >= b[3] or a[3] <= b[1]

    def check(screens):
        def read():
            rows = sorted(bridge.status()["progression_ui"], key=lambda r: r["screen"])
            if len(rows) == screens and all(r.get("arena_score_rect") and r.get("arena_announcement_rect") and r.get("stats_rect") for r in rows):
                return rows
        rows = bridge.wait(read, 25, what="deployed fighters shown on the Arena scoreboard")
        viewport_height = height / screens
        score = rows[0]["arena_score_rect"]
        assert 0 <= score[0] < score[2] <= width and 0 <= score[1] < score[3] <= viewport_height, score
        if small and screens == 2:
            assert score[3] - score[1] > 150, score  # Eight readable rows in a 240px viewport.
        banner = rows[0]["arena_announcement_rect"]
        assert separate(banner, score), (banner, score)
        assert 0 <= banner[0] < banner[2] <= width and 0 <= banner[1] < banner[3] <= height, banner
        stats = []
        for i, row in enumerate(rows):
            rect = row["stats_rect"]
            top = i * viewport_height
            assert 0 <= rect[0] < rect[2] <= width and top <= rect[1] < rect[3] <= top+viewport_height, rect
            assert rect[0] < 20 and rect[1] > top + viewport_height*.3, (rect, top)
            assert separate(rect, score) and separate(rect, banner), (rect, score, banner)
            stats.append(rect)
        return {"score": score, "stats": stats, "announcement": banner}

    menu("LobbyBeginHost();LobbyAddBot();")
    begin()
    single = check(1)
    ctx.shot(host, "single_player")
    _code(host, "LobbyEndGame();")
    bridge.wait(lambda: bridge.status()["online"]["lobby_waiting"] and not bridge.status()["level_loaded"],
                20, what="Arena returns to lobby")
    _join_pad(host, 0, 2)
    menu("for(int i=0;i<5;++i)LobbyAddBot();")
    assert len(_rows(host)) == 8
    begin()
    stacked = check(2)
    ctx.shot(host, "two_screens_eight_fighters")
    assert not script_errors(host), script_errors(host)
    return {"passed": True, "metrics": {"single": single, "stacked": stacked}, "notes": []}
