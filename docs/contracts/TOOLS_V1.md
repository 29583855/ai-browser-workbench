# 工具契约目录 v1（生成文件）

日期：2026-11-05。程序：0.1.13-poc。CLI/MCP/HTTP均复用C++注册表；HTTP路由为 POST /v1/tools/{工具名}。详细参数见 tools-v1.json 或 openapi-v1.json。

| 工具 | 用途 | 必填参数 |
| --- | --- | --- |
| capabilities | Report implemented technical-preview capabilities and unsupported features |  |
| doctor | Read component and local ownership health without starting browsers or changing quotas |  |
| admin_policy | Read the local development and commercial-preview policy without changing state |  |
| admin_summary | Read local profile, running-browser and free-capacity counters for the management UI |  |
| commerce_entitlements | Read local free counters or connected control-plane entitlements |  |
| commerce_quote | Preview whether a requested environment/concurrency/proxy scope is free or billable | environmentCount |
| account_connect | Login to control plane and enforce global commercial admission | url, email, password |
| account_connect_code | Exchange a one-time SSO desktop connection code and bind this device | url, code |
| account_disconnect | Logout only after every tracked browser is stopped |  |
| account_status | Read protected connection metadata or current server entitlement |  |
| account_action | Execute a customer portal action with server-side tenant and role checks | action, body |
| profile_list | List owned profiles; active by default, or choose a lifecycle state |  |
| profile_create | Create an isolated development profile with explicit origin scope | name, allowedOrigins |
| profile_group_list | List local versioned profile library resources |  |
| profile_group_create | Create a local profile group or configuration-only template | name, profileIds |
| profile_group_update | Replace a local resource using optimistic revision control | name, profileIds, id, expectedRevision |
| profile_group_delete | Delete a local resource without deleting member profiles | id, expectedRevision |
| profile_template_list | List local versioned profile library resources |  |
| profile_template_create | Create a local profile group or configuration-only template | name, configuration |
| profile_template_update | Replace a local resource using optimistic revision control | name, configuration, id, expectedRevision |
| profile_template_delete | Delete a local resource without deleting member profiles | id, expectedRevision |
| profile_template_instantiate | Create a new isolated profile from a pinned local template revision | id, expectedRevision, name |
| profile_template_history | Read immutable retained template revisions without modifying profiles | id |
| profile_template_rollback | Copy a retained template revision into a new current revision | id, expectedRevision, sourceRevision |
| profile_export | Export configurations only; cookies, credentials and local identities are excluded | profileIds |
| profile_import | Prevalidate and atomically create 1..32 profiles from a versioned configuration bundle | bundle |
| profile_batch | Prevalidate bounded lifecycle/start/stop operations and report each result without automatic retry | operations |
| profile_batch_list | Page durable active or archived batch receipts without replaying actions |  |
| profile_batch_get | Read durable item states after disconnect or restart; no automatic replay | batchId |
| profile_batch_archive | Compact confirmed terminal batch details while preserving request deduplication | batchId, confirmBatchId |
| profile_update | Update a stopped profile using optimistic revision control | profileId, expectedRevision |
| profile_archive | Archive a stopped, unreferenced active profile without deleting its data | profileId, expectedRevision |
| profile_trash | Move a stopped, unreferenced profile into the recycle bin | profileId, expectedRevision |
| profile_restore | Restore an archived or recycled profile with its original identity and data | profileId, expectedRevision |
| profile_purge | Permanently delete recycled data; repeat the exact profile ID to confirm | profileId, expectedRevision, confirmProfileId |
| browser_start | Start a new owned browser process tree | profileId |
| browser_status | Read process ownership and handshake version | profileId |
| browser_stop | Stop only this managed process tree; no payment or lease required | profileId |
| lease_acquire | Acquire exclusive local automatic control | profileId, owner, ttlMs |
| lease_revoke | Revoke prior control generation before manual takeover | profileId |
| browser_navigate | Navigate within the explicit profile origin scope | url, profileId, lease |
| page_snapshot | Read a bounded main-frame accessibility tree with document-bound refs | profileId, lease |
| page_locate | Locate exactly one CSS element in the main document | selector, profileId, lease |
| page_discover | Find bounded visible function candidates without clicking or reading field values | profileId, lease |
| page_inspect | Inspect a unique element and create action drafts without reading field values | selector, profileId, lease |
| page_preview | Observe current action conditions without scrolling or dispatching page actions | ref, action, profileId, lease |
| page_clear_highlight | Clear the managed page debug highlight without changing page styles | profileId, lease |
| page_pick_start | Select an element with Chromium native overlay; selection click is intercepted | profileId, lease |
| page_pick_read | Read selected main-document element and unique CSS candidates without clicking | profileId, lease |
| page_pick_stop | Stop element selection without replaying any click; no lease required | profileId |
| page_fill | Fill an input without echoing or logging its value | ref, value, profileId, lease |
| page_upload | Bind one bounded local file to a unique file input without reading its contents | ref, path, profileId, lease |
| page_download_config | Configure a managed download directory for the current page | directory, profileId, lease |
| page_download_wait | Wait for a unique tracked download completion event and verify its ordinary file | fileName, timeoutMs, profileId, lease |
| page_fill_secret | Fill using an origin/profile scoped SecretRef without returning the value | ref, credentialId, profileId, lease |
| page_click | Dispatch click; business success must be asserted separately | ref, profileId, lease |
| page_select | Select exact, unambiguous native option values without echoing them | ref, values, profileId, lease |
| page_text | Read bounded element text for assertions | ref, profileId, lease |
| page_wait_text | Retry read-only observation until exact text matches | selector, expected, timeoutMs, profileId, lease |
| page_assert | Assert URL, text, element count or control state with structured evidence | assertion, profileId, lease |
| page_screenshot | Capture authorized page as PNG base64 | profileId, lease |
| page_list | List managed pages and adopt authorized popups of managed pages | profileId, lease |
| page_open | Open and select a managed page inside the allowed origin scope | url, profileId, lease |
| page_switch | Select a managed page; old element references are invalidated | pageId, profileId, lease |
| page_close | Close one managed page without stopping the owned browser | pageId, profileId, lease |
| browser_events | Poll bounded metadata events with explicit cursor gaps; no live subscription | profileId, lease |
| browser_reconnect | Explicitly reconnect the same owned process without replaying page actions | profileId, lease |
| browser_diagnostics_start | Start bounded console, exception and network metadata observation before actions | profileId, lease |
| browser_diagnostics_read | Read metadata diagnostics and step associations; payloads are omitted | profileId, lease |
| browser_diagnostics_stop | Stop collecting diagnostics and retain the bounded in-memory records | profileId, lease |
| recorder_start | Observe main-document actions as parameterized metadata; no field values | profileId, lease |
| recorder_read | Read bounded recording events and an unvalidated draft; no page actions | profileId, lease |
| recorder_control | Pause, resume or stop and detach the main-document recorder | operation, profileId, lease |
| workflow_repair_draft | Review one selector replacement bound to the exact source hash; never publish or replay | definition, expectedSha256, stepId, selector, profileId, lease |
| page_extract | Read public main-document scalar or bounded string-array data using an explicit Schema dialect | schema, bindings, profileId, lease |
| page_batch | Prevalidate and execute ordered actions; unknown outcomes force a stop without replay | actions, profileId, lease |
| workflow_validate | Validate bounded workflow DSL, graph, input types, capabilities and optional input | definition |
| workflow_publish | Publish a validated immutable workflow version with its SHA-256 | definition |
| workflow_list | List local immutable workflow versions |  |
| workflow_get | Read an owned published workflow definition | workflowId |
| workflow_run | Start a fixed workflow version asynchronously; disconnect does not cancel it | workflowId, profileId, lease, input |
| workflow_status | Read secret-free run checkpoints and actual step outcomes | runId |
| workflow_runs | Find local run IDs after a client disconnect without replaying business actions |  |
| workflow_debug | Configure exact flow/step breakpoints and inspect redacted variables only at paused boundaries | runId, options |
| workflow_control | Pause at a step boundary, resume a paused run or cancel without business replay | runId, operation |
| credential_create | Store a scoped secret protected by Windows current-user DPAPI; never returns its value | name, value, purpose, allowedOrigins, profileIds |
| credential_list | List credential metadata only; no plaintext or ciphertext |  |
| credential_rotate | Rotate a secret using optimistic revision control; existing scope remains fixed | credentialId, expectedRevision, value |
| credential_delete | Delete a credential using optimistic revision control | credentialId, expectedRevision |
| credential_audit | Read bounded secret-free credential access audit with monotonically increasing sequence |  |
| model_configure | Configure an HTTPS or loopback OpenAI-compatible completion endpoint with durable conservative budgets | expectedRevision, endpoint, model, timeoutMs, maxCalls, maxOutputTokens, maxTotalTokens |
| model_status | Read model configuration and secret-free durable budget and outcome history |  |
| model_generate_workflow | Generate and validate a workflow draft without publishing or executing it; no automatic retry | task, allowedOrigins |
| site_package_preview | Validate data-only site package, capabilities, routes, workflow hashes and review import differences | package |
| site_package_import | Install an immutable validated site package version using optimistic version control; does not run workflows | package, expectedLatestVersion |
| site_package_list | List installed site package versions and hashes |  |
| site_package_export | Export one exact installed data-only package version and its hash | packageId, version |
