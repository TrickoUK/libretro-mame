#!/usr/bin/env python3
# Usage: smoke.py <seconds> <romset>... - boot each romset, check it's still alive after N
# seconds, screenshot it, then kill RetroArch for real. Results go to fork-specific/out/<TAG>.
import os, sys, time, subprocess, re
SCR = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.environ.get("M2PROF_OUT", os.path.join(os.path.dirname(SCR), "out")), os.environ.get("TAG", "smoke")); os.makedirs(OUT, exist_ok=True)
CORE = os.environ.get("CORE", "/var/home/bazzite/Projects/libretro/mame/mame_libretro.so")
ROMS = "/var/home/bazzite/Projects/mame-roms/roms"
secs = int(sys.argv[1]); names = sys.argv[2:]
cfg = os.path.join(OUT, "append.cfg")
open(cfg, "w").write('video_vsync = "false"\n')

def pids():
    return subprocess.run(["pgrep", "-x", "retroarch"], capture_output=True, text=True).stdout.split()

for n in names:
    assert not pids()
    log = open(os.path.join(OUT, n + ".log"), "wb")
    p = subprocess.Popen(["distrobox", "enter", "mame-dev", "--", "retroarch", "-v", "--appendconfig", cfg,
                          "-L", CORE, f"{ROMS}/{n}.zip"], stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    time.sleep(secs)
    alive = bool(pids())
    if alive:
        subprocess.run(["spectacle", "-a", "-b", "-n", "-o", os.path.join(OUT, n + ".png")], capture_output=True)
    while pids():
        subprocess.run(["pkill", "-9", "-x", "retroarch"]); time.sleep(0.3)
    p.kill()
    txt = open(os.path.join(OUT, n + ".log"), "rb").read().decode("latin1")
    bad = [l for l in txt.splitlines() if re.search(r"(?i)fatal|segmentation|unmapped code|core dumped|\[ERROR\]", l)]
    print(f"{n:10} alive_after_{secs}s={alive} errors={len(bad)} {bad[:2]}", flush=True)
