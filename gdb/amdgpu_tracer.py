# amdgpu_tracer.py
# Usage:
#   (rocgdb) source /path/to/amdgpu_tracer.py
#   (rocgdb) amdgpu_trace MY_KERNEL out=/tmp/trace.log max_steps=0
#
# Notes:
# - max_steps=0 means unlimited; otherwise per-wave cap to avoid runaway loops.
# - Traces each GPU wave by single-stepping one instruction at a time and
#   dumping: wave_id, PC, disasm, and all readable registers.

import gdb
import time

def _is_gpu_thread(t: gdb.InferiorThread) -> bool:
    # ROCgdb marks GPU waves with lwp==1 (and pid!=1).
    try:
        pid, lwp, tid = t.ptid
        return lwp == 1 and pid != 1
    except Exception:
        return False

def _get_wave_id() -> int:
    # Prefer the convenience var if present; fallback to ptid.tid
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

def _read_all_registers(frame: gdb.Frame) -> list[tuple[str, str]]:
    regs = []
    arch = frame.architecture()
    try:
        reg_names = arch.registers()
    except Exception:
        # Fallback: common core names; you can extend if needed
        reg_names = [ "pc", "sp", "scc", "vcc", "exec" ]
    for rn in reg_names:
        if not rn:
            continue
        try:
            v = frame.read_register(rn)
            # Represent as hex if possible
            try:
                regs.append((rn, v.format_string("x")))
            except Exception:
                regs.append((rn, str(v)))
        except Exception:
            # Register may not exist for this wave mode; skip
            continue
    return regs

def _disassemble_one(pc: int, arch: gdb.Architecture) -> tuple[str, int]:
    # Returns (text, length). Length is important for sanity checks.
    try:
        insns = arch.disassemble(pc, count=1)
        if not insns:
            return ("<unavailable>", 0)
        ins = insns[0]
        # ins has keys: 'addr', 'asm', 'length' (recent GDBs)
        text = f"{ins.get('asm','<asm?>')}"
        length = int(ins.get('length', 0))
        return (text, length)
    except Exception:
        return ("<disasm error>", 0)

def _wave_alive(t: gdb.InferiorThread) -> bool:
    try:
        # Re-check membership in thread list to avoid stale handles
        inf = gdb.selected_inferior()
        for tt in inf.threads():
            if tt.ptid == t.ptid:
                return True
        return False
    except Exception:
        return False

def _at_endpgm(disasm_text: str) -> bool:
    # AMDGPU end of program mnemonic
    return "s_endpgm" in disasm_text

def _select_thread(t: gdb.InferiorThread):
    try:
        t.switch()
    except gdb.error as e:
        raise

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
                out_path = a.split("=",1)[1]
            elif a.startswith("max_steps="):
                try: max_steps = int(a.split("=",1)[1])
                except: pass

        if not out_path:
            ts = int(time.time())
            out_path = f"/tmp/amdgpu_trace_{ts}.log"

        gdb.write(f"[amdgpu_trace] kernel={kernel} out={out_path} max_steps={max_steps}\n")

        # 1) Break at kernel entry and run to it
        gdb.execute(f"break {kernel}", to_string=True)
        gdb.execute("continue", to_string=True)

        # 2) On stop, collect GPU waves
        inf = gdb.selected_inferior()
        gpu_threads = [t for t in inf.threads() if _is_gpu_thread(t)]
        if not gpu_threads:
            gdb.write("[amdgpu_trace] No GPU waves found at kernel entry.\n")
            return

        with open(out_path, "w") as outf:
            outf.write(f"# AMDGPU instruction trace for {kernel}\n")

            # Iterate each wave independently
            for t in gpu_threads:
                if not _wave_alive(t):
                    continue

                # Select wave, then step/log until termination or limit
                steps = 0
                while _wave_alive(t) and (max_steps <= 0 or steps < max_steps):
                    try:
                        _select_thread(t)
                    except Exception:
                        break

                    frame = gdb.selected_frame()
                    arch = frame.architecture()
                    # PC may be named 'pc' (AMDGPU tdep sets it)
                    try:
                        pc_val = int(frame.read_register("pc"))
                    except Exception:
                        # Fallback via $pc
                        pc_val = int(gdb.parse_and_eval("$pc"))

                    disasm_text, insn_len = _disassemble_one(pc_val, arch)
                    wid = _get_wave_id()

                    # Dump instruction and registers
                    outf.write(f"\nwave_id={wid} pc=0x{pc_val:x} insn=\"{disasm_text}\"\n")
                    regs = _read_all_registers(frame)
                    for rn, rv in regs:
                        outf.write(f"{rn}={rv} ")
                    outf.write("\n")
                    outf.flush()

                    if _at_endpgm(disasm_text):
                        break

                    # Single-step one instruction for this wave
                    try:
                        gdb.execute("stepi", to_string=True)
                    except gdb.error as e:
                        # If the wave terminated or stepping failed, stop tracing this wave
                        break

                    steps += 1

        gdb.write(f"[amdgpu_trace] Trace complete. Output: {out_path}\n")

AMDGPUTrace()