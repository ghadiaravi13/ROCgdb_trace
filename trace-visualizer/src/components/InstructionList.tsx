import { FC } from "react";
import { TraceStep } from "../types";

interface InstructionListProps {
  steps: TraceStep[];
  selectedIndex: number;
  onSelect: (index: number) => void;
}

const InstructionList: FC<InstructionListProps> = ({ steps, selectedIndex, onSelect }) => {
  if (!steps.length) {
    return (
      <div className="section">
        <h2>Instructions</h2>
        <div className="empty-state">Load a trace and select a wave to inspect steps.</div>
      </div>
    );
  }

  const current = steps[selectedIndex];

  return (
    <div className="section">
      <h2>Instructions</h2>
      <select
        className="select"
        value={selectedIndex}
        onChange={(event) => onSelect(Number(event.target.value))}
      >
        {steps.map((step, idx) => (
          <option key={step.index} value={idx}>
            #{idx.toString().padStart(3, "0")} | pc={step.pc} | {step.insn || "<no insn>"}
          </option>
        ))}
      </select>
      {current && (
        <div style={{ marginTop: "0.75rem", fontSize: "0.9rem", color: "#cbd5f5" }}>
          <div>pc: {current.pc}</div>
          <div>insn: {current.insn || "<no insn>"}</div>
        </div>
      )}
    </div>
  );
};

export default InstructionList;
