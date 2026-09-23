#!/usr/bin/env python3
# Launch polystar from savestates/polystar.state (results go to fork-specific/out/<tag>) under a pty (Lua console for
# speed_percent), attach perf for DURATION seconds, sample per-thread CPU, then
# kill retroarch for real and restore MAME.opt.
import os, pty, sys, time, select, subprocess, shutil, re, signal

TAG = sys.argv[1] if len(sys.argv) > 1 else "baseline"
DURATION = int(sys.argv[2]) if len(sys.argv) > 2 else 60
CALLGRAPH = (sys.argv[3] == "cg") if len(sys.argv) > 3 else False
REPO = "/var/home/bazzite/Projects/libretro/mame"
SCR = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.environ.get("M2PROF_OUT", os.path.join(os.path.dirname(SCR), "out")), TAG); os.makedirs(OUT, exist_ok=True)
HOME = os.path.expanduser("~")
OPT = f"{HOME}/.config/retroarch/config/MAME/MAME.opt"
STATE_DST = f"{HOME}/.config/retroarch/states/MAME/polystar.state"
ROM = "/var/home/bazzite/Projects/mame-roms/roms/polystar.zip"

def ra_pids():
    r = subprocess.run(["pgrep", "-x", "retroarch"], capture_output=True, text=True)
    return [int(p) for p in r.stdout.split()]

def kill_ra():
    for _ in range(50):
        p = ra_pids()
        if not p: return
        subprocess.run(["pkill", "-9", "-x", "retroarch"])
        time.sleep(0.2)

assert not ra_pids(), "retroarch already running"
shutil.copy(f"{REPO}/savestates/polystar.state", STATE_DST)
opt_orig = open(OPT).read()
open(OPT, "w").write(re.sub(r'mame_lua_console = "\w+"', 'mame_lua_console = "enabled"', opt_orig))
cfg = os.path.join(OUT, "append.cfg")
open(cfg, "w").write('video_vsync = "false"\nnetwork_cmd_enable = "true"\n')

log = open(os.path.join(OUT, "ra.log"), "wb")
pid, fd = pty.fork()
if pid == 0:
    os.execvp("distrobox", ["distrobox", "enter", "mame-dev", "--", "retroarch", "-v",
              "-e", "0", "--appendconfig", cfg, "-L", f"{REPO}/mame_libretro.so", ROM])

buf = b""
def pump(t):
    global buf
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try: d = os.read(fd, 65536)
            except OSError: return
            log.write(d); log.flush(); buf += d

def send(s): os.write(fd, (s + "\n").encode())

try:
    # wait for the state load
    t0 = time.time()
    while time.time() - t0 < 120:
        pump(0.5)
        if re.search(rb"(?i)(loading state|state.*loaded|Loaded state)", buf): break
    pump(3)  # settle
    rpid = ra_pids()[0]
    print("retroarch pid", rpid, "state-load wait", round(time.time() - t0, 1), flush=True)

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

    perf_args = ["distrobox", "enter", "mame-dev", "--", "perf", "record", "-p", str(rpid),
                 "-o", os.path.join(OUT, "perf.data"), "--", "sleep", str(DURATION)]
    if CALLGRAPH:
        perf_args[6:6] = ["-F", "299", "--call-graph", "dwarf,16384"]
    else:
        perf_args[6:6] = ["-F", "1999", "-s"]
    tk0 = thread_ticks(); tw0 = time.time()
    perf = subprocess.Popen(perf_args, stdout=open(os.path.join(OUT, "perf.stdout"), "w"), stderr=subprocess.STDOUT)
    sp = []
    for i in range(DURATION // 5):
        pump(5)
        send(f"print('SPX'..string.format('%.3f', manager.machine.video.speed_percent))")
    pump(2)
    tk1 = thread_ticks(); tw1 = time.time()
    perf.wait()
    sp = re.findall(rb"SPX([0-9.]+)", buf)
    with open(os.path.join(OUT, "summary.txt"), "w") as s:
        s.write("speed_percent samples: " + " ".join(x.decode() for x in sp) + "\n")
        hz = os.sysconf("SC_CLK_TCK"); wall = tw1 - tw0
        rows = []
        for tid, (comm, t1) in tk1.items():
            if tid in tk0: rows.append(((t1 - tk0[tid][1]) / hz / wall * 100, tid, comm))
        rows.sort(reverse=True)
        for pct, tid, comm in rows[:20]:
            s.write(f"thread {tid:>8} {comm:<20} {pct:6.1f}% of one core\n")
    print(open(os.path.join(OUT, "summary.txt")).read())
finally:
    kill_ra()
    try: os.kill(pid, signal.SIGKILL)
    except OSError: pass
    open(OPT, "w").write(opt_orig)
    os.remove(STATE_DST)
    print("cleaned up; retroarch running:", ra_pids())
