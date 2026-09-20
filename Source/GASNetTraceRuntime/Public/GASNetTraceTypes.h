#pragma once

#include "CoreMinimal.h"
#include "GASNetTraceTypes.generated.h"

UENUM(BlueprintType)
enum class EGASNetTraceEventType : uint8
{
    AbilityAttempt,
    AbilityActivated,
    AbilityFailed,
    AbilityEnded,
    PredictionWindow,
    EffectAdded,
    EffectRemoved,
    EffectStack,
    TagChanged,
    AttributeChanged,
    GameplayCue,
    TargetData,
    AbilitySpec,
    ActorInfo,
    ClockSync,
    NetworkConfig
};

UENUM()
enum class EGASNetTraceIdentityKind : uint8
{
    None,
    NetworkGUID,
    IrisNetRefHandle,
    NormalizedPath
};

UENUM()
enum class EGASNetTraceConfidence : uint8
{
    None,
    Weak,
    Strong,
    Exact
};

enum class EGASNetTraceCoverage : uint32
{
    None             = 0,
    ObserverAbility  = 1 << 0,
    ObserverEffect   = 1 << 1,
    ObserverTag      = 1 << 2,
    ObserverAttribute= 1 << 3,
    AdapterAttempt   = 1 << 8,
    AdapterPrediction= 1 << 9,
    AdapterTargetData= 1 << 10,
    AdapterSpec      = 1 << 11,
    AdapterActorInfo = 1 << 12
};
ENUM_CLASS_FLAGS(EGASNetTraceCoverage);

USTRUCT(BlueprintType)
struct GASNETTRACERUNTIME_API FGASNetTraceContext
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite, Category="GAS Net Trace") int32 AbilitySpecHandle = INDEX_NONE;
    UPROPERTY(BlueprintReadWrite, Category="GAS Net Trace") int32 PredictionCurrent = 0;
    UPROPERTY(BlueprintReadWrite, Category="GAS Net Trace") int32 PredictionBase = 0;
    UPROPERTY(BlueprintReadWrite, Category="GAS Net Trace") int64 PredictiveConnectionKey = 0;
    UPROPERTY(BlueprintReadWrite, Category="GAS Net Trace") int32 ConnectionId = 0;
};
