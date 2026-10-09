#!/usr/bin/env bash
# rt-manipulation-check.sh — prove the host intervention actually happened, per arm.
#
# WHY THIS EXISTS
#
# The IRQ-affinity experiment of 2026-09-15/16 ran 16 arms over 80 minutes and produced an
# uninterpretable null: cpu 3's interrupt count was ~290 000 in BOTH arms, and the write test
# had hit IRQ 1, a legacy ISA line that answers EPERM. Nothing in that run demonstrated that
# the thing under test was ever in force. A null result from an intervention nobody showed
# to have occurred is not evidence about the intervention.
#
# So every arm of the RT re-test gets a before/after snapshot, and the comparison prints the
# DELTAS rather than the totals. Totals are useless here: cpu 3 may carry 486 000 rescheduling
# IPIs accumulated since boot and still be idle for the whole arm.
#
# WHAT IT CHECKS, AND WHAT EACH CHECK FALSIFIES
#
#   1. isolated / nohz_full / rcu_nocbs  — is the reservation actually on the command line?
#      If `isolated` is empty the arm measures an ordinary shared core and must be discarded.
#   2. .tick_stopped per CPU             — is the 1000 Hz tick really off the reserved cores?
#      Read from /proc/timer_list, which is the kernel's own state, so it is a fact rather
#      than an inference. This is the check the old run lacked, and it is the *point* of the
#      whole host change: the tick and the per-CPU IPIs were 98.8% of cpu 3's interrupts and
#      no runtime affinity write can move them.
#      The arch_timer delta is reported alongside it as corroboration, NOT as the test. IRQ 13
#      counts every arch-timer interrupt including hrtimers, and perf's event multiplexing
#      arms a ~1 kHz hrtimer on the CPU it profiles -- so a profiling arm shows ~1000
#      arch_timer/s on a core whose tick is provably stopped. The first version of this file
#      called that "the tick is RUNNING", which was wrong; see the note at the snapshot.
#   3. IPI0 (rescheduling) and IPI5 (IRQ work) deltas — the other unsteerable sources.
#   4. the audio IRQ, matched BY NAME  — its number, its affinity READ BACK, its kernel
#      thread's priority, and its delivered count per CPU. IRQ numbers on this board are
#      allocated in probe order under rp1_irq_chip and have been seen to move across a single
#      reboot (138 -> 139), so a check that pins a number can pass while watching the wrong
#      device. The name is stable.
#   5. governor / frequency / thermal / throttled — a throttled or hot run is a different
#      machine and its numbers are void.
#
# WHAT IT DELIBERATELY DOES NOT DO
#
# It changes nothing. Every other script in this directory manipulates the host; this one is
# the evidence that someone else's manipulation took effect, so it must be safe to run at any
# time, including while the board is mid-soak.
#
# Usage:
#   rt-manipulation-check.sh snapshot FILE
#   rt-manipulation-check.sh compare BEFORE AFTER
#   rt-manipulation-check.sh arm PREFIX COMMAND [ARG...]
#       snapshot PREFIX.pre, run COMMAND, snapshot PREFIX.post, write the comparison to
#       PREFIX.isolation, and exit with COMMAND's status.
#
# Exit: 0 on success, 2 usage, 3 when the comparison finds the intervention NOT in force
# (only ever from `compare`/`arm`, and reported so an arm can be discarded rather than
# silently quoted).

set -u

# The audio controller's interrupt, by name. Overridable because it is a property of which
# USB port the interface is on, and it differs per board: piv's SSL 2+ is on xhci-hcd:usb2,
# and an interface that lands on an EHCI or dwc2 controller reports one of the other names.
# Set AUDIO_IRQ_NAME to override. Left unset, the first of the candidates below that is
# actually present in /proc/interrupts wins, and if none is, the snapshot records that fact
# instead of silently reporting "(line not found)" for the one interrupt that matters.
AUDIO_IRQ_CANDIDATES="${AUDIO_IRQ_CANDIDATES:-${AUDIO_IRQ_NAME:-xhci-hcd:usb2 ehci_hcd:usb3 ehci_hcd:usb1 dwc2_hsotg:usb1 dwc2_hsotg:usb2}}"

