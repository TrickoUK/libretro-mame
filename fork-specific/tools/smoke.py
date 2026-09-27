#!/usr/bin/env python3
# Usage: smoke.py <seconds> <romset>... - boot each romset, check it's still alive after N
# seconds, screenshot it, then kill RetroArch for real. Results go to fork-specific/out/<TAG>.
# Screenshots use RetroArch's own SCREENSHOT network command (the core's frame), never a
# desktop capture tool, which grabs whatever window is active.
import os, sys, time, subprocess, re, socket, glob, shutil
SCR = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.environ.get("M2PROF_OUT", os.path.join(os.path.dirname(SCR), "out")), os.environ.get("TAG", "smoke")); os.makedirs(OUT, exist_ok=True)
CORE = os.environ.get("CORE", "/var/home/bazzite/Projects/libretro/mame/mame_libretro.so")
ROMS = "/var/home/bazzite/Projects/mame-roms/roms"
secs = int(sys.argv[1]); names = sys.argv[2:]
cfg = os.path.join(OUT, "append.cfg")
SHOTS = os.path.join(OUT, "shots"); os.makedirs(SHOTS, exist_ok=True)
open(cfg, "w").write(f'video_vsync = "false"\nnetwork_cmd_enable = "true"\nscreenshot_directory = "{SHOTS}"\n'
                     'auto_screenshot_filename = "true"\n')

def screenshot(dest):
    before = set(glob.glob(os.path.join(SHOTS, "*.png")))
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.sendto(b"SCREENSHOT", ("127.0.0.1", 55355)); s.close()
    for _ in range(50):
        new = set(glob.glob(os.path.join(SHOTS, "*.png"))) - before
        if new:
            time.sleep(0.3); shutil.move(new.pop(), dest); return
        time.sleep(0.1)

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
        screenshot(os.path.join(OUT, n + ".png"))
    while pids():
        subprocess.run(["pkill", "-9", "-x", "retroarch"]); time.sleep(0.3)
    p.kill()
    txt = open(os.path.join(OUT, n + ".log"), "rb").read().decode("latin1")
    bad = [l for l in txt.splitlines() if re.search(r"(?i)fatal|segmentation|unmapped code|core dumped|\[ERROR\]", l)]
    print(f"{n:10} alive_after_{secs}s={alive} errors={len(bad)} {bad[:2]}", flush=True)
