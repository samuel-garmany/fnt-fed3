"""The firmware's device core, built for the PC, behind SimDevice's interface.

fnt's simulator tests take a device factory; passing ``CoreDevice`` runs every
one of them against the C++ that goes on the FED3 instead of the Python model.
"""
import ctypes
import os
import tempfile
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = ctypes.CDLL(os.path.join(HERE, "libfed3core.so"))

LIB.fed_new.restype = ctypes.c_void_p
LIB.fed_new.argtypes = [ctypes.c_char_p, ctypes.c_longlong, ctypes.c_int, ctypes.c_int,
                        ctypes.c_uint]
for name, args in {
    "fed_free": [],
    "fed_reboot": [ctypes.c_int, ctypes.c_int],
    "fed_power_off": [ctypes.c_longlong, ctypes.c_int],
    "fed_advance": [ctypes.c_longlong],
    "fed_poke": [ctypes.c_int, ctypes.c_int],
    "fed_retrieve": [],
    "fed_command": [ctypes.c_char_p],
    "fed_set_jams": [ctypes.c_int],
    "fed_set_sd_failing": [ctypes.c_int],
    "fed_drift": [ctypes.c_longlong],
}.items():
    getattr(LIB, name).argtypes = [ctypes.c_void_p] + args
    getattr(LIB, name).restype = None
for name in ("fed_take", "fed_cues", "fed_rule"):
    getattr(LIB, name).argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    getattr(LIB, name).restype = ctypes.c_int
LIB.fed_get_jams.argtypes = [ctypes.c_void_p]
LIB.fed_get_jams.restype = ctypes.c_int
LIB.fed_file.argtypes = [ctypes.c_void_p]
LIB.fed_file.restype = ctypes.c_char_p

RESETS = {"power": 0, "watchdog": 1, "software": 2, "external": 3}


def _text(fn, handle):
    size = 1 << 16
    while True:
        buf = ctypes.create_string_buffer(size)
        n = fn(handle, buf, size)
        if n >= 0:
            return buf.value.decode("ascii")
        size = -n + 16


class CoreCard:
    """The device's SD card: a directory."""

    def __init__(self, path=None):
        self.path = path or tempfile.mkdtemp(prefix="fed3card-")
        self.device = None

    @property
    def files(self):
        out = {}
        for name in os.listdir(self.path):
            with open(os.path.join(self.path, name), encoding="ascii") as f:
                out[name] = f.read()
        return out

    @property
    def failing(self):
        return False

    @failing.setter
    def failing(self, value):
        LIB.fed_set_sd_failing(self.device.handle, int(bool(value)))


class CoreDevice:
    def __init__(self, device_id=1, now=None, card=None, seed=0, clock_set=True):
        self.card = card or CoreCard()
        now = now or datetime(2026, 10, 20, 9, tzinfo=timezone.utc)
        offset = int(now.utcoffset().total_seconds() // 60)
        device_file = os.path.join(self.card.path, "DEVICE.TXT")
        if not os.path.exists(device_file):
            sign = "-" if offset < 0 else "+"
            with open(device_file, "w", encoding="ascii") as f:
                f.write(f"id={device_id}\noffset={sign}{abs(offset) // 60:02d}:{abs(offset) % 60:02d}\n")
        self.output = []
        self.cues = []
        self.handle = LIB.fed_new(self.card.path.encode(), int(now.timestamp() * 1000),
                                  offset, int(clock_set), seed or 0)
        self.card.device = self
        self._collect()

    def __del__(self):
        if getattr(self, "handle", None):
            LIB.fed_free(self.handle)
            self.handle = None

    def _collect(self):
        text = _text(LIB.fed_take, self.handle)
        if text:
            self.output.extend(text.rstrip("\n").split("\n"))
        cues = _text(LIB.fed_cues, self.handle)
        if cues:
            for line in cues.rstrip("\n").split("\n"):
                slot, cue = line.split("\t")
                self.cues.append((None, slot, cue))

    # -- SimDevice's interface ----------------------------------------------

    @property
    def file(self):
        """The data file the device is writing now."""
        return LIB.fed_file(self.handle).decode("ascii")

    @property
    def jams_pending(self):
        return LIB.fed_get_jams(self.handle)

    @jams_pending.setter
    def jams_pending(self, value):
        LIB.fed_set_jams(self.handle, value)

    def take(self):
        out, self.output = self.output, []
        return out

    def rows(self):
        return [line[4:] for line in self.output if line.startswith("ROW,")]

    def command(self, line):
        before = len(self.output)
        LIB.fed_command(self.handle, line.encode("ascii", "replace"))
        self._collect()
        return self.output[before:]

    def advance(self, seconds):
        LIB.fed_advance(self.handle, int(round(seconds * 1000)))
        self._collect()

    def poke(self, side, ms=100):
        side = 0 if side.upper().startswith("L") else 1
        LIB.fed_poke(self.handle, side, 0)
        LIB.fed_advance(self.handle, ms)
        LIB.fed_poke(self.handle, side, 1)
        self._collect()

    def retrieve(self):
        LIB.fed_retrieve(self.handle)
        self._collect()

    def power_off(self, seconds, lose_clock=False):
        LIB.fed_power_off(self.handle, int(seconds * 1000), int(lose_clock))
        self._collect()

    def reboot(self, cause="power", lose_clock=False):
        LIB.fed_reboot(self.handle, RESETS[cause], int(lose_clock))
        self._collect()

    def drift(self, seconds):
        LIB.fed_drift(self.handle, int(seconds * 1000))

    def rule(self):
        return dict(item.split("=", 1) for item in _text(LIB.fed_rule, self.handle).split(" "))
