#include "GASNetTraceClockSyncActor.h"

#include "Engine/World.h"
#include "GASNetTraceTrace.h"
#include "HAL/PlatformTime.h"
#include "TimerManager.h"

AGASNetTraceClockSyncActor::AGASNetTraceClockSyncActor()
{
    bReplicates = true;
    bOnlyRelevantToOwner = true;
    SetReplicateMovement(false);
    PrimaryActorTick.bCanEverTick = false;
}

void AGASNetTraceClockSyncActor::BeginPlay()
{
    Super::BeginPlay();
#if UE_TRACE_ENABLED && !UE_BUILD_SHIPPING
    if (!HasAuthority() && FGASNetTrace::IsEnabled())
    {
        GetWorldTimerManager().SetTimerForNextTick(this, &ThisClass::SendSample);
    }
#endif
}

void AGASNetTraceClockSyncActor::SendSample()
{
#if UE_TRACE_ENABLED && !UE_BUILD_SHIPPING
    if (!FGASNetTrace::IsEnabled()) return;
    ServerRequestClockSample(NextSequence++, FPlatformTime::Seconds());
    const float Delay = InitialSamplesRemaining > 0 ? 0.25f : 30.0f;
    if (InitialSamplesRemaining > 0) --InitialSamplesRemaining;
    GetWorldTimerManager().SetTimer(SampleTimer, this, &ThisClass::SendSample, Delay, false);
#endif
}

void AGASNetTraceClockSyncActor::ServerRequestClockSample_Implementation(uint32 Sequence, double ClientSendSeconds)
{
#if UE_TRACE_ENABLED && !UE_BUILD_SHIPPING
    const double ServerReceive = FPlatformTime::Seconds();
    const double ServerSend = FPlatformTime::Seconds();
    FGASNetTrace::EmitClockSync(Sequence, ClientSendSeconds, ServerReceive, ServerSend, 0.0);
    ClientReceiveClockSample(Sequence, ClientSendSeconds, ServerReceive, ServerSend);
#endif
}

void AGASNetTraceClockSyncActor::ClientReceiveClockSample_Implementation(
    uint32 Sequence, double ClientSendSeconds, double ServerReceiveSeconds, double ServerSendSeconds)
{
#if UE_TRACE_ENABLED && !UE_BUILD_SHIPPING
    FGASNetTrace::EmitClockSync(Sequence, ClientSendSeconds, ServerReceiveSeconds, ServerSendSeconds, FPlatformTime::Seconds());
#endif
}