resolve_audio_irq_name() {
  local n
  for n in $AUDIO_IRQ_CANDIDATES; do
    if awk -v name="$n" '$NF == name { found = 1 } END { exit !found }' /proc/interrupts; then
      printf '%s' "$n"
      return 0
    fi
  done
  printf '%s' "${AUDIO_IRQ_CANDIDATES%% *}"
}
AUDIO_IRQ_NAME="$(resolve_audio_irq_name)"

die() { echo "rt-manipulation-check: $*" >&2; exit 2; }

# CPU count taken from the header row of /proc/interrupts. Scanning every numeric field
# instead picks up the chip's hwirq from the trailing columns ("rp1_irq_chip 36 Edge") and
# invents a phantom CPU -- a bug that was actually written once in the rtpi rig.
cpu_count() { awk 'NR == 1 { print NF; exit }' /proc/interrupts; }

# A stable key for an interrupt line: its first field without the colon ("IPI0", "13").
irq_key() { printf '%s' "${1%:}"; }

# ---------------------------------------------------------------------------- snapshot
do_snapshot() {
  local out="$1"
  local n nframes
  nframes=$(cpu_count)

  {
    echo "timestamp=$(date -Is)"
    echo "uptime_s=$(cut -d' ' -f1 /proc/uptime)"
    echo "kernel_release=$(uname -r)"
    echo "kernel_version=$(uname -v)"
    # /sys/kernel/realtime DOES exist on Ubuntu's raspi-realtime flavour and reads 1. It is an
    # out-of-tree artifact that never reached mainline, so its ABSENCE is not a negative --
    # the version banner is the portable check.
    echo "uname_says_rt=$(uname -v | grep -q PREEMPT_RT && echo true || echo false)"
    echo "sys_kernel_realtime=$(cat /sys/kernel/realtime 2>/dev/null || echo absent)"
    echo "cmdline=$(cat /proc/cmdline)"
    echo "nproc=$(nproc)"
    # The reservation is a property of the command line, not of the config, so it cannot be
    # recovered from a config hash and must be recorded per run.
    echo "isolated_cpus=$(cat /sys/devices/system/cpu/isolated 2>/dev/null || echo '')"
    echo "nohz_full_cpus=$(cat /sys/devices/system/cpu/nohz_full 2>/dev/null || echo '')"
    echo "governor=$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_governor 2>/dev/null || echo unknown)"
    echo "scaling_max_khz=$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq 2>/dev/null || echo unknown)"
    echo "scaling_cur_khz=$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq 2>/dev/null || echo unknown)"
    # hwmon0 is cpu_thermal on this board and needs no privilege; vcgencmd needs root
    # (/dev/vcio is 0600 root) and fails with a message that reads like broken firmware, so
    # it degrades to "unavailable" rather than to a scary error.
    echo "temp_mC=$(cat /sys/class/hwmon/hwmon0/temp1_input 2>/dev/null || cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo unknown)"
    if command -v vcgencmd >/dev/null 2>&1; then
      echo "throttled=$(sudo -n vcgencmd get_throttled 2>/dev/null | sed 's/^throttled=//' || echo unavailable)"
    else
      echo "throttled=no-vcgencmd"
    fi
    echo "loadavg=$(cat /proc/loadavg)"
    echo "audio_card=$(aplay -l 2>/dev/null | grep -i 'SSL 2' | head -1 | sed 's/^/ /' || echo unknown)"

    # --- the scheduler tick, per CPU, from the kernel's own state ------------------
    # /proc/timer_list exposes ".tick_stopped" per CPU: the direct answer to "is the
    # scheduler tick off here", which is the question this script exists to answer.
    #
    # The arch_timer delta below is NOT that answer, and treating it as one was a real bug
    # in the first version of this file. IRQ 13 counts every arch-timer interrupt, which
    # includes hrtimers -- and perf's event multiplexing arms a ~1 kHz hrtimer on the CPU it
    # profiles. A perf arm therefore measured ~920 arch_timer/s on cpu 3 while
    # .tick_stopped was 1, and the old wording called that "the tick is RUNNING". It was not.
    # The counter stays because it is useful (it is how a *stray* tick or an unexpected
    # hrtimer source shows up), but the verdict reads .tick_stopped.
    local tl
    tl=$(sudo -n cat /proc/timer_list 2>/dev/null)
    if [[ -n $tl ]]; then
      printf '%s\n' "$tl" | awk '
        /^cpu: / { c = $2 }
        /\.nohz[[:space:]]*:/ { printf "nohz_cpu%s=%s\n", c, $3 }
        /\.tick_stopped[[:space:]]*:/ { printf "tick_stopped_cpu%s=%s\n", c, $3 }
      '
    else
      # Unreadable without root is normal when the arm is not root; the verdict then falls
      # back to the interrupt counter and says so.
      echo "tick_stopped_cpu0="; echo "tick_stopped_cpu1="
      echo "tick_stopped_cpu2="; echo "tick_stopped_cpu3="
    fi

    # --- the audio interrupt, by name ---------------------------------------------
    local irq
    irq=$(awk -v name="$AUDIO_IRQ_NAME" '$NF == name { sub(/:$/, "", $1); print $1; exit }' \
          /proc/interrupts)
    echo "audio_irq_name=$AUDIO_IRQ_NAME"
    if [[ -z $irq ]]; then
      echo "audio_irq_number="
      echo "audio_irq_affinity="
      echo "audio_irq_thread="
    else
      echo "audio_irq_number=$irq"
      # Read back: not every interrupt controller honours the write, and an accepted-but-
      # ignored write is indistinguishable from success unless it is read.
      echo "audio_irq_affinity=$(cat /proc/irq/$irq/smp_affinity_list 2>/dev/null || echo unreadable)"
      local tid
      tid=$(ps -eLo tid,comm= 2>/dev/null | awk -v p="irq/$irq-" '$2 ~ "^"p { print $1; exit }')
      if [[ -n $tid ]]; then
        echo "audio_irq_thread=$tid"
        echo "audio_irq_thread_prio=$(chrt -p "$tid" 2>/dev/null | awk '/priority/ { print $NF }' | tail -1)"
      else
        # No thread is normal on a non-RT kernel, where forced threading is not in force.
        echo "audio_irq_thread="
        echo "audio_irq_thread_prio="
      fi
    fi

    # --- every interrupt line, per CPU --------------------------------------------
    # Generic on purpose: a fixed list would need editing whenever the board enumerates
    # something new, and the interesting deltas are not always the ones anticipated.
    awk -v n="$nframes" '
      NR == 1 { next }
      {
        key = $1; sub(/:$/, "", key)
        desc = ""
        for (i = n + 2; i <= NF; i++) desc = desc (i > n + 2 ? " " : "") $i
        printf "label %s %s\n", key, desc
        for (c = 2; c <= n + 1; c++) printf "int %s cpu%d %s\n", key, c - 2, $c
      }
    ' /proc/interrupts
  } > "$out" 2>/dev/null

  [[ -s $out ]] || die "could not write snapshot $out"
}

