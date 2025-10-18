import { FC, useMemo } from "react";

interface HeaderProps {
  fileName?: string;
  kernelName?: string;
  createdAt?: number;
  instructionCount: number;
  waveCount: number;
}

const Header: FC<HeaderProps> = ({ fileName, kernelName, createdAt, instructionCount, waveCount }) => {
  const timestamp = useMemo(() => {
    if (typeof createdAt !== "number") {
      return undefined;
    }
    try {
      return new Date(createdAt * 1000).toLocaleString();
    } catch (error) {
      return String(createdAt);
    }
  }, [createdAt]);

  return (
    <header className="section" style={{ display: "flex", flexDirection: "column", gap: "0.5rem" }}>
      <div style={{ display: "flex", flexWrap: "wrap", gap: "0.75rem", alignItems: "center" }}>
        <h1 style={{ margin: 0, fontSize: "1.75rem", color: "#f1f5f9" }}>AMDGPU Trace Visualizer</h1>
        {fileName && <span className="badge">{fileName}</span>}
        {kernelName && <span className="badge">kernel={kernelName}</span>}
        {timestamp && <span className="badge">captured={timestamp}</span>}
      </div>
      <div className="stats">
        <span>Waves: {waveCount}</span>
        <span>Instructions: {instructionCount}</span>
      </div>
    </header>
  );
};

export default Header;
