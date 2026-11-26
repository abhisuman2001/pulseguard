#!/bin/sh
# On-target control script (QNX 8.0).  pulseguard.sh start|stop|restart|status
DIR=$(dirname "$0")
case "$1" in
  start)
    # watchdogd spawns and supervises everything else
    "$DIR/pg_watchdogd" >"$DIR/pulseguard.log" 2>&1 &
    sleep 1
    "$0" status
    ;;
  stop)
    slay -f pg_watchdogd pg_telemetryd pg_aiguard pg_controld pg_sensord pg_stressor 2>/dev/null
    rm -f /dev/shmem/pulseguard_telemetry /dev/shmem/pulseguard_watchdog 2>/dev/null
    ;;
  restart) "$0" stop; sleep 1; "$0" start ;;
  status)
    # prio column: 60f = SCHED_FIFO 60, 30s = SCHED_SPORADIC 30 (5s when throttled)
    pidin | grep -E "pid|pg_" || echo "not running"
    [ -e /dev/pulseguard/plant ] && cat /dev/pulseguard/plant
    ;;
  log)  slog2info -w | grep -E "pg_" ;;
  *) echo "usage: $0 start|stop|restart|status|log"; exit 1 ;;
esac
