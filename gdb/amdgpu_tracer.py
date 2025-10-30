# amdgpu_tracer.py
# Usage:
#   (rocgdb) source /path/to/amdgpu_tracer.py
#   (rocgdb) amdgpu_trace MY_KERNEL out=/tmp/trace.json max_steps=0
#
# Notes:
# - max_steps=0 means unlimited; otherwise per-wave cap to avoid runaway loops.
# - Traces each GPU wave by single-stepping one instruction at a time and
#   dumping: wave_id, PC, disasm, and all readable registers.

import gdb
import json
import time

import os
# Redirect stderr to suppress ROCgdb messages
old_stderr = sys.stderr
sys.stderr = open(os.devnull, 'w')

REG_CHANGE_RECORD = {}


def _is_gpu_thread(t: gdb.InferiorThread) -> bool:
    """Detect ROCm GPU wave threads."""
    try:
        pid, lwp, tid = t.ptid
        return lwp == 1 and pid != 1
    except Exception:
        return False


def _get_wave_id() -> int:
    """Retrieve the current wave identifier if GDB exposes it."""
    try:
        wid = int(gdb.parse_and_eval("$_wave_id"))
        if wid != 0:
            return wid
    except Exception:
        pass
    try:
        pid, lwp, tid = gdb.selected_thread().ptid
        return int(tid)
    except Exception:
        return -1


def _read_all_changed_registers(frame: gdb.Frame) -> list[tuple[str, str]]:
    regs: list[tuple[str, str]] = []
    arch = frame.architecture()
    try:
        reg_names = arch.registers()
    except Exception:
        reg_names = ["pc", "sp", "scc", "vcc", "exec"]

    for rn in reg_names:
        if rn is None:
            continue
        name = str(rn).strip()
        if not name:
            continue
        # if frame.name() in REG_CHANGE_RECORD and rn not in REG_CHANGE_RECORD[frame.name()]:
        #     gdb.write(f"[amdgpu_trace] Skipping unchanged register: {name}\n")
        #     continue
        try:
            value = frame.read_register(name)
            if name not in REG_CHANGE_RECORD or str(value) != REG_CHANGE_RECORD[name]:
                REG_CHANGE_RECORD[name] = str(value)
                try:
                    regs.append((name, value.format_string("x")))
                except Exception:
                    regs.append((name, str(value)))
        except Exception:
            continue

        # Clear record of this register change after reading
        # REG_CHANGE_RECORD[frame.name()].remove(rn) if frame.name() in REG_CHANGE_RECORD and rn in REG_CHANGE_RECORD[frame.name()] else None
    return regs


def _disassemble_one(pc: int, arch: gdb.Architecture) -> tuple[str, int]:
    try:
        insns = arch.disassemble(pc, count=1)
        if not insns:
            return ("<unavailable>", 0)
        ins = insns[0]
        return (ins.get("asm", "<asm?>"), int(ins.get("length", 0)))
    except Exception:
        return ("<disasm error>", 0)


def _wave_alive(t: gdb.InferiorThread) -> bool:
    try:
        inf = gdb.selected_inferior()
        return any(tt.ptid == t.ptid for tt in inf.threads())
    except Exception:
        return False


def _at_endpgm(disasm_text: str) -> bool:
    return "s_endpgm" in disasm_text


def _select_thread(t: gdb.InferiorThread):
    t.switch()

# def reg_change_handler(event):
#     gdb.write(f"[amdgpu_trace] reg_change_handler invoked for frame {getattr(event.frame, 'name', lambda: '<unknown>')()} regnum={getattr(event, 'regnum', '<unknown>')}\n")
#     if not (hasattr(event, "frame") or hasattr(event, "regnum")):
#         raise gdb.GdbError("reg_change_handler: event missing required attributes")
#     else:
#         if event.frame.name() in REG_CHANGE_RECORD:
#             REG_CHANGE_RECORD[event.frame.name()].append(event.regnum)
#         else:
#             gdb.write(f"[amdgpu_trace] Register change detected in frame {event.frame.name()}: regnum={event.regnum}\n")
#             REG_CHANGE_RECORD[event.frame.name()] = [event.regnum]


