class JarvisAgentStartWorkflow {
  description = {
    displayName: 'JarvisAgent: Start Workflow',
    name: 'jarvisAgentStartWorkflow',
    group: ['transform'],
    version: 1,
    description: 'Start a JarvisAgent workflow run via POST /api/integrations/n8n/start',
    defaults: {
      name: 'JarvisAgent Start Workflow',
    },
    inputs: ['main'],
    outputs: ['main'],
    properties: [
      {
        displayName: 'JarvisAgent Base URL',
        name: 'baseUrl',
        type: 'string',
        default: 'http://localhost:8080',
        required: true,
      },
      {
        displayName: 'Workflow ID',
        name: 'workflowId',
        type: 'string',
        default: '',
        required: true,
      },
      {
        displayName: 'Run ID',
        name: 'runId',
        type: 'string',
        default: '',
        required: false,
      },
      {
        displayName: 'Task Name',
        name: 'taskName',
        type: 'string',
        default: 'n8n',
        required: false,
        description: 'Used for on-disk traceability under workflows/<workflowId>/<taskName>/...'
      },
      {
        displayName: 'Callback URL',
        name: 'callbackUrl',
        type: 'string',
        default: '',
        required: false,
      },
      {
        displayName: 'Context (JSON)',
        name: 'contextJson',
        type: 'string',
        typeOptions: {
          rows: 6,
        },
        default: '{}',
        required: false,
        description: 'JSON object of context fields. Values may be strings or nested JSON.',
      },
    ],
  };

  async execute() {
    const items = this.getInputData();
    const returnData = [];

    for (let i = 0; i < items.length; i++) {
      const baseUrl = this.getNodeParameter('baseUrl', i);
      const workflowId = this.getNodeParameter('workflowId', i);
      const runId = this.getNodeParameter('runId', i);
      const taskName = this.getNodeParameter('taskName', i);
      const callbackUrl = this.getNodeParameter('callbackUrl', i);
      const contextJsonText = this.getNodeParameter('contextJson', i);

      let context;
      try {
        context = contextJsonText ? JSON.parse(contextJsonText) : {};
      } catch (e) {
        throw new Error(`Invalid Context (JSON): ${e.message}`);
      }

      const body = {
        workflowId,
        context,
      };

      if (runId) body.runId = runId;
      if (taskName) body.taskName = taskName;
      if (callbackUrl) body.callbackUrl = callbackUrl;

      const options = {
        method: 'POST',
        uri: `${String(baseUrl).replace(/\/$/, '')}/api/integrations/n8n/start`,
        json: true,
        body,
      };

      const response = await this.helpers.httpRequest(options);

      returnData.push({
        json: {
          ...items[i].json,
          jarvisAgent: response,
        },
      });
    }

    return [returnData];
  }
}

module.exports = { JarvisAgentStartWorkflow };
