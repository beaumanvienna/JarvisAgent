import React, { useCallback, useEffect, useState } from "react";

export type CreateWorkflowModalProps = {
  isOpen: boolean;
  defaultId?: string;
  title?: string;
  submitLabel?: string;
  onSubmit: (workflowId: string) => void;
  onCancel: () => void;
};

function isValidWorkflowId(id: string): boolean
{
  if (id.length === 0)
  {
    return false;
  }
  // Allow alphanumeric, underscore, hyphen. Must start with letter or underscore.
  return /^[a-zA-Z_][a-zA-Z0-9_-]*$/.test(id);
}

export default function CreateWorkflowModal(props: CreateWorkflowModalProps): JSX.Element | null
{
  const [inputValue, setInputValue] = useState<string>(props.defaultId ?? "");
  const [validationError, setValidationError] = useState<string | null>(null);

  useEffect(() => {
    if (props.isOpen)
    {
      setInputValue(props.defaultId ?? "");
      setValidationError(null);
    }
  }, [props.isOpen, props.defaultId]);

  const validate = useCallback((value: string): string | null => {
    const trimmed = value.trim();
    if (trimmed.length === 0)
    {
      return "Workflow id is required.";
    }
    if (!isValidWorkflowId(trimmed))
    {
      return "Id must start with a letter or underscore, and contain only letters, numbers, underscores, or hyphens.";
    }
    return null;
  }, []);

  const onInputChange = useCallback((e: React.ChangeEvent<HTMLInputElement>) => {
    const value = e.target.value;
    setInputValue(value);
    setValidationError(validate(value));
  }, [validate]);

  const onSubmitClick = useCallback(() => {
    const trimmed = inputValue.trim();
    const error = validate(trimmed);
    if (error)
    {
      setValidationError(error);
      return;
    }
    props.onSubmit(trimmed);
  }, [inputValue, validate, props.onSubmit]);

  const onKeyDown = useCallback((e: React.KeyboardEvent) => {
    if (e.key === "Enter")
    {
      onSubmitClick();
    }
    else if (e.key === "Escape")
    {
      props.onCancel();
    }
  }, [onSubmitClick, props.onCancel]);

  if (!props.isOpen)
  {
    return null;
  }

  const title = props.title ?? "Create Workflow";
  const submitLabel = props.submitLabel ?? "Create";

  return (
    <div className="modalBackdrop" onClick={props.onCancel}>
      <div className="modalContent" onClick={(e) => e.stopPropagation()} onKeyDown={onKeyDown}>
        <div className="modalHeader">{title}</div>

        <div className="modalBody">
          <label style={{ display: "block", marginBottom: 6 }}>
            Workflow ID
          </label>
          <input
            type="text"
            value={inputValue}
            onChange={onInputChange}
            placeholder="my_workflow"
            style={{ width: "100%", padding: "8px 10px", boxSizing: "border-box" }}
            autoFocus
          />
          {validationError
            ? <div className="errorText" style={{ marginTop: 6 }}>{validationError}</div>
            : null}
        </div>

        <div className="modalFooter">
          <button className="btn" type="button" onClick={props.onCancel}>
            Cancel
          </button>
          <button className="btn btnPrimary" type="button" onClick={onSubmitClick}>
            {submitLabel}
          </button>
        </div>
      </div>
    </div>
  );
}
