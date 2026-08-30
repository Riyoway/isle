# Isle Plugin Protocol v1

Isle deliberately does **not** load third-party DLLs into the UI process. Plugins are ordinary processes launched from `%LOCALAPPDATA%\Isle\plugins\<plugin>`, and communicate with Isle over newline-delimited JSON (NDJSON) on stdin/stdout.

That gives us four useful properties:

- a bad plugin cannot corrupt the renderer's address space;
- plugins may be written in any language that can read/write JSON lines;
- host/plugin versions are decoupled by a tiny versioned protocol;
- the UI stays native: plugins describe activities, Isle renders them consistently.

## Manifest

Each plugin directory contains `plugin.json`:

```json
{
  "id": "example.usage-meters",
  "name": "Usage meters",
  "version": "1.0.0",
  "api": 1,
  "executable": "powershell.exe",
  "args": ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "usage-meters.ps1"]
}
```

`executable` can be a file relative to the plugin directory or a command available on `PATH`.

## Lifecycle

The host launches the plugin with the additional argument `--isle-plugin` and then sends:

```json
{"type":"host.hello","api":1,"host":"Isle"}
```

The plugin can immediately publish activities. Every JSON value must fit on one line.

## Publish or update an activity

```json
{
  "type": "activity.upsert",
  "activity": {
    "id": "codex",
    "kind": "metric",
    "title": "Codex",
    "subtitle": "Weekly usage",
    "glyph": "✦",
    "accent": "#FF5A1F",
    "progress": 0.73,
    "value": 73,
    "valueSuffix": "%",
    "priority": 20,
    "pinned": false,
    "actions": [
      {"id":"refresh","label":"Refresh","glyph":"↻"}
    ]
  }
}
```

Supported `kind` values in v1 are `metric`, `status`, `timer`, `text`, and `media`.

`progress` is always normalized to `0.0 .. 1.0`. `value` is display data and has no implied range.

The host namespaces the local id internally as `<plugin-id>:<activity-id>`, so two plugins may both publish `activity.id = "status"` safely.

## Remove an activity

```json
{"type":"activity.remove","id":"codex"}
```

## Host action invocation

If an activity declares actions, Isle may send:

```json
{"type":"action.invoke","activityId":"codex","actionId":"refresh"}
```

The plugin should perform the action and publish an updated activity if its visible state changed.

## Error handling

Malformed lines are ignored and an error activity is surfaced for the plugin. When a plugin process exits, all of its activities are removed and a disconnected status is shown. A plugin crash never terminates Isle.

## Security model

v1 plugins are still normal user processes. Installing one is equivalent to running its executable manually. Isle does not auto-download or auto-update third-party plugins. A future protocol can add an AppContainer broker for plugins that only need network/filesystem capabilities declared in the manifest.
