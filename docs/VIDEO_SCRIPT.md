# Demo video script (target 2:45, hard limit 3:00)

Rules: show it **running on QNX 8.0** (QEMU), public YouTube, **no copyrighted music** (use none, or a royalty-free track you have rights to), no third-party trademarks beyond QNX's own product name.

Recording setup: OBS at 1080p. Left two-thirds: browser dashboard (connected to the QNX target, **no emulator banner**). Right third: terminal with `ssh root@<ip>`. Increase the terminal font size.

| Time | Screen | Say (roughly) |
|---|---|---|
| 0:00–0:15 | Dashboard, waveforms breathing | "This is PulseGuard, a ventilator controller running on QNX 8.0. It breathes for a simulated patient 1000 times a second. Next to it runs an AI that diagnoses the lung every breath. The question we answer: what happens to the patient when the AI goes wrong?" |
| 0:15–0:35 | Terminal: `uname -a`, `pidin \| grep pg_`, `cat /dev/pulseguard/plant` | "This is QNX 8.0 in QEMU. Six processes: the control loop is FIFO priority 60, pinned to its own core. The AI is SCHED_SPORADIC, priority 30, with a 2 ms budget every 10 ms. The hardware is a QNX resource manager, so I can read it like a file." |
| 0:35–0:55 | Click **Stiff lung** | "Let's make the patient sick. Compliance drops, tidal volume falls. Within a few breaths the AI fits a lung model, says stiff lung, and proposes a small lung-protective change. It can't apply it. Only the clinician can." Click **Accept**. |
| 0:55–1:25 | Click **CPU stress**; zoom on the jitter tile and histogram | "Now let's attack the system. This floods every core except one with priority-55 busy loops, including the control loop's own core. The jitter doesn't move: still TBD microseconds worst case, zero deadline misses. On the same machine, the same loop on Linux hits TBD milliseconds." (Optionally cut to `rtbench` side by side.) |
| 1:25–1:55 | Stress off. Click **Rogue AI**. Terminal: `pidin \| grep aiguard` shows `5s` | "Worse: the AI goes rogue, spinning forever and proposing 80 cmH2O, a lethal pressure. The kernel's sporadic scheduler has already demoted it to priority 5; you can see 5s right here. The safety envelope clamps every proposal. And the watchdog notices the silence, kills it and restarts a clean model. The patient never noticed." |
| 1:55–2:25 | Click **Kill controller** | "Last one: we kill the control process itself. The heartbeat pulses stop. The watchdog detects it in TBD milliseconds and a new controller is breathing in TBD milliseconds, with the clinician's settings restored. During the gap, the hardware fail-safe keeps the patient safe. Here's the slog2 trail." (`pulseguard.sh log`) |
| 2:25–2:45 | README evidence table, then dashboard | "Resource managers, pulses, priority inheritance, sporadic scheduling, runmasks, procmgr abilities: QNX doesn't just run this, it's what makes it safe. Everything's open source, with 38 unit tests, a reproducible AI model, and a step-by-step codelab. PulseGuard: AI that advises, and an OS that guarantees." |

Checklist before upload:
- [ ] Dashboard shows "QNX 8.0", no yellow emulator banner
- [ ] Under 3:00
- [ ] Title: "PulseGuard: AI-safe ventilator control on QNX 8.0 (QNX Global Hackathon)"
- [ ] Visibility: **Public**
- [ ] Description links the GitHub repo