# ---------------------------------------------------------------------------- compare
# Echo <key> for the tick source, the two IPI sources and the audio IRQ, by DESCRIPTION, so
# the keys do not have to be hard-coded to numbers that move.
find_key() {
  local file="$1" want="$2"
  awk -v want="$want" '
    $1 == "label" && index($0, want) { print $2; exit }
  ' "$file"
}

rate_per_s() {
  local delta="$1" secs="$2"
  awk -v d="$delta" -v s="$secs" 'BEGIN { printf (s > 0) ? "%.1f" : "n/a", (s > 0) ? d / s : 0 }'
}

# field FILE KEY -> the value of "KEY=..." in a snapshot
field() { awk -F= -v k="$2" '$1 == k { sub(/^[^=]*=/, ""); print; exit }' "$1"; }

# count FILE KEY CPU -> that interrupt line's count on that CPU
count() {
  awk -v k="$2" -v c="cpu$3" '$1 == "int" && $2 == k && $3 == c { print $4; exit }' "$1"
}

# delta BEFORE AFTER KEY CPU -> count(after) - count(before), 0 when a line is absent
delta() {
  local a b
  a=$(count "$1" "$3" "$4")
  b=$(count "$2" "$3" "$4")
  echo $(( ${b:-0} - ${a:-0} ))
}

