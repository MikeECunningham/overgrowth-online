"""Survival HUD contrast and host difficulty through a split-screen battery break."""
from pathlib import Path
import json
from PIL import Image, ImageStat

from .campaign_scaling_suite import RENDER_KEYS
from .mode_settings_suite import SANDBOX_MAP, _code, _probe
from .newcampaign_local_suite import _join_pad, _ready, _rows
from .scenarios import script_errors


def survival_status_hud(ctx):
    host = ctx.instance("survival_status_hud", width=1024, height=720, keys={
        **RENDER_KEYS, "mp_port": "29943", "difficulty_scaling": "true",
        "show_difficulty_readout": "true", "player_invincible": "true"})
    bridge = host.bridge

    def wait(fn, label):
        try:
            return bridge.wait(fn, 35, step=.1, what=label)
        except Exception:
            snapshot = bridge.status()
            Path(ctx.out_dir, "hud_failure.json").write_text(json.dumps({
                "waiting": label, "status_text": status(), "difficulty": snapshot["difficulty_hud"],
                "layout": snapshot["difficulty_hud_layout"], "progression": panels()}, indent=2))
            raise

    def director(code):
        return _code(host, code, "level", "level_specific")

    def status():
        return _probe(host, "status_text", "level", "level_specific")

    def difficulty():
        return bridge.status()["difficulty_hud_layout"]

    def panels():
        return bridge.status()["progression_ui"] or []

    _code(host, "LobbyBeginHost();")
    _join_pad(host, 0, 2)
    _code(host, f'SetControllerPlayer(0);LobbySetSelection("survival","","{SANDBOX_MAP}",false);'
          'LobbySetModeSetting("progression",1);LobbySetModeSetting("progression_free_buffs",0);'
          'LobbySetModeSetting("progression_shop",1);LobbySetModeSetting("progression_timer",0);LobbyStart();')
    _ready(host, SANDBOX_MAP)
    seats = sorted(_rows(host), key=lambda row: row["local_index"])
    wait(lambda: difficulty().get("drawn") and "Wave 1/1" in status(), "fight HUD")
    wait(lambda: any(row.get("arena_score_rect") for row in panels()), "wave panel drawn")
    frame = next(row["arena_score_rect"] for row in panels() if "arena_score_rect" in row)
    x, y, right, bottom = map(round, frame)
    assert 0 <= x < right < 512 and 0 <= y < bottom <= 360
    sky = {"pos": [0, 80, 0], "target": [0, 160, 3]}
    samples = []
    for gui in (False, True):
        bridge.call("view", **sky, gui=gui)
        label = "wave_on_sky" if gui else "sky"
        ctx.shot(host, label)
        with Image.open(Path(ctx.out_dir, f"{host.name}_{label}.jpg")) as image:
            samples.append(ImageStat.Stat(image.convert("L").crop((x+3, y+3, right-3, bottom-3))).median[0])
    bridge.call("view", clear=True)
    assert samples[0] > 65 and samples[1] < samples[0] * .65, samples
    for row in bridge.state():
        if row.get("enabled", True) and "survival_enemies" in row["teams"] and row["knocked_out"] == 0:
            _code(host, "SetKnockedOut(_dead);Ragdoll(_RGDL_LIMP);", row["id"])
    wait(lambda: "complete" in status() and difficulty().get("drawn"), "winner camera retains difficulty")
    wait(lambda: "Choose upgrades" in status() and all(p.get("shop") for p in panels())
         and difficulty().get("shop_inset"), "difficulty beside the two battery shops")
    dock = difficulty()
    assert dock["host_player"] == seats[0]["id"] and dock["screen"] == seats[0]["screen"]
    view_width = dock["viewport"][2] - dock["viewport"][0]
    view_height = dock["viewport"][3] - dock["viewport"][1]
    right, bottom = dock["shop_inset"]
    if view_width >= 1180 and view_height >= 320:
        assert 0 < right < view_width / 2 and bottom == 0
    else:
        assert right == 0 and 0 < bottom < 80
    assert "Next wave" in " ".join(dock["rows"])
    ctx.shot(host, "difficulty_in_shop")
    for row in seats:
        assert bridge.call("progression", actor=row["id"], action="ready")["submitted"]
    wait(lambda: "Next battery in" in status() and difficulty().get("drawn") and not difficulty().get("shop_inset"),
         "normal readout resumes during the rest countdown")
    director("due = the_time;")
    wait(lambda: "Battery 2" in status() and "Starts in" in status() and difficulty().get("drawn"),
         "next battery deployment retains difficulty")
    assert not script_errors(host)
    return {"passed": True, "metrics": {"sky_luma": samples[0], "wave_panel_luma": samples[1],
        "shop_difficulty_rows": dock["rows"], "break_phases_checked": 3}, "notes": []}
