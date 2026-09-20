#pragma once

#include "CoreMinimal.h"
#include "GASNetTraceTypes.h"

class UAbilitySystemComponent;
class UObject;

class GASNETTRACERUNTIME_API FGASNetTrace
{
public:
    static bool IsEnabled();
    static const FGuid& GetCaptureId();
    static const FString& GetEndpointId();

    static void EmitCoverage(const UAbilitySystemComponent* ASC, EGASNetTraceCoverage Coverage);
    static void EmitEvent(
        EGASNetTraceEventType Type,
        const UAbilitySystemComponent* ASC,
        const UObject* Subject,
        const FGASNetTraceContext& Context = {},
        FStringView Detail = FStringView(),
        double ValueA = 0.0,
        double ValueB = 0.0,
        uint8 Flags = 0);

    static void EmitClockSync(uint32 Sequence, double ClientSend, double ServerReceive, double ServerSend, double ClientReceive);
    static void EmitNetworkConfig(const UObject* WorldContext);
};

#if UE_TRACE_ENABLED && !UE_BUILD_SHIPPING
#define GAS_NET_TRACE_EVENT(Type, ASC, Subject, Context, Detail) \
    do { if (FGASNetTrace::IsEnabled()) { FGASNetTrace::EmitEvent(Type, ASC, Subject, Context, Detail); } } while (false)
#else
#define GAS_NET_TRACE_EVENT(Type, ASC, Subject, Context, Detail) do { } while (false)
#endif
