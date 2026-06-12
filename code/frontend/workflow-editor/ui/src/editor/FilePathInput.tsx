import React, { useEffect, useRef } from "react";

type Props = {
  value: string;
  onChange: (value: string) => void;
  className?: string;
  style?: React.CSSProperties;
  placeholder?: string;
  listId?: string;
  onKeyDown?: (e: React.KeyboardEvent<HTMLInputElement>) => void;
};

// A controlled text input for file paths. A long path clips horizontally; for a file path the
// meaningful part (the filename) is at the END, so we keep the right-hand edge in view whenever the
// field isn't being edited — scrolling only while unfocused so it never fights the caret. The full
// path is also exposed in a hover title tooltip.
export default function FilePathInput(props: Props): React.ReactElement
{
  const { value, onChange, className, style, placeholder, listId, onKeyDown } = props;
  const ref = useRef<HTMLInputElement>(null);

  const scrollToEndIfUnfocused = (el: HTMLInputElement | null) => {
    if (el && document.activeElement !== el)
    {
      el.scrollLeft = el.scrollWidth;
    }
  };

  // Re-apply when the value changes (e.g. selecting a different node loads a new path).
  useEffect(() => {
    scrollToEndIfUnfocused(ref.current);
  }, [value]);

  return (
    <input
      ref={ref}
      className={className ?? "input"}
      style={style}
      value={value}
      title={value || undefined}
      placeholder={placeholder}
      list={listId}
      onChange={(e) => onChange(e.target.value)}
      onKeyDown={onKeyDown}
      onBlur={(e) => { e.currentTarget.scrollLeft = e.currentTarget.scrollWidth; }}
    />
  );
}
