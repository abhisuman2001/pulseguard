# Codelab: Let AI near a safety-critical loop without letting it hurt anyone (QNX 8.0)

**Time:** about 60 minutes · **Level:** you know C and have used Linux · **You'll need:** a free QNX SDP 8.0 license, Ubuntu 22.04/24.04 (WSL2 works), QEMU

You'll build the core of a mixed-criticality system step by step, the same way [PulseGuard](../../README.md) is built:

1. Boot QNX 8.0 in QEMU and cross-compile for it
2. A **1 kHz control loop** woken by **timer pulses**, with measured jitter
3. Model hardware as a **resource manager** (`/dev/...`, `devctl()`)
4. Let other processes talk to the loop with **message passing**, and see why **priority inheritance** makes that safe
5. Put the AI in a box with **`SCHED_SPORADIC`**
6. Add a **heartbeat watchdog** with pulses and `posix_spawn`
7. Prove it: stress, rogue AI, kill -9

Each step ends with a ✅ check you can run.

---

## 1. Boot QNX 8.0 and cross-compile

Install QNX SDP 8.0 through the QNX Software Center (free non-commercial license), then:

```bash
sudo apt install qemu-system-x86
source ~/qnx800/qnxsdp-env.sh            # puts qcc, mkqnximage, ... on PATH
mkdir -p ~/vm && cd ~/vm
mkqnximage --type=qemu --arch=x86_64 --ssh-ident=$HOME/.ssh/id_ed25519 --build
mkqnximage --run=-h                      # headless boot
mkqnximage --getip                       # -> e.g. 172.31.1.11
ssh root@$(mkqnximage --getip) uname -a  # QNX ... 8.0.0 ...
```

> **WSL2 tip:** check `ls /dev/kvm`. Without KVM, QEMU emulates the CPU in software and every timing number you measure gets much worse.

Cross-compiling is one flag. `qcc -V gcc_ntox86_64` targets QNX x86_64, and `gcc_ntoaarch64le` targets 64-bit ARM (Raspberry Pi 4):

```bash
qcc -V gcc_ntox86_64 -O2 -o hello hello.c && scp hello root@$IP:/data/
```

`/data` is the writable partition on `mkqnximage` images.

✅ `ssh root@$IP /data/hello` prints your message.

## 2. A 1 kHz loop driven by pulses

On QNX the idiomatic periodic loop isn't `sleep()` and isn't a signal handler. It's a **timer that sends a pulse** to a channel your thread blocks on. A pulse is a tiny fixed-size, non-blocking message, and it carries a priority, so the kernel knows how urgent your wake-up is.

The whole program is in [`tick.c`](tick.c). The important part:

```c
int chid = ChannelCreate(0);
int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);

struct sigevent ev;
SIGEV_PULSE_INIT(&ev, coid, 60 /* prio */, TICK_CODE, 0);
timer_t tid;
timer_create(CLOCK_MONOTONIC, &ev, &tid);
struct itimerspec its = { {0, 1000000}, {0, 1000000} };   /* 1 ms */
timer_settime(tid, 0, &its, NULL);

for (;;) {
    MsgReceivePulse(chid, &p, sizeof p, NULL);   /* wake exactly here */
    /* control law */
}
```

Three more lines make it trustworthy:

```c
mlockall(MCL_CURRENT | MCL_FUTURE);                                  /* no page faults   */
pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);              /* prio 60          */
ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)(1u << cpu));        /* own a CPU        */
```

To measure jitter, `ClockCycles()` reads the free-running cycle counter. Convert it with `SYSPAGE_ENTRY(qtime)->cycles_per_sec`. Compare every wake-up against the *ideal* time `t0 + k·period`, not against the previous tick, so drift can't hide.

```bash
qcc -V gcc_ntox86_64 -O2 -o tick docs/codelab/tick.c && scp tick root@$IP:/data/
ssh root@$IP /data/tick 10 1
```

✅ You get a worst-case lateness in microseconds and, ideally, zero misses.

## 3. Hardware is a path name: resource managers

