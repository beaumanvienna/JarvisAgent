const crypto = require('crypto');

class JarvisAgentStartWorkflow {
  description = {
    displayName: 'JarvisAgent: Start Workflow',
    name: 'jarvisAgentStartWorkflow',
    group: ['transform'],
    version: 2,
    description: 'Start a JarvisAgent workflow run via webhook or legacy endpoint',
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
        displayName: 'Endpoint',
        name: 'endpoint',
        type: 'options',
        options: [
          { name: 'Webhook (recommended)', value: 'webhook' },
          { name: 'Legacy n8n/start', value: 'legacy' },
        ],
        default: 'webhook',
        description: 'Webhook: POST /api/webhook/<id>. Legacy: POST /api/integrations/n8n/start.',
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
        description: 'Legacy only. On-disk traceability folder name.',
        displayOptions: { show: { endpoint: ['legacy'] } },
      },
      {
        displayName: 'HMAC Secret',
        name: 'hmacSecret',
        type: 'string',
        typeOptions: { password: true },
        default: '',
        required: false,
        description: 'Webhook only. Shared secret for X-Webhook-Signature HMAC-SHA256 signing. Leave empty for open webhooks.',
        displayOptions: { show: { endpoint: ['webhook'] } },
      },
      {
        displayName: 'Callback URL',
        name: 'callbackUrl',
        type: 'string',
        default: '',
        required: false,
        description: 'URL to receive a completion callback POST when the run finishes.',
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
      const baseUrl = String(this.getNodeParameter('baseUrl', i)).replace(/\/$/, '');
      const endpoint = this.getNodeParameter('endpoint', i) || 'webhook';
      const workflowId = this.getNodeParameter('workflowId', i);
      const runId = this.getNodeParameter('runId', i);
      const callbackUrl = this.getNodeParameter('callbackUrl', i);
      const contextJsonText = this.getNodeParameter('contextJson', i);

      let context;
      try {
        context = contextJsonText ? JSON.parse(contextJsonText) : {};
      } catch (e) {
        throw new Error(`Invalid Context (JSON): ${e.message}`);
      }

      let url;
      let body;

      if (endpoint === 'webhook') {
        url = `${baseUrl}/api/webhook/${encodeURIComponent(workflowId)}`;
        body = { context };
        if (runId) body.runId = runId;
        if (callbackUrl) body.callbackUrl = callbackUrl;
      } else {
        url = `${baseUrl}/api/integrations/n8n/start`;
        const taskName = this.getNodeParameter('taskName', i);
        body = { workflowId, context };
        if (runId) body.runId = runId;
        if (taskName) body.taskName = taskName;
        if (callbackUrl) body.callbackUrl = callbackUrl;
      }

      const headers = { 'Content-Type': 'application/json' };

      if (endpoint === 'webhook') {
        const hmacSecret = this.getNodeParameter('hmacSecret', i) || '';
        if (hmacSecret) {
          const rawBody = JSON.stringify(body);
          const sig = crypto.createHmac('sha256', hmacSecret).update(rawBody).digest('hex');
          headers['X-Webhook-Signature'] = `sha256=${sig}`;
        }
      }

      const options = {
        method: 'POST',
        url,
        headers,
        body,
        json: true,
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
