import React, { useCallback, useEffect, useRef, useState } from "react";

type Props = {
  className?: string;
  style?: React.CSSProperties;
  value: string;
  onChange: (value: string) => void;
  rows?: number;
  placeholder?: string;
  templateVariables: string[];
};

export default function TemplateTextarea({
  className,
  style,
  value,
  onChange,
  rows,
  placeholder,
  templateVariables,
}: Props): JSX.Element
{
  const textareaRef = useRef<HTMLTextAreaElement>(null);
  const dropdownRef = useRef<HTMLDivElement>(null);
  const [suggestions, setSuggestions] = useState<string[]>([]);
  const [selectedIndex, setSelectedIndex] = useState(0);
  const [triggerStart, setTriggerStart] = useState<number | null>(null);

  const closeSuggestions = useCallback(() => {
    setSuggestions([]);
    setSelectedIndex(0);
    setTriggerStart(null);
  }, []);

  const detectTrigger = useCallback(() => {
    const textarea = textareaRef.current;
    if (!textarea || templateVariables.length === 0) return;

    const cursorPos = textarea.selectionStart;
    const textBeforeCursor = value.slice(0, cursorPos);

    const lastOpen = textBeforeCursor.lastIndexOf("{{");
    if (lastOpen === -1)
    {
      closeSuggestions();
      return;
    }

    const closingAfterOpen = textBeforeCursor.indexOf("}}", lastOpen);
    if (closingAfterOpen !== -1)
    {
      closeSuggestions();
      return;
    }

    const partial = textBeforeCursor.slice(lastOpen + 2).toLowerCase();
    const filtered = templateVariables.filter((v) =>
      v.toLowerCase().startsWith(partial)
    );

    if (filtered.length > 0)
    {
      setSuggestions(filtered);
      setSelectedIndex(0);
      setTriggerStart(lastOpen);
    }
    else
    {
      closeSuggestions();
    }
  }, [value, templateVariables, closeSuggestions]);

  const insertSuggestion = useCallback((variable: string) => {
    if (triggerStart === null) return;

    const textarea = textareaRef.current;
    const cursorPos = textarea?.selectionStart ?? value.length;
    const before = value.slice(0, triggerStart);
    const after = value.slice(cursorPos);
    const inserted = `{{${variable}}}`;
    const newValue = before + inserted + after;
    onChange(newValue);
    closeSuggestions();

    requestAnimationFrame(() => {
      if (textarea)
      {
        const newPos = before.length + inserted.length;
        textarea.selectionStart = newPos;
        textarea.selectionEnd = newPos;
        textarea.focus();
      }
    });
  }, [triggerStart, value, onChange, closeSuggestions]);

  const handleKeyDown = useCallback((e: React.KeyboardEvent<HTMLTextAreaElement>) => {
    if (suggestions.length === 0) return;

    if (e.key === "ArrowDown")
    {
      e.preventDefault();
      setSelectedIndex((prev) => (prev + 1) % suggestions.length);
    }
    else if (e.key === "ArrowUp")
    {
      e.preventDefault();
      setSelectedIndex((prev) => (prev - 1 + suggestions.length) % suggestions.length);
    }
    else if (e.key === "Enter" || e.key === "Tab")
    {
      e.preventDefault();
      insertSuggestion(suggestions[selectedIndex]);
    }
    else if (e.key === "Escape")
    {
      e.preventDefault();
      closeSuggestions();
    }
  }, [suggestions, selectedIndex, insertSuggestion, closeSuggestions]);

  useEffect(() => {
    const handleClickOutside = (e: MouseEvent) => {
      if (
        dropdownRef.current &&
        !dropdownRef.current.contains(e.target as Node) &&
        textareaRef.current &&
        !textareaRef.current.contains(e.target as Node)
      )
      {
        closeSuggestions();
      }
    };
    if (suggestions.length > 0)
    {
      document.addEventListener("mousedown", handleClickOutside);
    }
    return () => document.removeEventListener("mousedown", handleClickOutside);
  }, [suggestions.length, closeSuggestions]);

  return (
    <div style={{ position: "relative" }}>
      <textarea
        ref={textareaRef}
        className={className}
        style={style}
        value={value}
        onChange={(e) => {
          onChange(e.target.value);
        }}
        onKeyUp={detectTrigger}
        onClick={detectTrigger}
        onKeyDown={handleKeyDown}
        rows={rows}
        placeholder={placeholder}
      />
      {suggestions.length > 0 && (
        <div
          ref={dropdownRef}
          className="templateDropdown"
        >
          {suggestions.map((s, i) => (
            <div
              key={s}
              className={`templateDropdownItem${i === selectedIndex ? " templateDropdownItemActive" : ""}`}
              onMouseDown={(e) => {
                e.preventDefault();
                insertSuggestion(s);
              }}
              onMouseEnter={() => setSelectedIndex(i)}
            >
              <span style={{ opacity: 0.5 }}>{"{{"}</span>{s}<span style={{ opacity: 0.5 }}>{"}}"}</span>
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
