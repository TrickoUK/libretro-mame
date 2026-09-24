#!/usr/bin/env python3
# Usage: profile_run.py <romset> <tag> [--warmup S] [--duration S] [--shot-every S] [--state FILE] [--cg]
#                       [--no-perf] [--core PATH] [--lua T:CMD ...]
# Generic version of m2_profile_run.py: boots <romset> (optionally from a save state) under a pty with the
# Lua console enabled, waits --warmup seconds, then attaches perf for --duration seconds while polling
# speed_percent and taking core-framebuffer screenshots (UDP SCREENSHOT) every --shot-every seconds.
# Results go to fork-specific/out/<tag>/. Kills RetroArch and restores MAME.opt afterwards.
import os, pty, sys, time, select, subprocess, shutil, re, signal, socket, argparse

ap = argparse.ArgumentParser()
ap.add_argument("romset"); ap.add_argument("tag")
ap.add_argument("--warmup", type=int, default=30)
ap.add_argument("--duration", type=int, default=60)
ap.add_argument("--shot-every", type=int, default=5)
ap.add_argument("--state"); ap.add_argument("--cg", action="store_true")
ap.add_argument("--no-perf", action="store_true")
ap.add_argument("--lua", action="append", default=[], metavar="T:CMD",
                help="send Lua console command CMD at T seconds after launch (repeatable)")
ap.add_argument("--core", default="/var/home/bazzite/Projects/libretro/mame/mame_libretro.so")
a = ap.parse_args()

REPO = "/var/home/bazzite/Projects/libretro/mame"
SCR = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.environ.get("M2PROF_OUT", os.path.join(os.path.dirname(SCR), "out")), a.tag)
SHOTS = os.path.join(OUT, "shots"); os.makedirs(SHOTS, exist_ok=True)
HOME = os.path.expanduser("~")
OPT = f"{HOME}/.config/retroarch/config/MAME/MAME.opt"
ROM = f"/var/home/bazzite/Projects/mame-roms/roms/{a.romset}.zip"
STATE_DST = f"{HOME}/.config/retroarch/states/MAME/{a.romset}.state"

def ra_pids():
    return [int(p) for p in subprocess.run(["pgrep", "-x", "retroarch"], capture_output=True, text=True).stdout.split()]

def kill_ra():
    for _ in range(50):
        if not ra_pids(): return
        subprocess.run(["pkill", "-9", "-x", "retroarch"]); time.sleep(0.2)

def udp(cmd):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.sendto(cmd.encode(), ("127.0.0.1", 55355)); s.close()

assert not ra_pids(), "retroarch already running"
if a.state: shutil.copy(a.state, STATE_DST)
opt_orig = open(OPT).read()
open(OPT, "w").write(re.sub(r'mame_lua_console = "\w+"', 'mame_lua_console = "enabled"', opt_orig))
cfg = os.path.join(OUT, "append.cfg")
open(cfg, "w").write(f'video_vsync = "false"\nnetwork_cmd_enable = "true"\nscreenshot_directory = "{SHOTS}"\n'
                     'auto_screenshot_filename = "true"\n')

log = open(os.path.join(OUT, "ra.log"), "wb")
args = ["distrobox", "enter", "mame-dev", "--", "retroarch", "-v"] + (["-e", "0"] if a.state else []) + \
       ["--appendconfig", cfg, "-L", a.core, ROM]
pid, fd = pty.fork()
if pid == 0:
    os.execvp(args[0], args)

buf = b""
t_launch = time.time()
lua_sched = sorted((float(x.split(":", 1)[0]), x.split(":", 1)[1]) for x in a.lua)
def pump(t):
    global buf
    end = time.time() + t
    while time.time() < end:
        while lua_sched and time.time() - t_launch >= lua_sched[0][0]:
            os.write(fd, (lua_sched.pop(0)[1] + "\n").encode())
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try: d = os.read(fd, 65536)
            except OSError: return
            log.write(d); log.flush(); buf += d

def send(s): os.write(fd, (s + "\n").encode())

try:
    t0 = time.time()
    while time.time() - t0 < 60 and not ra_pids(): pump(0.5)
    rpid = ra_pids()[0]
    # warmup, with a speed sample + shot every shot_every seconds so boot progress is visible too
    w = 0
    while w < a.warmup:
        pump(a.shot_every); w += a.shot_every
        send("print('SPX'..string.format('%.3f', manager.machine.video.speed_percent))")
        udp("SCREENSHOT")
    print("retroarch pid", rpid, "warmup done", round(time.time() - t0, 1), flush=True)

    def thread_ticks():
        d = {}
        for tid in os.listdir(f"/proc/{rpid}/task"):
            try:
                st = open(f"/proc/{rpid}/task/{tid}/stat").read()
                comm = st[st.index("(")+1:st.rindex(")")]
                f = st[st.rindex(")")+2:].split()
                d[tid] = (comm, int(f[11]) + int(f[12]))
            except OSError: pass
        return d

    nwarm = len(re.findall(rb"SPX([0-9.]+)", buf))
    perf = None
    if not a.no_perf:
        perf_args = ["distrobox", "enter", "mame-dev", "--", "perf", "record", "-p", str(rpid),
                     "-o", os.path.join(OUT, "perf.data")]
        perf_args += (["-F", "299", "--call-graph", "dwarf,16384"] if a.cg else ["-F", "1999", "-s"])
        perf_args += ["--", "sleep", str(a.duration)]
        perf = subprocess.Popen(perf_args, stdout=open(os.path.join(OUT, "perf.stdout"), "w"), stderr=subprocess.STDOUT)
    tk0 = thread_ticks(); tw0 = time.time()
    for i in range(a.duration // a.shot_every):
        pump(a.shot_every)
        send("print('SPX'..string.format('%.3f', manager.machine.video.speed_percent))")
        udp("SCREENSHOT")
    pump(2)
    tk1 = thread_ticks(); tw1 = time.time()
    if perf: perf.wait()
    sp = [x.decode() for x in re.findall(rb"SPX([0-9.]+)", buf)]
    with open(os.path.join(OUT, "summary.txt"), "w") as s:
        s.write("warmup speed_percent: " + " ".join(sp[:nwarm]) + "\n")
        s.write("profile speed_percent: " + " ".join(sp[nwarm:]) + "\n")
        hz = os.sysconf("SC_CLK_TCK"); wall = tw1 - tw0
        rows = [((t1 - tk0[tid][1]) / hz / wall * 100, tid, comm) for tid, (comm, t1) in tk1.items() if tid in tk0]
        rows.sort(reverse=True)
        for pct, tid, comm in rows[:20]:
            s.write(f"thread {tid:>8} {comm:<20} {pct:6.1f}% of one core\n")
    print(open(os.path.join(OUT, "summary.txt")).read())
finally:
    kill_ra()
    try: os.kill(pid, signal.SIGKILL)
    except OSError: pass
    open(OPT, "w").write(opt_orig)
    if a.state and os.path.exists(STATE_DST): os.remove(STATE_DST)
    print("cleaned up; retroarch running:", ra_pids())