do_compare() {
  local before="$1" after="$2"
  [[ -r $before && -r $after ]] || die "need two readable snapshots"

  local t0 t1 secs
  t0=$(field "$before" timestamp)
  t1=$(field "$after" timestamp)
  # date(1) on both ends: arithmetic on the ISO strings is not portable to older coreutils,
  # and this runs on whatever the board ships.
  secs=$(( $(date -d "$t1" +%s) - $(date -d "$t0" +%s) ))
  (( secs > 0 )) || secs=0

  local k v
  echo "=== host state ==="
  echo "period           $t0 -> $t1 (${secs}s)"
  for k in kernel_release uname_says_rt cmdline isolated_cpus nohz_full_cpus governor \
           scaling_max_khz scaling_cur_khz temp_mC throttled audio_card audio_irq_name \
           audio_irq_number audio_irq_affinity audio_irq_thread_prio; do
    v=$(field "$after" "$k")
    printf '%-16s %s\n' "$k" "${v:-<absent>}"
  done

  # --- the deltas ----------------------------------------------------------------
  # Looked up deliberately, not by a word-split list: "IRQ work interrupts" as a shell word
  # splits into "IRQ" and "work", and "IRQ" then matches some unrelated line's description
  # while looking perfectly plausible in the output. That bug was written and caught here.
  local tick_key ipi0_key ipi1_key ipi5_key audio_key
  tick_key=$(find_key "$after" arch_timer)
  ipi0_key=IPI0; ipi1_key=IPI1; ipi5_key=IPI5
  audio_key=$(field "$after" audio_irq_number)

  echo
  echo "=== interrupt deltas per CPU (${secs}s) ==="
  printf '%-18s %10s %10s %10s %10s\n' "source" cpu0 cpu1 cpu2 cpu3
  local spec label key c
  for spec in "arch_timer:tick" "IPI0_resched:ipi0" "IPI5_irqwork:ipi5" "audio_IRQ:audio"; do
    label="${spec%%:*}"
    case "${spec##*:}" in
      tick)  key="$tick_key" ;;
      ipi0)  key="$ipi0_key" ;;
      ipi5)  key="$ipi5_key" ;;
      audio) key="$audio_key" ;;
    esac
    if [[ -z $key ]]; then printf '%-18s %s\n' "$label" "(line not found)"; continue; fi
    printf '%-18s' "$label"
    for c in 0 1 2 3; do printf '%10s' "$(delta "$before" "$after" "$key" "$c")"; done
    echo
  done
  # IPI1 (function call) is reported separately because it is the one IPI a driver can cause
  # in bulk -- a USB submission storm shows up here and nowhere else.
  if [[ -n $ipi1_key ]]; then
    printf '%-18s' "IPI1_fncall"
    for c in 0 1 2 3; do printf '%10s' "$(delta "$before" "$after" "$ipi1_key" "$c")"; done
    echo
  fi

  # --- the verdict: on every reserved CPU, is the tick still running? --------------
  echo
  echo "=== verdict ==="
  local isolated verdict_rc=0
  isolated=$(field "$after" isolated_cpus)
  # "2-3" and "2,3" both occur; expand rather than assuming one form.
  local cpu_list
  cpu_list=$(awk -v s="$isolated" 'BEGIN {
    n = split(s, parts, ",")
    for (i = 1; i <= n; i++) {
      if (split(parts[i], r, "-") == 2) { for (c = r[1]; c <= r[2]; c++) printf "%s ", c }
      else printf "%s ", parts[i]
    }
  }')
  if [[ -z $isolated ]]; then
    # On a host that has no cores to spare, no isolation is the correct configuration rather
    # than a failure -- a 4-core board running a full userspace may deliberately decline
    # NO_HZ_FULL. EXPECT_NO_ISOLATION=1 says "I know, and this arm is
    # still the experiment". Without it, an unisolated host is reported as a failure, because
    # on a host that *can* isolate, an arm that silently lost its reservation measures a
    # different machine than the one being reported.
    if [[ ${EXPECT_NO_ISOLATION:-0} == 1 ]]; then
      echo "ok:   no CPUs are isolated, and that is this host's expected configuration."
      echo "      No reservation is being claimed; the RT kernel's scheduler is the only"
      echo "      protection on this arm, so say that when quoting it."
    else
      echo "FAIL: no CPUs are isolated -- this arm measured an ordinary shared core."
      echo "      The kernel command line is missing isolcpus=. Check: cat /proc/cmdline"
      echo "      (If this host has no cores to spare and that is deliberate, re-run the arm"
      echo "      with EXPECT_NO_ISOLATION=1.)"
      verdict_rc=3
    fi
  else
    local d r stopped
    for c in $cpu_list; do
      # The verdict reads the kernel's own tick state. The arch_timer rate is reported next
      # to it as corroboration, not as the test: it also counts hrtimers.
      stopped=$(field "$after" "tick_stopped_cpu$c")
      if [[ $stopped == 1 ]]; then
        echo "ok:   cpu $c .tick_stopped=1 -- the scheduler tick is stopped on this core."
      elif [[ $stopped == 0 ]]; then
        echo "FAIL: cpu $c .tick_stopped=0 -- the scheduler tick is RUNNING on a reserved"
        echo "      core. nohz_full suppresses it only while exactly one task is runnable, so"
        echo "      a second runnable task here brings it back. Discard this arm, or move the"
        echo "      competing task to the other reserved core."
        verdict_rc=3
      else
        # Either /proc/timer_list was unreadable, or this is an older snapshot.
        if [[ -n $tick_key ]]; then
          d=$(delta "$before" "$after" "$tick_key" "$c")
          r=$(rate_per_s "$d" "$secs")
          if awk -v r="$r" 'BEGIN { exit !(r >= 100) }'; then
            echo "UNKNOWN: .tick_stopped unavailable and cpu $c took $d arch_timer"
            echo "      interrupts in ${secs}s ($r/s). That is consistent with a running tick"
            echo "      AND with a ~1 kHz hrtimer on this core (a profiler attached to it does"
            echo "      exactly this), so it cannot decide the question. Re-run with root."
            verdict_rc=3
          else
            echo "ok:   cpu $c took $d arch_timer interrupts in ${secs}s ($r/s), and"
            echo "      .tick_stopped was unavailable -- consistent with a stopped tick."
          fi
        else
          echo "UNKNOWN: cannot tell whether the tick is off on cpu $c."
          verdict_rc=3
        fi
      fi
    done
    # The arch_timer delta, always reported: it is how an unexpected interrupt source on a
    # reserved core becomes visible, even when .tick_stopped says the tick is stopped.
    if [[ -n $tick_key ]]; then
      for c in $cpu_list; do
        d=$(delta "$before" "$after" "$tick_key" "$c")
        echo "info: cpu $c arch_timer delta $d in ${secs}s ($(rate_per_s "$d" "$secs")/s)."
        echo "      On a tickless core this is normally 0; a nonzero value is an hrtimer (or a"
        echo "      tick that came back), not proof of either."
      done
    fi
    if [[ -n $audio_key ]]; then
      for c in $cpu_list; do
        d=$(delta "$before" "$after" "$audio_key" "$c")
        if (( d > 0 )); then
          echo "ok:   the audio IRQ ($audio_key) was delivered $d times to reserved cpu $c,"
          echo "      at $(rate_per_s "$d" "$secs")/s -- the interface's data path is landing"
          echo "      where it was pinned rather than being accepted and ignored."
        fi
      done
    fi
    echo "ok:   isolated=$isolated nohz_full=$(field "$after" nohz_full_cpus)"
  fi
  return $verdict_rc
}

# ---------------------------------------------------------------------------- arm
do_arm() {
  local prefix="$1"; shift
  [[ $# -ge 1 ]] || die "arm needs a command"

  do_snapshot "$prefix.pre"
  "$@"
  local status=$?
  do_snapshot "$prefix.post"

  do_compare "$prefix.pre" "$prefix.post" > "$prefix.isolation" 2>&1
  cat "$prefix.isolation"

  # The arm's own exit status is what matters to the caller (it is the soak's gate); the
  # isolation verdict is reported separately so a failed arm and an unproven arm are not
  # confused. A non-zero verdict is echoed in the file either way.
  return "$status"
}

case "${1:-}" in
  snapshot) shift; [[ $# -eq 1 ]] || die "snapshot FILE"; do_snapshot "$1" ;;
  compare)  shift; [[ $# -eq 2 ]] || die "compare BEFORE AFTER"; do_compare "$1" "$2" ;;
  arm)      shift; [[ $# -ge 2 ]] || die "arm PREFIX COMMAND [ARG...]"; do_arm "$@" ;;
  *)        sed -n '2,60p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
