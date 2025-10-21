import { ParseResult, TraceMetadata, TraceStep, TraceWave, RegisterEntry } from "../types";

function normalizePc(value: unknown): string {
  if (typeof value === "string" && value.trim()) {
    return value.trim();
  }
  if (typeof value === "number" && Number.isFinite(value)) {
    return `0x${value.toString(16)}`;
  }
  return "";
}

function normalizeInsn(value: unknown): string {
  if (typeof value === "string") {
    return value;
  }
  return "";
}

function normalizeRegister(entry: unknown, index: number, warnings: string[]): RegisterEntry | null {
  if (!entry || typeof entry !== "object") {
    warnings.push(`Register entry ${index} is not an object.`);
    return null;
  }

  const name = "name" in entry && typeof entry.name === "string" ? entry.name : null;
  const value = "value" in entry && typeof entry.value === "string" ? entry.value : null;

  if (!name) {
    warnings.push(`Register entry ${index} is missing a name.`);
    return null;
  }

  return {
    name,
    value: value ?? "",
  };
}

export function parseTraceFile(contents: string): ParseResult {
  const warnings: string[] = [];
  let parsed: unknown;

  try {
    parsed = JSON.parse(contents);
  } catch (error) {
    throw new Error("Trace file is not valid JSON.");
  }

  if (!parsed || typeof parsed !== "object") {
    throw new Error("Trace file must be a JSON object.");
  }

  const root = parsed as Record<string, unknown>;

  const metadata: TraceMetadata = {
    schemaVersion: typeof root.schema_version === "number" ? root.schema_version : undefined,
    kernel: typeof root.kernel === "string" ? root.kernel : undefined,
    createdAt: typeof root.created_at === "number" ? root.created_at : undefined,
    maxSteps: typeof root.max_steps === "number" ? root.max_steps : undefined,
    inferiorPid: typeof root.inferior_pid === "number" ? root.inferior_pid : undefined,
  };

  const wavesSrc = Array.isArray(root.waves) ? root.waves : [];
  if (!Array.isArray(root.waves)) {
    warnings.push("Trace file is missing 'waves' array; defaulting to empty.");
  }

  const waves: TraceWave[] = wavesSrc.map((wave, waveIndex) => {
    if (!wave || typeof wave !== "object") {
      warnings.push(`Wave ${waveIndex} is not an object.`);
      return {
        waveId: String(waveIndex),
        metadata: {},
        steps: [],
      };
    }

    const waveRecord = wave as Record<string, unknown>;
    const waveIdRaw = waveRecord.wave_id ?? waveIndex;
    const waveId = typeof waveIdRaw === "number" ? String(waveIdRaw) : String(waveIdRaw ?? waveIndex);

    const waveMeta: Record<string, string> = {};
    if (Array.isArray(waveRecord.ptid)) {
      waveMeta.ptid = JSON.stringify(waveRecord.ptid);
    }
    if (typeof waveRecord.total_steps === "number") {
      waveMeta.total_steps = String(waveRecord.total_steps);
    }

    const stepsSrc = Array.isArray(waveRecord.steps) ? waveRecord.steps : [];
    if (!Array.isArray(waveRecord.steps)) {
      warnings.push(`Wave ${waveId} is missing 'steps' array; defaulting to empty.`);
    }

    const steps: TraceStep[] = stepsSrc
      .map((step, stepIndex) => {
        if (!step || typeof step !== "object") {
          warnings.push(`Wave ${waveId} step ${stepIndex} is not an object.`);
          return null;
        }

        const stepRecord = step as Record<string, unknown>;
        const index = typeof stepRecord.step_index === "number" ? stepRecord.step_index : stepIndex;
        const pc = normalizePc(stepRecord.pc);
        const insn = normalizeInsn(stepRecord.insn);

        const registersSrc = Array.isArray(stepRecord.registers) ? stepRecord.registers : [];
        if (!Array.isArray(stepRecord.registers)) {
          warnings.push(`Wave ${waveId} step ${index} has no 'registers' array.`);
        }

        const registers: RegisterEntry[] = [];
        registersSrc.forEach((entry, regIndex) => {
          const reg = normalizeRegister(entry, regIndex, warnings);
          if (reg) {
            registers.push(reg);
          }
        });

        return {
          index,
          pc,
          insn,
          registers,
        };
      })
      .filter((entry): entry is TraceStep => entry !== null)
      .sort((a, b) => a.index - b.index);

    return {
      waveId,
      metadata: waveMeta,
      steps,
    };
  });

  waves.sort((a, b) => {
    const idA = Number(a.waveId);
    const idB = Number(b.waveId);
    if (!Number.isNaN(idA) && !Number.isNaN(idB)) {
      return idA - idB;
    }
    return a.waveId.localeCompare(b.waveId);
  });

  const instructionCount = waves.reduce((sum, wave) => sum + wave.steps.length, 0);
  if (instructionCount === 0 && wavesSrc.length === 0) {
    warnings.push("No wave data found in trace.");
  }

  return { metadata, waves, warnings };
}
