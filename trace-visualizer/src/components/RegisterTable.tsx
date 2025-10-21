import { FC, useMemo } from "react";
import { TraceStep } from "../types";

interface RegisterTableProps {
  step: TraceStep | null;
}

const VECTOR_NAME_PATTERN = /^v\d+/i;

function isVectorRegister(name: string): boolean {
  return VECTOR_NAME_PATTERN.test(name.trim());
}

function parseVectorValues(raw: string): string[] {
  const hexMatches = raw.match(/0x[0-9a-fA-F]+/g);
  if (hexMatches && hexMatches.length > 0) {
    return hexMatches;
  }

  const cleaned = raw.replace(/[\[\],]/g, " ");
  const parts = cleaned
    .trim()
    .split(/\s+/)
    .filter(Boolean);

  return parts.length > 1 ? parts : [];
}

function chunk<T>(values: T[], size: number): T[][] {
  const rows: T[][] = [];
  for (let i = 0; i < values.length; i += size) {
    rows.push(values.slice(i, i + size));
  }
  return rows;
}

const RegisterTable: FC<RegisterTableProps> = ({ step }) => {
  const rows = useMemo(() => step?.registers ?? [], [step]);

  if (!step) {
    return (
      <div className="section">
        <h2>Registers</h2>
        <div className="empty-state">Select a step to view register state.</div>
      </div>
    );
  }

  return (
    <div className="section">
      <h2>Registers</h2>
      <div className="table-wrapper">
        <table>
          <thead>
            <tr>
              <th>Register</th>
              <th>Value</th>
            </tr>
          </thead>
          <tbody>
            {rows.map((entry) => {
              const vector = isVectorRegister(entry.name) ? parseVectorValues(entry.value) : [];
              const isVector = vector.length > 0;
              const vectorRows = isVector ? chunk(vector, 8) : [];

              return (
                <tr key={`${step.index}_${entry.name}`}>
                  <td className="reg-name">{entry.name}</td>
                  <td className="reg-value">
                    {isVector ? (
                      <table className="vector-table">
                        <tbody>
                          {vectorRows.map((row, rowIdx) => (
                            <tr key={`${entry.name}_row_${rowIdx}`}>
                              {Array.from({ length: 8 }).map((_, colIdx) => (
                                <td key={`${entry.name}_cell_${rowIdx}_${colIdx}`}>
                                  {row[colIdx] ?? ""}
                                </td>
                              ))}
                            </tr>
                          ))}
                        </tbody>
                      </table>
                    ) : (
                      <span>{entry.value}</span>
                    )}
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
    </div>
  );
};

export default RegisterTable;