On QNX, drivers are ordinary user-space processes that **own part of the path namespace**. Your control loop shouldn't care whether `/dev/pulseguard/plant` is a real pressure sensor or a simulation, and on QNX it can't tell.

PulseGuard's [`sensord.c`](../../src/sensord/sensord.c) attaches a path and handles `devctl()`:

```c
dispatch_t *dpp = dispatch_create();
iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS, &io_funcs);
io_funcs.devctl = io_devctl;           /* our handler                    */
io_funcs.read   = io_read;             /* so `cat /dev/...` works too    */
iofunc_attr_init(&attr, S_IFCHR | 0660, NULL, NULL);
resmgr_attach(dpp, &rattr, "/dev/pulseguard/plant", _FTYPE_ANY, 0,
              &connect_funcs, &io_funcs, &attr);
```

A neat trick for control loops is a **bidirectional** devctl. `__DIOTF` means *data to and from*, so one message pass sends the actuator command and returns the newest sensor sample:

```c
#define DCMD_PG_EXCHANGE __DIOTF(_DCMD_MISC, 0x50, pg_plant_xchg_t)
...
devctl(fd, DCMD_PG_EXCHANGE, &x, sizeof x, NULL);   /* in: command, out: sample */
```

In the handler, always call `iofunc_devctl_default()` first, copy the input out of the message before you write the reply into the same buffer, and reply with `_RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + nbytes)`.

✅ `ssh root@$IP cat /dev/pulseguard/plant` prints live pressure and flow.

## 4. Message passing, and why it's safe

Other processes (the dashboard bridge, the AI) need to send settings to the loop. QNX IPC is **synchronous**: `MsgSend()` blocks the client until the server calls `MsgReply()`. Servers register a name:

```c
name_attach_t *att = name_attach(NULL, "pulseguard/control", 0);   /* server */
int coid = name_open("pulseguard/control", 0);                      /* client */
MsgSend(coid, &msg, sizeof msg, &reply, sizeof reply);
```

The server loop must also handle the connect message and disconnect pulses that `name_open` and client exit generate:

```c
rcvid = MsgReceive(att->chid, &msg, sizeof msg, &info);
if (rcvid == 0 && msg.pulse.code == _PULSE_CODE_DISCONNECT) ConnectDetach(msg.pulse.scoid);
if (msg.type == _IO_CONNECT) { MsgReply(rcvid, EOK, NULL, 0); continue; }
if (info.msglen < sizeof(expected)) { MsgError(rcvid, EINVAL); continue; }   /* validate! */
```

**The safety property.** When a client sends a message, the server thread receiving it **runs at the client's priority** (priority inheritance). A priority-30 AI talking to the controller's server thread is handled at priority 30, so it can never borrow the control loop's priority and starve anything. Conversely, when the priority-60 control loop calls `devctl()` on the resource manager, that server runs at 60. You get a correct priority picture across process boundaries without configuring anything.

PulseGuard pushes settings from the server thread to the 1 kHz thread with a `trylock` hand-off, so the loop **never blocks** on a lock (see `control_thread` in [`controld.c`](../../src/controld/controld.c)).

✅ Send a settings message and watch the loop pick it up on the next tick.

## 5. Put the AI in a box: `SCHED_SPORADIC`

Earlier QNX releases had the Adaptive Partitioning scheduler for CPU budgets. **QNX 8.0's redesigned kernel doesn't ship APS.** The tool for "this thread may use at most X% of a CPU at high priority" is the POSIX **sporadic server** policy, which QNX implements in the kernel:

```c
pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED);
pthread_attr_setschedpolicy(&a, SCHED_SPORADIC);
sp.sched_priority               = 30;          /* normal priority            */
sp.sched_ss_low_priority        = 5;           /* when budget is exhausted   */
sp.sched_ss_init_budget.tv_nsec = 2000000;     /* 2 ms of CPU ...            */
sp.sched_ss_repl_period.tv_nsec = 10000000;    /* ... per 10 ms              */
sp.sched_ss_max_repl            = 8;
pthread_attr_setschedparam(&a, &sp);
pthread_create(&th, &a, advisor_thread, NULL);
```

