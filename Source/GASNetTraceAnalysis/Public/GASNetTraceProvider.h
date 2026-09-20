#pragma once

#include "CoreMinimal.h"
#include "TraceServices/Model/AnalysisSession.h"

struct FGASNetTraceAnalysisEvent
{
    uint64 EventId = 0;
    double Time = 0.0;
    uint64 ASCId = 0;
    uint64 SubjectId = 0;
    uint64 OwnerId = 0;
    uint64 AvatarId = 0;
    uint8 Type = 0;
    uint8 Flags = 0;
    uint32 ConnectionId = 0;
    int32 AbilitySpecHandle = INDEX_NONE;
    int32 PredictionCurrent = 0;
    int32 PredictionBase = 0;
    uint64 PredictiveConnectionKey = 0;
    double ValueA = 0.0;
    double ValueB = 0.0;
    FString ASCPath;
    FString SubjectPath;
    FString Detail;
};

struct FGASNetTraceObjectIdentity
{
    uint64 LocalObjectId = 0;
    uint8 Kind = 0;
    uint8 Confidence = 0;
    uint32 ConnectionId = 0;
    FString StableId;
    FString ObjectPath;
    FString ClassPath;
};

struct FGASNetTraceNetworkConfig
{
    double Time = 0.0;
    int32 PacketLagMs = 0;
    int32 PacketLossPercent = 0;
    int32 PacketOrder = 0;
    int32 PacketDupPercent = 0;
};

struct FGASNetTraceClockSample
{
    uint32 Sequence = 0;
    double ClientSend = 0.0;
    double ServerReceive = 0.0;
    double ServerSend = 0.0;
    double ClientReceive = 0.0;
};

class GASNETTRACEANALYSIS_API FGASNetTraceProvider final : public TraceServices::IProvider
{
public:
    void SetSession(uint16 InSchemaVersion, FString InCaptureId, FString InEndpointId, FString InRole);
    void AppendEvent(FGASNetTraceAnalysisEvent&& Event);
    void AppendClockSample(const FGASNetTraceClockSample& Sample);
    void SetCoverage(uint64 ASCId, uint32 Mask, FString Path);
    void SetObjectIdentity(FGASNetTraceObjectIdentity&& Identity);
    void SetNetworkConfig(const FGASNetTraceNetworkConfig& Config);

    const FString& GetCaptureId() const { return CaptureId; }
    const FString& GetEndpointId() const { return EndpointId; }
    const FString& GetRole() const { return Role; }
    uint16 GetSchemaVersion() const { return SchemaVersion; }
    const TArray<FGASNetTraceAnalysisEvent>& GetEvents() const { return Events; }
    const TArray<FGASNetTraceClockSample>& GetClockSamples() const { return ClockSamples; }
    const TMap<uint64, uint32>& GetCoverage() const { return Coverage; }
    const TMap<uint64, FGASNetTraceObjectIdentity>& GetObjectIdentities() const { return ObjectIdentities; }
    const TArray<FGASNetTraceNetworkConfig>& GetNetworkConfigs() const { return NetworkConfigs; }

    /** Lowest-RTT NTP estimate: server time = client time + Offset, ± Uncertainty. */
    bool EstimateClock(double& OutOffset, double& OutUncertainty) const;

private:
    uint16 SchemaVersion = 1;
    FString CaptureId;
    FString EndpointId;
    FString Role;
    TArray<FGASNetTraceAnalysisEvent> Events;
    TArray<FGASNetTraceClockSample> ClockSamples;
    TMap<uint64, uint32> Coverage;
    TMap<uint64, FString> ASCPaths;
    TMap<uint64, FGASNetTraceObjectIdentity> ObjectIdentities;
    TArray<FGASNetTraceNetworkConfig> NetworkConfigs;
    uint64 NextLegacyEventId = 1;
};

GASNETTRACEANALYSIS_API FName GetGASNetTraceProviderName();
GASNETTRACEANALYSIS_API const FGASNetTraceProvider* ReadGASNetTraceProvider(const TraceServices::IAnalysisSession& Session);
