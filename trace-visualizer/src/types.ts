export interface RegisterEntry {
  name: string;
  value: string;
}

export interface TraceStep {
  index: number;
  pc: string;
  insn: string;
  registers: RegisterEntry[];
}

export interface TraceWave {
  waveId: string;
  metadata: Record<string, string>;
  steps: TraceStep[];
}

export interface TraceMetadata {
  schemaVersion?: number;
  kernel?: string;
  createdAt?: number;
  maxSteps?: number;
  inferiorPid?: number;
}

export interface ParseResult {
  metadata: TraceMetadata;
  waves: TraceWave[];
  warnings: string[];
}
