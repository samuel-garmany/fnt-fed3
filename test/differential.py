"""The firmware core and fnt's simulator, given the same days, must write the same file.

Random but reproducible scenarios (pokes, retrievals, waits, reboots, power
cuts, clock corrections, FNT commands) are played through both. The data files
must agree row for row, except the ``ms`` column, which counts from each
implementation's own boot and only matters within a run of rows.

Actions land on whole seconds, as the core's host loop does when idle, so a
difference means the two decide differently, not that one sampled time later.
"""
import os
import random
import sys
from datetime import datetime, timedelta, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
FNT = os.environ.get("FNT", os.path.join(HERE, "..", "..", "fnt"))
sys.path.insert(0, FNT)
sys.path.insert(0, HERE)

from fed3_core import CoreDevice                     # noqa: E402
from fnt.fed3 import device_sim as dev               # noqa: E402
from fnt.fed3 import protocol as proto               # noqa: E402

TZ = timezone(timedelta(hours=-7))
PROTOCOL = f"""\
format: {proto.FORMAT}
name: Differential
devices: [A]
cues:
  ding: [{{tone: {{hz: 3000, ms: 50}}}}, {{flash: {{target: poke, color: blue, ms: 300}}}}]
phases:
  - name: Free
    until: {{at: "19:00"}}
    all: {{active: none, free_feed: 1, refill_delay: 20s}}
  - name: Shared
    until: [{{rewards: 15}}, {{after: 1d}}]
    all: {{active: both, ratio: 2, cue: {{reward: ding, counted: [click]}}}}
  - name: PerPort
    until: {{pokes: 40, side: left}}
    all: {{active: both, ratio: 3, count: per_port, timeout: 20s, cue: {{timeout: [click]}}}}
  - name: PR
    until: [{{rewards: 6}}, {{after: 12h}}]
    all: {{ratio_kind: progressive, active: right, min_poke_ms: 2000}}
  - name: Quota
    until: {{after: 2d}}
    all: {{active: both, quota: 4, quota_period: 1h, cue: {{depleted: [click]}}}}
    daily:
      - {{from: "02:00", to: "04:00", all: {{free_feed: 1, refill_delay: 30s}}}}
  - name: Wait
    until: [{{rewards: 1000, devices: all}}, {{after: 6h}}]
    all: {{active: left, ratio: 4}}
  - name: Tail
    all: {{active: left, reward: none, cue: {{reward: [click]}}}}
"""


def build_actions(seed, count=4000):
    rng = random.Random(seed)
    actions = []
    for _ in range(count):
        roll = rng.random()
        if roll < 0.5:
            actions.append(("poke", rng.choice("LLR"), rng.choice([1, 1, 1, 2, 3, 61])))
        elif roll < 0.7:
            actions.append(("retrieve",))
        elif roll < 0.97:
            actions.append(("advance", rng.choice([1, 2, 5, 30, 60, 300, 900, 3600])))
        elif roll < 0.98:
            actions.append(("reboot", rng.choice(["power", "watchdog"])))
        elif roll < 0.985:
            actions.append(("power_off", rng.choice([60, 600, 7200, 30000])))
        elif roll < 0.99:
            actions.append(("command", "GOTO_CURRENT"))
        elif roll < 0.995:
            actions.append(("command", "STATUS"))
        else:
            actions.append(("feed",))
    return actions


def play(device, schedule, actions):
    device.command("SCHED:BEGIN")
    for line in schedule.lines():
        device.command(f"S:{line}")
    device.command(f"SCHED:COMMIT,{schedule.crc():08x}")
    t0 = datetime(2026, 10, 20, 9, tzinfo=TZ)
    device.command(f"RUN:BEGIN,DIFF1,{dev.iso(t0)},{schedule.crc():08x}")
    for action in actions:
        kind = action[0]
        if kind == "poke":
            device.poke(action[1], ms=action[2] * 1000)
        elif kind == "retrieve":
            device.retrieve()
        elif kind == "advance":
            device.advance(action[1])
        elif kind == "reboot":
            device.reboot(action[1])
        elif kind == "power_off":
            device.power_off(action[1])
        elif kind == "feed":
            device.command("FEED")
        elif action[1] == "GOTO_CURRENT":
            status = device.command("STATUS")[-1]
            phase = int(dict(f.split("=", 1) for f in status.split(",")[1:])["phase"])
            if phase == 5:      # the phase that only FNT (or 6 h) ends
                device.command(f"GOTO:{phase},{phase + 1}")
        else:
            device.command(action[1])
    return device.card.files["DIFF1.CSV"]


def compare(a, b):
    rows_a = a.splitlines()
    rows_b = b.splitlines()
    for index, (x, y) in enumerate(zip(rows_a, rows_b)):
        fx, fy = x.split(",", 15), y.split(",", 15)
        if index and (fx[:1] + fx[2:] != fy[:1] + fy[2:]):
            return f"row {index}:\n      sim:  {x}\n      core: {y}"
    if len(rows_a) != len(rows_b):
        return f"{len(rows_a)} rows from the simulator, {len(rows_b)} from the core"
    return None


def main(seeds=range(1, 9)):
    compiled = proto.compile_protocol(proto.load(PROTOCOL))
    schedule = compiled.schedules["A"]
    failures = 0
    for seed in seeds:
        actions = build_actions(seed)
        start = datetime(2026, 10, 20, 9, tzinfo=TZ)
        sim = play(dev.SimDevice(device_id=4, now=start, seed=seed), schedule, actions)
        core = play(CoreDevice(device_id=4, now=start, seed=seed), schedule, actions)
        problem = compare(sim, core)
        rows = len(sim.splitlines()) - 1
        phases = sorted({r.split(",")[3] for r in sim.splitlines()[1:]})
        print(f"  {'PASS' if not problem else 'FAIL'}  seed {seed}: {rows} rows, "
              f"phases {','.join(phases)}{'' if not problem else ' — ' + problem}")
        failures += bool(problem)
    print(f"\n{len(seeds) - failures}/{len(seeds)} scenarios identical")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
