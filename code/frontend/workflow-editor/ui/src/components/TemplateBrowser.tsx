import React, { useCallback, useState } from "react";
import { workflowTemplates, type WorkflowTemplate } from "../templates/workflowTemplates";
import CreateWorkflowModal from "./CreateWorkflowModal";
import type { JcwfFile } from "../jcwf/types";

export type TemplateBrowserProps = {
  onCreateFromTemplate: (workflowId: string, jcwf: JcwfFile) => void;
};

export default function TemplateBrowser(props: TemplateBrowserProps): JSX.Element
{
  const [selectedTemplate, setSelectedTemplate] = useState<WorkflowTemplate | null>(null);
  const [showCreateModal, setShowCreateModal] = useState<boolean>(false);

  const onTemplateClick = useCallback((template: WorkflowTemplate) => {
    setSelectedTemplate(template);
  }, []);

  const onCreateClick = useCallback(() => {
    if (selectedTemplate)
    {
      setShowCreateModal(true);
    }
  }, [selectedTemplate]);

  const onModalSubmit = useCallback((workflowId: string) => {
    if (!selectedTemplate)
    {
      return;
    }
    setShowCreateModal(false);

    // Clone the template JCWF and update the id
    const jcwf: JcwfFile = {
      ...selectedTemplate.jcwf,
      id: workflowId,
    };

    props.onCreateFromTemplate(workflowId, jcwf);
  }, [selectedTemplate, props.onCreateFromTemplate]);

  return (
    <div className="card" style={{ maxWidth: 860 }}>
      <div style={{ fontWeight: 700, marginBottom: 8 }}>Templates</div>
      <div className="small muted" style={{ marginBottom: 12 }}>
        Start from a pre-built workflow template.
      </div>

      <div style={{ display: "flex", gap: 12, flexWrap: "wrap" }}>
        {workflowTemplates.map((template) => (
          <button
            key={template.id}
            type="button"
            className={`btn ${selectedTemplate?.id === template.id ? "btnActive" : ""}`}
            onClick={() => onTemplateClick(template)}
            style={{ textAlign: "left", minWidth: 180, padding: "10px 14px" }}
          >
            <div style={{ fontWeight: 700 }}>{template.name}</div>
            <div className="small muted" style={{ marginTop: 2 }}>{Object.keys(template.jcwf.tasks).length} tasks</div>
          </button>
        ))}
      </div>

      {selectedTemplate
        ? (
          <div style={{ marginTop: 16, padding: 12, background: "rgba(255,255,255,0.04)", borderRadius: 6 }}>
            <div style={{ fontWeight: 700, marginBottom: 4 }}>{selectedTemplate.name}</div>
            <div className="small" style={{ marginBottom: 8 }}>{selectedTemplate.description}</div>
            <div className="small muted" style={{ marginBottom: 10 }}>
              Tasks: {Object.keys(selectedTemplate.jcwf.tasks).join(", ")}
            </div>
            <button className="btn btnPrimary" type="button" onClick={onCreateClick}>
              Create from Template
            </button>
          </div>
        )
        : null}

      <CreateWorkflowModal
        isOpen={showCreateModal}
        defaultId={selectedTemplate?.id ?? "workflow"}
        title={`Create from "${selectedTemplate?.name ?? "Template"}"`}
        submitLabel="Create"
        onCancel={() => setShowCreateModal(false)}
        onSubmit={onModalSubmit}
      />
    </div>
  );
}
