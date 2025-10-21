import { useMemo, useState } from "react";
import FileLoader from "./components/FileLoader";
import Header from "./components/Header";
import WaveSelector from "./components/WaveSelector";
import InstructionList from "./components/InstructionList";
import RegisterTable from "./components/RegisterTable";
import { parseTraceFile } from "./utils/parser";
import { TraceWave, TraceMetadata } from "./types";
import "./App.css";

function App() {
  const [fileName, setFileName] = useState<string | undefined>();
  const [metadata, setMetadata] = useState<TraceMetadata | null>(null);
  const [parseWarnings, setParseWarnings] = useState<string[]>([]);
  const [errorMessage, setErrorMessage] = useState<string | null>(null);
  const [waves, setWaves] = useState<TraceWave[]>([]);
  const [selectedWaveId, setSelectedWaveId] = useState<string | null>(null);
  const [selectedStepIndex, setSelectedStepIndex] = useState(0);

  const selectedWave = useMemo(
    () => waves.find((wave) => wave.waveId === selectedWaveId) ?? null,
    [waves, selectedWaveId]
  );

  const selectedStep = useMemo(() => {
    if (!selectedWave) {
      return null;
    }
    return selectedWave.steps[selectedStepIndex] ?? null;
  }, [selectedWave, selectedStepIndex]);

  const totalInstructions = useMemo(
    () => waves.reduce((sum, wave) => sum + wave.steps.length, 0),
    [waves]
  );

  const handleFileLoaded = (name: string, contents: string) => {
    setErrorMessage(null);
    try {
      const result = parseTraceFile(contents);
      setFileName(name);
      setMetadata(result.metadata);
      setParseWarnings(result.warnings);
      setWaves(result.waves);

      if (result.waves.length) {
        setSelectedWaveId(result.waves[0].waveId);
        setSelectedStepIndex(0);
      } else {
        setSelectedWaveId(null);
        setSelectedStepIndex(0);
      }
    } catch (error) {
      const message = error instanceof Error ? error.message : "Failed to parse trace";
      setErrorMessage(message);
      setParseWarnings([]);
      setWaves([]);
      setSelectedWaveId(null);
      setSelectedStepIndex(0);
      setMetadata(null);
    }
  };

  const handleWaveChange = (waveId: string) => {
    setSelectedWaveId(waveId);
    setSelectedStepIndex(0);
  };

  return (
    <div className="app">
      <Header
        fileName={fileName}
        kernelName={metadata?.kernel}
        waveCount={waves.length}
        instructionCount={totalInstructions}
        createdAt={metadata?.createdAt}
      />

      <div className="section" style={{ display: "flex", justifyContent: "space-between", alignItems: "center" }}>
        <FileLoader onFileLoaded={handleFileLoaded} onError={setErrorMessage} />
        {errorMessage && <span style={{ color: "#fca5a5" }}>{errorMessage}</span>}
      </div>

      {parseWarnings.length > 0 && (
        <div className="section" style={{ borderColor: "rgba(250, 204, 21, 0.4)", color: "#fde68a" }}>
          <h2>Warnings</h2>
          <ul style={{ margin: 0, paddingLeft: "1.2rem" }}>
            {parseWarnings.map((warning, idx) => (
              <li key={idx}>{warning}</li>
            ))}
          </ul>
        </div>
      )}

      <WaveSelector waves={waves} selectedWaveId={selectedWaveId} onSelect={handleWaveChange} />

      <InstructionList
        steps={selectedWave?.steps ?? []}
        selectedIndex={selectedStepIndex}
        onSelect={setSelectedStepIndex}
      />

      {selectedWave && (
        <div className="section">
          <h2>Wave metadata</h2>
          <div style={{ display: "flex", flexWrap: "wrap", gap: "0.75rem", fontSize: "0.9rem", color: "#cbd5f5" }}>
            {Object.entries(selectedWave.metadata).map(([key, value]) => (
              <span key={key} className="badge">
                {key}={value}
              </span>
            ))}
            {selectedWave.metadata.total_steps === undefined && (
              <span className="badge">total_steps={selectedWave.steps.length}</span>
            )}
          </div>
        </div>
      )}

      <RegisterTable step={selectedStep} />
    </div>
  );
}

export default App;