class AMDGPUTrace(gdb.Command):
    def __init__(self):
        super().__init__("amdgpu_trace", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        argv = [a for a in arg.split() if a.strip()]
        if not argv:
            raise gdb.GdbError("amdgpu_trace <kernel> [out=<file>] [max_steps=N]")

        kernel = argv[0]
        out_path = None
        max_steps = 0

        for a in argv[1:]:
            if a.startswith("out="):
                out_path = a.split("=", 1)[1]
            elif a.startswith("max_steps="):
                try:
                    max_steps = int(a.split("=", 1)[1])
                except Exception:
                    pass

        if not out_path:
            ts = int(time.time())
            out_path = f"/tmp/amdgpu_trace_{ts}.json"
        elif not out_path.endswith(".json"):
            out_path = f"{out_path}.json"

        gdb.write(f"[amdgpu_trace] kernel={kernel} out={out_path} max_steps={max_steps}\n")

        # Suppress ALL output
        gdb.execute("set pagination off", to_string=True)
        gdb.execute("set breakpoint pending on", to_string=True)
        gdb.execute("set verbose off", to_string=True)
        gdb.execute("set trace-commands off", to_string=True)
        gdb.execute("set print thread-events off", to_string=True)
        gdb.execute("set print inferior-events off", to_string=True)
        
        # Redirect GDB output to /dev/null
        gdb.execute("set logging file /dev/null", to_string=True)
        gdb.execute("set logging redirect on", to_string=True)
        gdb.execute("set logging overwrite on", to_string=True)
        gdb.execute("set logging enabled on", to_string=True)
        
        gdb.execute("set step-mode off", to_string=True)
        gdb.execute("set confirm off", to_string=True)

        gdb.execute(f"break {kernel}", to_string=True)#, quiet=True=True)
        gdb.execute("continue", to_string=True)#, quiet=True=True)

        scheduler_locked = False
        try:
            gdb.execute("set scheduler-lock on", to_string=True)#, to_string=True)#, quiet=True=True)
            scheduler_locked = True
        except gdb.error:
            gdb.write("[amdgpu_trace] Warning: failed to enable scheduler-lock.\n")

        trace_data: dict[str, object] = {
            "schema_version": 1,
            "kernel": kernel,
            "created_at": int(time.time()),
            "max_steps": max_steps,
            "waves": [],
        }

        try:
            inf = gdb.selected_inferior()
            if getattr(inf, "pid", None) is not None:
                trace_data["inferior_pid"] = inf.pid
        except Exception:
            inf = None

        if inf is None:
            with open(out_path, "w", encoding="utf-8") as outf:
                json.dump(trace_data, outf, indent=2)
            if scheduler_locked:
                try:
                    gdb.execute("set scheduler-lock off", to_string=True)#, to_string=True)#, quiet=True=True)
                except gdb.error:
                    pass
            gdb.write(f"[amdgpu_trace] Trace complete. Output: {out_path}\n")
            return

        gpu_threads = [t for t in inf.threads() if _is_gpu_thread(t)]
        gdb.write(f"[amdgpu_trace] Found {len(gpu_threads)} GPU waves at kernel entry.\n")

        alive_gpu_threads = [t for t in gpu_threads if _wave_alive(t)]
        gdb.write(f"[amdgpu_trace] {len(alive_gpu_threads)} GPU waves are alive at kernel entry.\n")

        if not gpu_threads:
            gdb.write(
                f"[amdgpu_trace] No GPU waves found at kernel entry. Inf: {inf.main_name} threads: {gpu_threads}\n"
            )
            with open(out_path, "w", encoding="utf-8") as outf:
                json.dump(trace_data, outf, indent=2)
            if scheduler_locked:
                try:
                    gdb.execute("set scheduler-lock off", to_string=True)#, quiet=True=True)
                except gdb.error:
                    pass
            gdb.write(f"[amdgpu_trace] Trace complete. Output: {out_path}\n")
            return

        try:
            for i, t in enumerate(gpu_threads):
                #reset register change record
                REG_CHANGE_RECORD.clear()

                num_alive = sum(1 for tt in gpu_threads if _wave_alive(tt))
                gdb.write(
                    f"[amdgpu_trace] Tracing wave {i + 1}/{len(gpu_threads)} (alive waves remaining: {num_alive})...\n"
                )
                if not _wave_alive(t):
                    gdb.write(f"[amdgpu_trace] Wave {i} not alive at start of tracing; skipping.\n")
                    continue

                wave_steps = []
                wave_id_value = None
                steps = 0
                ptid = list(getattr(t, "ptid", ()))

                while _wave_alive(t) and (max_steps <= 0 or steps < max_steps):
                    try:
                        _select_thread(t)
                    except Exception:
                        break

                    frame = gdb.selected_frame()
                    arch = frame.architecture()
                    try:
                        pc_val = int(frame.read_register("pc"))
                    except Exception:
                        pc_val = int(gdb.parse_and_eval("$pc"))

                    disasm_text, _ = _disassemble_one(pc_val, arch)
                    wid = _get_wave_id()
                    if wave_id_value is None:
                        wave_id_value = wid

                    regs = _read_all_changed_registers(frame)
                    wave_steps.append(
                        {
                            "step_index": steps,
                            "frame_name": frame.name(),
                            "pc": f"0x{pc_val:x}",
                            "insn": disasm_text,
                            "registers": [
                                {"name": rn, "value": rv}
                                for rn, rv in regs
                            ],
                        }
                    )

                    if _at_endpgm(disasm_text):
                        break

                    try:
                        gdb.execute("stepi", to_string=True)#, quiet=True=True)
                    except gdb.error:
                        break

                    steps += 1

                trace_data["waves"].append(
                    {
                        "wave_id": wave_id_value if wave_id_value is not None else _get_wave_id(),
                        "ptid": ptid,
                        "total_steps": len(wave_steps),
                        "steps": wave_steps,
                    }
                )
        finally:
            if scheduler_locked:
                try:
                    gdb.execute("set scheduler-lock off", to_string=True)#, quiet=True=True)
                except gdb.error:
                    pass
            sys.sterr = old_stderr  # Restore stderr

        with open(out_path, "w", encoding="utf-8") as outf:
            json.dump(trace_data, outf, indent=2)

        gdb.write(f"[amdgpu_trace] Trace complete. Output: {out_path}\n")


AMDGPUTrace()
