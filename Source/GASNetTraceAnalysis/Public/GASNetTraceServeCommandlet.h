#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "GASNetTraceServeCommandlet.generated.h"

/** Loopback-only workspace API host for the GAS Net Trace web viewer. */
UCLASS()
class GASNETTRACEANALYSIS_API UGASNetTraceServeCommandlet final : public UCommandlet
{
    GENERATED_BODY()

public:
    UGASNetTraceServeCommandlet();
    virtual int32 Main(const FString& Params) override;
};