A well-behaved model uses a few microseconds per breath and never notices the fence. A runaway model burns its 2 ms and gets **demoted by the kernel** to priority 5. It can still use idle CPU, but it can no longer compete with anything that matters.

Two more QNX tools let the AI *know* it's being throttled:

```c
pthread_getschedparam(pthread_self(), &pol, &cur);
cur.sched_curpriority;                         /* effective priority: 30 or 5 */

pthread_getcpuclockid(pthread_self(), &cid);   /* per-thread CPU-time clock   */
clock_gettime(cid, &ts);
```

Finish the box with memory protection: map telemetry **read-only** (`PROT_READ`). The AI can then only *propose* changes, by message, to a server that clamps them.

✅ `pidin | grep aiguard` shows `30s` (sporadic, prio 30). Trigger a runaway and it shows `5s`.

## 6. A watchdog in 40 lines of logic

The controller sends a heartbeat **pulse** every 10 ms. Pulses never block the sender, so a wedged watchdog can't wedge the control loop:

```c
MsgSendPulse(wdog_coid, 58, PG_PULSE_HEARTBEAT, 0);
```

The watchdog gets its own timer pulse every 5 ms on the *same* channel, so heartbeats and supervision ticks arrive in one `MsgReceive` loop:

```c
case PG_PULSE_HEARTBEAT: last_beat = now; break;
case PG_PULSE_CHECK:
    waitpid(-1, &st, WNOHANG);                        /* reap crashes        */
    if (now - last_beat > 50 ms) {                    /* hung or dead        */
        kill(pid, SIGKILL); waitpid(pid, NULL, 0);
        posix_spawn(&pid, path, NULL, NULL, argv, environ);
    }
```

Design the restart so state isn't lost: PulseGuard's controller keeps the clinician's settings in shared memory and restores them on start. The simulated hardware has its own fail-safe that opens the valve if commands stop for 20 ms, because the cheapest protection is the one that doesn't depend on software at all.

Finally, **least privilege**. Once running, nothing but the watchdog needs to create processes:

```c
procmgr_ability(0,
  PROCMGR_ADN_ROOT|PROCMGR_ADN_NONROOT|PROCMGR_AOP_DENY|PROCMGR_AOP_LOCK|PROCMGR_AID_SPAWN,
  PROCMGR_ADN_ROOT|PROCMGR_ADN_NONROOT|PROCMGR_AOP_DENY|PROCMGR_AOP_LOCK|PROCMGR_AID_FORK,
  PROCMGR_AID_EOL);
```

✅ `slay -s KILL pg_controld` (or the **Kill controller** button). `slog2info` shows the detection and the restart, and the dashboard shows the recovery time.

## 7. Prove it

```bash
git clone <this repo> && cd pulseguard
source ~/qnx800/qnxsdp-env.sh
scripts/qnx-vm.sh create && scripts/qnx-vm.sh start
scripts/deploy.sh                       # builds, deploys, opens http://localhost:8080
```

Then press **CPU stress**, **Rogue AI** and **Kill controller**, and watch the jitter histogram not move.

For a reproducible comparison, run the same loop on QNX and on Linux on the same machine:

```bash
ssh root@$IP /data/pulseguard/rtbench -s 30 -l 7     # QNX 8.0
make build/host/rtbench && sudo ./build/host/rtbench -s 30 -l 7   # Linux
```

## What you learned

| Problem | QNX 8.0 answer |
|---|---|
| Wake up exactly every 1 ms | `timer_create` + `SIGEV_PULSE_INIT` + `MsgReceivePulse` |
| Measure it honestly | `ClockCycles()`, ideal-time reference |
| Hardware abstraction | resource manager + `devctl(__DIOTF)` |
| Safe cross-process requests | synchronous `MsgSend` + priority inheritance |
| Bound a misbehaving thread | `SCHED_SPORADIC`, `sched_curpriority`, per-thread CPU clock |
| Detect and recover from crashes | heartbeat pulses + `posix_spawn` |
| Limit blast radius | `procmgr_ability`, `PROT_READ` mappings, separate processes |
