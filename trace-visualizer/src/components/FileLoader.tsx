import { ChangeEvent, FC, useRef } from "react";

interface FileLoaderProps {
  onFileLoaded: (name: string, contents: string) => void;
  onError: (message: string) => void;
}

const FileLoader: FC<FileLoaderProps> = ({ onFileLoaded, onError }) => {
  const inputRef = useRef<HTMLInputElement | null>(null);

  const handleFileChange = async (event: ChangeEvent<HTMLInputElement>) => {
    const file = event.target.files?.[0];
    if (!file) {
      return;
    }

    try {
      const text = await file.text();
      onFileLoaded(file.name, text);
    } catch (error) {
      onError(error instanceof Error ? error.message : "Unable to read file");
    } finally {
      if (inputRef.current) {
        inputRef.current.value = "";
      }
    }
  };

  return (
    <label className="file-input">
      <input ref={inputRef} type="file" accept=".log,.txt" onChange={handleFileChange} />
      <span>Load trace.log</span>
    </label>
  );
};

export default FileLoader;
