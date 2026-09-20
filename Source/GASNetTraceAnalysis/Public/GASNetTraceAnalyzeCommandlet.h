#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "GASNetTraceAnalyzeCommandlet.generated.h"

/** Headless entry point. Accepts -InputTrace=<file> or semicolon-separated -InputTraces=<files>. */
UCLASS()
class GASNETTRACEANALYSIS_API UGASNetTraceAnalyzeCommandlet final : public UCommandlet
{
    GENERATED_BODY()

public:
    UGASNetTraceAnalyzeCommandlet();
    virtual int32 Main(const FString& Params) override;
};
