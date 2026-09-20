#pragma once

#include "CoreMinimal.h"

class FJsonObject;

struct GASNETTRACEANALYSIS_API FGASNetTraceDiagnosticThresholds
{
    double RollbackDeadlineSeconds = 0.5;
    double ConvergenceDeadlineSeconds = 1.0;
    double ActorInfoDeadlineSeconds = 0.5;
    double AttributeStableSeconds = 0.5;
    double CueWindowSeconds = 0.5;
    double VerdictDeadlineSeconds = 1.0;
    double NetworkProcessingDeadlineSeconds = 0.25;
    double ReplicationDeadlineSeconds = 1.0;
    double AttributeAbsoluteTolerance = 0.01;
    double AttributeRelativeTolerance = 0.001;
    double RejectionRateThreshold = 0.20;
    double RollbackRateThreshold = 0.10;
    double ConvergenceP95Seconds = 0.5;
    double ClockUncertaintySeconds = 0.010;
    int32 MinimumRateSamples = 20;

    static FGASNetTraceDiagnosticThresholds FromJson(const TSharedPtr<FJsonObject>& Json);
    TSharedRef<FJsonObject> ToJson() const;
};

struct GASNETTRACEANALYSIS_API FGASNetTraceWorkspaceResult
{
    bool bSucceeded = false;
    FString CaptureId;
    FString Error;
    FString Json;
    int64 EventCount = 0;
};

/** Host-independent workspace core shared by commandlets and the localhost service. */
class GASNETTRACEANALYSIS_API FGASNetTraceWorkspaceAnalyzer
{
public:
    static FGASNetTraceWorkspaceResult AnalyzeFiles(const TArray<FString>& TracePaths,
        const FGASNetTraceDiagnosticThresholds& Thresholds = {});
    static FGASNetTraceWorkspaceResult AnalyzeWorkspaceFile(const FString& WorkspacePath);
};
