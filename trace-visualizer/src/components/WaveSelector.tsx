import { FC } from "react";
import { TraceWave } from "../types";

interface WaveSelectorProps {
  waves: TraceWave[];
  selectedWaveId: string | null;
  onSelect: (waveId: string) => void;
}

const WaveSelector: FC<WaveSelectorProps> = ({ waves, selectedWaveId, onSelect }) => {
  if (!waves.length) {
    return (
      <div className="section">
        <h2>Select Wave</h2>
        <div className="empty-state">Load a trace to inspect available waves.</div>
      </div>
    );
  }

  return (
    <div className="section">
      <h2>Select Wave</h2>
      <select
        className="select"
        value={selectedWaveId ?? ""}
        onChange={(event) => onSelect(event.target.value)}
      >
        <option value="" disabled>
          Choose a wave
        </option>
        {waves.map((wave) => (
          <option key={wave.waveId} value={wave.waveId}>
            wave_id={wave.waveId} · steps={wave.steps.length}
          </option>
        ))}
      </select>
    </div>
  );
};

export default WaveSelector;
