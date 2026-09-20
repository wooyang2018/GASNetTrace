# GAS Net Trace

Trace-native GAS network causality diagnostics for Unreal Engine 5.8.

## Capture

Launch every process with the same capture GUID and a unique endpoint:

```text
-trace=gasnettrace,net,frame,cpu,object -NetTrace=1 -tracehost=127.0.0.1
-GASNetTraceCapture=<GUID> -GASNetTraceEndpoint=Server|Client0|Client1
```

The runtime emits `.utrace` events only while the `gasnettrace` channel is enabled. Shipping builds compile the project adapter and clock synchronization work to no-ops.

## Viewer

```powershell
cd Plugins/GASNetTrace/Viewer
npm install
npm run build

# Terminal 1: Analyze one or more traces and host the API.
D:\Software\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe `
  D:\Workspace\ue-gas-learn\ue-gas-learn.uproject `
  -run=GASNetTraceServe `
  '-InputTraces=D:\Traces\server.utrace;D:\Traces\client0.utrace' `
  -Port=4174

# Terminal 2: use the random token printed by the commandlet.
$env:GAS_NET_TRACE_TOKEN='<printed-token>'
npm run serve
```

Both processes bind only to `127.0.0.1`. The Viewer contains no production sample data: it uses the commandlet's authenticated, paged API. `GASNetTraceAnalyze` produces the same report headlessly.

For a reusable workspace, pass `-Workspace=D:\Traces\capture.gntworkspace` to either commandlet:

```json
{
  "schema": "gasnettrace.workspace.v2",
  "captureId": "optional-expected-capture-guid",
  "tracePaths": ["server.utrace", "client0.utrace", "client1.utrace"],
  "filters": {},
  "annotations": [],
  "thresholds": {
    "rollbackDeadlineSeconds": 0.5,
    "convergenceDeadlineSeconds": 1.0,
    "clockUncertaintySeconds": 0.01
  }
}
```

Relative trace paths resolve from the workspace file. The workspace stores paths, filters, annotations and thresholds only; it never copies trace payloads.

## Test exit

Non-Shipping test processes accept `-GASNetTraceAutoQuitAfter=<seconds>`. The world requests a normal engine exit when the timer elapses so Trace can flush its tail instead of being force-killed.

## Modules

- `GASNetTraceRuntime`: Trace channel, Observer, adapter API and owner-only clock probes.
- `GASNetTraceAnalysis`: TraceServices Analyzer/Provider, clock estimate and NetProfiler-aware report generation.
- `Viewer`: React/TypeScript Causality Workbench with a Canvas swimlane.

The plugin never repairs gameplay state and never decodes packets itself.
