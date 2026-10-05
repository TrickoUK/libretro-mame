#!/usr/bin/env python3
# Usage: lua_run.py <romset> <tag> <seconds> [--lua T:FILE ...] [--shot T ...] [--env K=V ...]
# Boots <romset> in RetroArch (distrobox mame-dev) with the Lua console on, runs `dofile(FILE)` at
# T seconds after launch for every --lua, takes a RetroArch screenshot at every --shot T, kills
# RetroArch after <seconds>. Everything the console printed is in out/<tag>/ra.log (grep for your
# own marker prefix); screenshots in out/<tag>/shots/. Restores MAME.opt afterwards.
import os, sys, re, time, pty, select, socket, subprocess, argparse, signal

ap = argparse.ArgumentParser()
ap.add_argument("romset"); ap.add_argument("tag"); ap.add_argument("seconds", type=float)
ap.add_argument("--lua", action="append", default=[], metavar="T:FILE")
ap.add_argument("--shot", action="append", default=[], type=float, metavar="T")
ap.add_argument("--env", action="append", default=[])
ap.add_argument("--core", default="/var/home/bazzite/Projects/libretro/mame/mame_libretro.so")
a = ap.parse_args()

SCR = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.environ.get("M2PROF_OUT", os.path.join(os.path.dirname(SCR), "out")), a.tag)
SHOTS = os.path.join(OUT, "shots"); os.makedirs(SHOTS, exist_ok=True)
OPT = os.path.expanduser("~/.config/retroarch/config/MAME/MAME.opt")
ROM = f"/var/home/bazzite/Projects/mame-roms/roms/{a.romset}.zip"

def ra_pids():
    return [int(p) for p in subprocess.run(["pgrep", "-x", "retroarch"], capture_output=True, text=True).stdout.split()]

def kill_ra():
    for _ in range(50):
        if not ra_pids(): return
        subprocess.run(["pkill", "-9", "-x", "retroarch"]); time.sleep(0.2)

def udp(cmd):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.sendto(cmd.encode(), ("127.0.0.1", 55355)); s.close()

assert not ra_pids(), "retroarch already running"
opt_orig = open(OPT).read()
open(OPT, "w").write(re.sub(r'mame_lua_console = "\w+"', 'mame_lua_console = "enabled"', opt_orig))
cfg = os.path.join(OUT, "append.cfg")
open(cfg, "w").write(f'video_vsync = "false"\nnetwork_cmd_enable = "true"\nscreenshot_directory = "{SHOTS}"\n'
                     'auto_screenshot_filename = "true"\n')
log = open(os.path.join(OUT, "ra.log"), "wb")
args = ["distrobox", "enter", "mame-dev", "--", "env"] + a.env + ["retroarch", "-v", "--appendconfig", cfg, "-L", a.core, ROM]
pid, fd = pty.fork()
if pid == 0:
    os.execvp(args[0], args)

sched = sorted([(float(x.split(":", 1)[0]), "lua", x.split(":", 1)[1]) for x in a.lua] + [(t, "shot", "") for t in a.shot])
t0 = time.time()
try:
    while time.time() - t0 < a.seconds:
        while sched and time.time() - t0 >= sched[0][0]:
            _, kind, arg = sched.pop(0)
            if kind == "lua": os.write(fd, (f"dofile('{os.path.abspath(arg)}')\n").encode())
            else: udp("SCREENSHOT")
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try: d = os.read(fd, 65536)
            except OSError: break
            log.write(d); log.flush()
finally:
    kill_ra()
    try: os.kill(pid, signal.SIGKILL)
    except OSError: pass
    open(OPT, "w").write(opt_orig)
print("done; log:", os.path.join(OUT, "ra.log"))
