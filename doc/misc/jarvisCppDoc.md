# jarvisCppDocu — header → cpp pairing review

Walked `engine/` and `application/` — found 139 headers and 117 cpp files.
Pairing rule: a header pairs with the .cpp of the same stem in the same directory.

| Header (.h) | Paired cpp (same dir, same stem) |
|---|---|
| `application/application.h` |  |
| `application/assistant/assistantController.h` | `application/assistant/assistantController.cpp` |
| `application/assistant/assistantMemory.h` | `application/assistant/assistantMemory.cpp` |
| `application/assistant/assistantSession.h` | `application/assistant/assistantSession.cpp` |
| `application/assistant/assistantTools.h` | `application/assistant/assistantTools.cpp` |
| `application/assistant/contextAssembler.h` | `application/assistant/contextAssembler.cpp` |
| `application/assistant/workspaceIndexer.h` | `application/assistant/workspaceIndexer.cpp` |
| `application/cloud/azureBlobCloudTaskExecutor.h` | `application/cloud/azureBlobCloudTaskExecutor.cpp` |
| `application/cloud/azureBlobConnector.h` | `application/cloud/azureBlobConnector.cpp` |
| `application/cloud/azureSharedKeySigner.h` | `application/cloud/azureSharedKeySigner.cpp` |
| `application/cloud/cloudCircuitBreaker.h` | `application/cloud/cloudCircuitBreaker.cpp` |
| `application/cloud/cloudConnectionManager.h` | `application/cloud/cloudConnectionManager.cpp` |
| `application/cloud/cloudConnectionPool.h` | `application/cloud/cloudConnectionPool.cpp` |
| `application/cloud/cloudConnector.h` |  |
| `application/cloud/cloudConnectorRegistry.h` | `application/cloud/cloudConnectorRegistry.cpp` |
| `application/cloud/cloudRetryPolicy.h` | `application/cloud/cloudRetryPolicy.cpp` |
| `application/cloud/cloudTaskExecutor.h` | `application/cloud/cloudTaskExecutor.cpp` |
| `application/cloud/dbQueryCloudTaskExecutor.h` | `application/cloud/dbQueryCloudTaskExecutor.cpp` |
| `application/cloud/emailCloudTaskExecutor.h` | `application/cloud/emailCloudTaskExecutor.cpp` |
| `application/cloud/emailConnector.h` | `application/cloud/emailConnector.cpp` |
| `application/cloud/gcsCloudTaskExecutor.h` | `application/cloud/gcsCloudTaskExecutor.cpp` |
| `application/cloud/gcsConnector.h` | `application/cloud/gcsConnector.cpp` |
| `application/cloud/gitHubCloudTaskExecutor.h` | `application/cloud/gitHubCloudTaskExecutor.cpp` |
| `application/cloud/gitHubConnector.h` | `application/cloud/gitHubConnector.cpp` |
| `application/cloud/googleSheetsCloudTaskExecutor.h` | `application/cloud/googleSheetsCloudTaskExecutor.cpp` |
| `application/cloud/googleSheetsConnector.h` | `application/cloud/googleSheetsConnector.cpp` |
| `application/cloud/jiraCloudTaskExecutor.h` | `application/cloud/jiraCloudTaskExecutor.cpp` |
| `application/cloud/jiraConnector.h` | `application/cloud/jiraConnector.cpp` |
| `application/cloud/oneDriveCloudTaskExecutor.h` | `application/cloud/oneDriveCloudTaskExecutor.cpp` |
| `application/cloud/oneDriveConnector.h` | `application/cloud/oneDriveConnector.cpp` |
| `application/cloud/polarionConnector.h` | `application/cloud/polarionConnector.cpp` |
| `application/cloud/polarionWriteTaskExecutor.h` | `application/cloud/polarionWriteTaskExecutor.cpp` |
| `application/cloud/postgresConnector.h` | `application/cloud/postgresConnector.cpp` |
| `application/cloud/providerRateLimitPolicy.h` | `application/cloud/providerRateLimitPolicy.cpp` |
| `application/cloud/redmineCloudTaskExecutor.h` | `application/cloud/redmineCloudTaskExecutor.cpp` |
| `application/cloud/redmineConnector.h` | `application/cloud/redmineConnector.cpp` |
| `application/cloud/s3CloudTaskExecutor.h` | `application/cloud/s3CloudTaskExecutor.cpp` |
| `application/cloud/s3Connector.h` | `application/cloud/s3Connector.cpp` |
| `application/cloud/sigV4Signer.h` | `application/cloud/sigV4Signer.cpp` |
| `application/cloud/slackCloudTaskExecutor.h` | `application/cloud/slackCloudTaskExecutor.cpp` |
| `application/cloud/slackConnector.h` | `application/cloud/slackConnector.cpp` |
| `application/cloud/snowflakeCloudTaskExecutor.h` | `application/cloud/snowflakeCloudTaskExecutor.cpp` |
| `application/cloud/snowflakeConnector.h` | `application/cloud/snowflakeConnector.cpp` |
| `application/cloud/taskCancellationToken.h` |  |
| `application/content/chunkPlanner.h` | `application/content/chunkPlanner.cpp` |
| `application/content/markdownSectionSplitter.h` | `application/content/markdownSectionSplitter.cpp` |
| `application/file/fileWatcher.h` | `application/file/fileWatcher.cpp` |
| `application/file/scriptRegistry.h` | `application/file/scriptRegistry.cpp` |
| `application/jarvisAgent.h` | `application/jarvisAgent.cpp` |
| `application/json/jcwfGenerationGuide.generated.h` |  |
| `application/json/jcwfSchema.generated.h` |  |
| `application/json/jsonObjectParser.h` | `application/json/jsonObjectParser.cpp` |
| `application/json/replyParser.h` | `application/json/replyParser.cpp` |
| `application/json/replyParserAPI1.h` | `application/json/replyParserAPI1.cpp` |
| `application/json/replyParserAPI2.h` | `application/json/replyParserAPI2.cpp` |
| `application/json/replyParserAPI3.h` | `application/json/replyParserAPI3.cpp` |
| `application/json/replyParserAPI4.h` | `application/json/replyParserAPI4.cpp` |
| `application/json/replyParserAPI5.h` | `application/json/replyParserAPI5.cpp` |
| `application/json/requestBuilder.h` | `application/json/requestBuilder.cpp` |
| `application/json/schemaValidator.h` | `application/json/schemaValidator.cpp` |
| `application/log/statusRenderer.h` | `application/log/statusRenderer.cpp` |
| `application/python/pythonEngine.h` | `application/python/pythonEngine.cpp` |
| `application/python/pythonEnginePool.h` | `application/python/pythonEnginePool.cpp` |
| `application/session/fileWriter.h` | `application/session/fileWriter.cpp` |
| `application/task/carMaintenanceTask.h` | `application/task/carMaintenanceTask.cpp` |
| `application/task/internalTaskRegistry.h` |  |
| `application/task/taskBase.h` |  |
| `application/web/aiJcwfService.h` | `application/web/aiJcwfService.cpp` |
| `application/web/mcpKeyManager.h` | `application/web/mcpKeyManager.cpp` |
| `application/web/webServer.h` | `application/web/webServer.cpp`, `application/web/webServer_helpers.h`, `application/web/webServer_studio.cpp` |
| `application/web/webSessionManager.h` | `application/web/webSessionManager.cpp` |
| `application/workflow/adhocWorkflowManager.h` | `application/workflow/adhocWorkflowManager.cpp` |
| `application/workflow/aiCallEvents.h` |  |
| `application/workflow/aiCallTaskExecutor.h` | `application/workflow/aiCallTaskExecutor.cpp` |
| `application/workflow/aiInvocation.h` |  |
| `application/workflow/aiReply.h` |  |
| `application/workflow/aiRequestPool.h` | `application/workflow/aiRequestPool.cpp` |
| `application/workflow/aiTranscript.h` | `application/workflow/aiTranscript.cpp` |
| `application/workflow/dataflowResolver.h` | `application/workflow/dataflowResolver.cpp` |
| `application/workflow/filter/filterEngine.h` | `application/workflow/filter/filterEngine.cpp` |
| `application/workflow/filter/filterManifest.h` | `application/workflow/filter/filterManifest.cpp` |
| `application/workflow/filter/polarionClient.h` | `application/workflow/filter/polarionClient.cpp` |
| `application/workflow/filter/queryParser.h` | `application/workflow/filter/queryParser.cpp` |
| `application/workflow/internalTaskExecutor.h` | `application/workflow/internalTaskExecutor.cpp` |
| `application/workflow/jcwfContainer.h` | `application/workflow/jcwfContainer.cpp` |
| `application/workflow/pythonTaskExecutor.h` | `application/workflow/pythonTaskExecutor.cpp` |
| `application/workflow/scriptCatalog.h` | `application/workflow/scriptCatalog.cpp` |
| `application/workflow/shellTaskExecutor.h` | `application/workflow/shellTaskExecutor.cpp` |
| `application/workflow/subWorkflowTaskExecutor.h` | `application/workflow/subWorkflowTaskExecutor.cpp` |
| `application/workflow/taskExecutor.h` |  |
| `application/workflow/taskExecutorRegistry.h` | `application/workflow/taskExecutorRegistry.cpp` |
| `application/workflow/taskFreshnessChecker.h` | `application/workflow/taskFreshnessChecker.cpp` |
| `application/workflow/taskPathResolver.h` | `application/workflow/taskPathResolver.cpp` |
| `application/workflow/templateEngine.h` | `application/workflow/templateEngine.cpp` |
| `application/workflow/triggerEngine.h` | `application/workflow/triggerEngine.cpp` |
| `application/workflow/workflowFileIndex.h` | `application/workflow/workflowFileIndex.cpp` |
| `application/workflow/workflowJsonParser.h` | `application/workflow/workflowJsonParser.cpp` |
| `application/workflow/workflowJsonParserDetails.h` | `application/workflow/workflowJsonParserDetails.cpp` |
| `application/workflow/workflowRegistry.h` | `application/workflow/workflowRegistry.cpp` |
| `application/workflow/workflowRuntimeManager.h` | `application/workflow/workflowRuntimeManager.cpp` |
| `application/workflow/workflowTriggerBinder.h` | `application/workflow/workflowTriggerBinder.cpp` |
| `application/workflow/workflowTypes.h` |  |
| `application/workflow/workflowValidator.h` | `application/workflow/workflowValidator.cpp` |
| `engine/auxiliary/file.h` | `engine/auxiliary/file.cpp` |
| `engine/auxiliary/threadPool.h` | `engine/auxiliary/threadPool.cpp` |
| `engine/core.h` | `engine/core.cpp` |
| `engine/curlWrapper/authSigner.h` | `engine/curlWrapper/authSigner.cpp` |
| `engine/curlWrapper/awsSigV4.h` | `engine/curlWrapper/awsSigV4.cpp` |
| `engine/curlWrapper/curlManager.h` |  |
| `engine/curlWrapper/curlMultiDispatcher.h` | `engine/curlWrapper/curlMultiDispatcher.cpp` |
| `engine/curlWrapper/curlWrapper.h` | `engine/curlWrapper/curlWrapper.cpp` |
| `engine/curlWrapper/rateLimitController.h` | `engine/curlWrapper/rateLimitController.cpp` |
| `engine/curlWrapper/rateLimitObservation.h` |  |
| `engine/curlWrapper/rateLimitStrategy.h` | `engine/curlWrapper/rateLimitStrategy.cpp` |
| `engine/engine.h` | `engine/engine.cpp` |
| `engine/event/applicationEvent.h` |  |
| `engine/event/engineEvent.h` |  |
| `engine/event/event.h` |  |
| `engine/event/eventQueue.h` | `engine/event/eventQueue.cpp` |
| `engine/event/events.h` |  |
| `engine/event/filesystemEvent.h` |  |
| `engine/event/keyboardEvent.h` |  |
| `engine/event/pythonErrorEvent.h` |  |
| `engine/event/timerEvent.h` |  |
| `engine/input/keyboardInput.h` | `engine/input/keyboardInput.cpp` |
| `engine/json/configChecker.h` | `engine/json/configChecker.cpp` |
| `engine/json/configParser.h` | `engine/json/configParser.cpp` |
| `engine/json/jsonHelper.h` | `engine/json/jsonHelper.cpp` |
| `engine/keys/credential.h` |  |
| `engine/keys/jwtGenerator.h` | `engine/keys/jwtGenerator.cpp` |
| `engine/keys/keyEncryption.h` | `engine/keys/keyEncryption.cpp` |
| `engine/keys/keyManager.h` | `engine/keys/keyManager.cpp` |
| `engine/keys/oauthTokenManager.h` | `engine/keys/oauthTokenManager.cpp` |
| `engine/keys/secureString.h` | `engine/keys/secureString.cpp` |
| `engine/log/log.h` | `engine/log/log.cpp` |
| `engine/log/secretRedactor.h` | `engine/log/secretRedactor.cpp` |
| `engine/log/terminalLogStreamBuf.h` |  |
| `engine/log/terminalManager.h` | `engine/log/terminalManager.cpp` |
| `engine/auxiliary/TracyClient.cpp` |  |
| `engine/entryPoint.cpp` |  |
