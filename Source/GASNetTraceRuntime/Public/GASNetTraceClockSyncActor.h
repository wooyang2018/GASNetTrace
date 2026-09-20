#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GASNetTraceClockSyncActor.generated.h"

/** Owner-only non-shipping NTP-style clock probe. It observes time; it never changes game state. */
UCLASS(NotBlueprintable, Transient, NotPlaceable)
class GASNETTRACERUNTIME_API AGASNetTraceClockSyncActor final : public AActor
{
    GENERATED_BODY()

public:
    AGASNetTraceClockSyncActor();
    virtual void BeginPlay() override;

private:
    void SendSample();

    UFUNCTION(Server, Unreliable)
    void ServerRequestClockSample(uint32 Sequence, double ClientSendSeconds);

    UFUNCTION(Client, Unreliable)
    void ClientReceiveClockSample(uint32 Sequence, double ClientSendSeconds, double ServerReceiveSeconds, double ServerSendSeconds);

    FTimerHandle SampleTimer;
    uint32 NextSequence = 1;
    uint8 InitialSamplesRemaining = 8;
};
