#include "GASNetTraceTrace.h"

#include "AbilitySystemComponent.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/PackageMapClient.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Iris/ReplicationSystem/NetRefHandle.h"
#include "Iris/ReplicationSystem/ObjectReplicationBridge.h"
#include "Iris/ReplicationSystem/ReplicationSystem.h"
#include "Misc/CommandLine.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Trace/Trace.h"

UE_TRACE_CHANNEL_DEFINE(GASNetTraceChannel);

UE_TRACE_EVENT_BEGIN(GASNetTrace, Session, Important | NoSync)
    UE_TRACE_EVENT_FIELD(uint16, SchemaVersion)
    UE_TRACE_EVENT_FIELD(UE::Trace::WideString, CaptureId)
    UE_TRACE_EVENT_FIELD(UE::Trace::WideString, EndpointId)
    UE_TRACE_EVENT_FIELD(UE::Trace::WideString, ProcessRole)
UE_TRACE_EVENT_END()

UE_TRACE_EVENT_BEGIN(GASNetTrace, ObjectIdentity, Important | NoSync)
    UE_TRACE_EVENT_FIELD(uint64, LocalObjectId)
    UE_TRACE_EVENT_FIELD(uint8, IdentityKind)
    UE_TRACE_EVENT_FIELD(uint8, Confidence)
    UE_TRACE_EVENT_FIELD(uint32, ConnectionId)
    UE_TRACE_EVENT_FIELD(UE::Trace::WideString, StableId)
    UE_TRACE_EVENT_FIELD(UE::Trace::WideString, ObjectPath)
    UE_TRACE_EVENT_FIELD(UE::Trace::WideString, ClassPath)
UE_TRACE_EVENT_END()

UE_TRACE_EVENT_BEGIN(GASNetTrace, Coverage, Important | NoSync)
    UE_TRACE_EVENT_FIELD(uint64, Timestamp)
    UE_TRACE_EVENT_FIELD(uint64, ASCId)
    UE_TRACE_EVENT_FIELD(uint32, CoverageMask)
    UE_TRACE_EVENT_FIELD(UE::Trace::WideString, ASCPath)
UE_TRACE_EVENT_END()

UE_TRACE_EVENT_BEGIN(GASNetTrace, Event)
    UE_TRACE_EVENT_FIELD(uint64, EventId)
    UE_TRACE_EVENT_FIELD(uint64, Timestamp)
    UE_TRACE_EVENT_FIELD(uint64, ASCId)
    UE_TRACE_EVENT_FIELD(uint64, SubjectId)
    UE_TRACE_EVENT_FIELD(uint64, OwnerId)
    UE_TRACE_EVENT_FIELD(uint64, AvatarId)
    UE_TRACE_EVENT_FIELD(uint8, Type)
    UE_TRACE_EVENT_FIELD(uint8, Flags)
    UE_TRACE_EVENT_FIELD(uint32, ConnectionId)
    UE_TRACE_EVENT_FIELD(int32, AbilitySpecHandle)
    UE_TRACE_EVENT_FIELD(int32, PredictionCurrent)
    UE_TRACE_EVENT_FIELD(int32, PredictionBase)
    UE_TRACE_EVENT_FIELD(uint64, PredictiveConnectionKey)
    UE_TRACE_EVENT_FIELD(double, ValueA)
    UE_TRACE_EVENT_FIELD(double, ValueB)
    UE_TRACE_EVENT_FIELD(UE::Trace::WideString, ASCPath)
    UE_TRACE_EVENT_FIELD(UE::Trace::WideString, SubjectPath)
UE_TRACE_EVENT_FIELD(UE::Trace::WideString, Detail)
UE_TRACE_EVENT_END()

UE_TRACE_EVENT_BEGIN(GASNetTrace, NetworkConfig, Important | NoSync)
    UE_TRACE_EVENT_FIELD(uint64, Timestamp)
    UE_TRACE_EVENT_FIELD(int32, PacketLagMs)
    UE_TRACE_EVENT_FIELD(int32, PacketLossPercent)
    UE_TRACE_EVENT_FIELD(int32, PacketOrder)
    UE_TRACE_EVENT_FIELD(int32, PacketDupPercent)
UE_TRACE_EVENT_END()

UE_TRACE_EVENT_BEGIN(GASNetTrace, ClockSync)
    UE_TRACE_EVENT_FIELD(uint32, Sequence)
    UE_TRACE_EVENT_FIELD(double, ClientSend)
    UE_TRACE_EVENT_FIELD(double, ServerReceive)
    UE_TRACE_EVENT_FIELD(double, ServerSend)
    UE_TRACE_EVENT_FIELD(double, ClientReceive)
UE_TRACE_EVENT_END()

namespace
{
    struct FSessionIdentity
    {
        FGuid CaptureId;
        FString EndpointId;
        bool bSessionEmitted = false;
        TAtomic<uint64> NextEventId { 1 };
        FCriticalSection MetadataLock;
        TSet<uint64> EmittedObjectIds;

        FSessionIdentity()
        {
            FString CaptureArg;
            if (!FParse::Value(FCommandLine::Get(), TEXT("GASNetTraceCapture="), CaptureArg) || !FGuid::Parse(CaptureArg, CaptureId))
            {
                CaptureId = FGuid::NewGuid();
            }
            if (!FParse::Value(FCommandLine::Get(), TEXT("GASNetTraceEndpoint="), EndpointId))
            {
                EndpointId = TEXT("Unknown");
            }
        }
    };

    FSessionIdentity& Identity()
    {
        static FSessionIdentity Value;
        return Value;
    }

    struct FResolvedIdentity
    {
        EGASNetTraceIdentityKind Kind = EGASNetTraceIdentityKind::None;
        EGASNetTraceConfidence Confidence = EGASNetTraceConfidence::None;
        uint32 ConnectionId = 0;
        FString StableId;
        FString Path;
        FString ClassPath;
    };

    uint32 ResolveConnectionId(const UObject* Object)
    {
        const AActor* Actor = Cast<AActor>(Object);
        const APlayerController* Controller = Cast<APlayerController>(Actor);
        if (!Controller)
        {
            if (const APawn* Pawn = Cast<APawn>(Actor)) Controller = Cast<APlayerController>(Pawn->GetController());
        }
        if (!Controller && Actor) Controller = Cast<APlayerController>(Actor->GetOwner());
        if (const UNetConnection* Connection = Controller ? Controller->GetNetConnection() : nullptr)
        {
            const UE::Net::FConnectionHandle Handle = Connection->GetConnectionHandle();
            return Handle.IsValid() ? Handle.GetParentConnectionId() : 0;
        }
        return 0;
    }

    FResolvedIdentity ResolveIdentity(const UObject* Object)
    {
        FResolvedIdentity Result;
        if (!Object) return Result;
        Result.Path = Object->GetPathName();
        Result.ClassPath = Object->GetClass()->GetPathName();
        Result.ConnectionId = ResolveConnectionId(Object);
        const UWorld* World = Object->GetWorld();
        UNetDriver* Driver = World ? World->GetNetDriver() : nullptr;
#if UE_WITH_IRIS
        if (Driver && Driver->GetReplicationSystem())
        {
            if (const UObjectReplicationBridge* Bridge = Driver->GetReplicationSystem()->GetReplicationBridge())
            {
                const UE::Net::FNetRefHandle Handle = Bridge->GetReplicatedRefHandle(Object);
                if (Handle.IsValid())
                {
                    Result.Kind = EGASNetTraceIdentityKind::IrisNetRefHandle;
                    Result.Confidence = EGASNetTraceConfidence::Exact;
                    Result.StableId = FString::Printf(TEXT("iris:%llu"), Handle.GetId());
                    return Result;
                }
            }
        }
#endif
        if (Driver)
        {
            const UNetConnection* Connection = Driver->ServerConnection;
            if (!Connection && Driver->ClientConnections.Num() > 0) Connection = Driver->ClientConnections[0];
            if (Connection && Connection->PackageMap)
            {
                const FNetworkGUID Guid = Connection->PackageMap->GetNetGUIDFromObject(Object);
                if (Guid.IsValid())
                {
                    Result.Kind = EGASNetTraceIdentityKind::NetworkGUID;
                    Result.Confidence = EGASNetTraceConfidence::Exact;
                    Result.StableId = FString::Printf(TEXT("netguid:%s"), *Guid.ToString());
                    return Result;
                }
            }
        }
        Result.Kind = EGASNetTraceIdentityKind::NormalizedPath;
        Result.Confidence = EGASNetTraceConfidence::Weak;
        Result.StableId = FString::Printf(TEXT("path:%s"), *Result.Path.Replace(TEXT("UEDPIE_0_"), TEXT("")));
        return Result;
    }

    void EmitObjectIdentityIfNeeded(const UObject* Object)
    {
#if UE_TRACE_ENABLED && !UE_BUILD_SHIPPING
        if (!Object || !GASNetTraceChannel) return;
        const uint64 LocalId = reinterpret_cast<uint64>(Object);
        FSessionIdentity& State = Identity();
        {
            FScopeLock Lock(&State.MetadataLock);
            if (State.EmittedObjectIds.Contains(LocalId)) return;
            State.EmittedObjectIds.Add(LocalId);
        }
        const FResolvedIdentity Resolved = ResolveIdentity(Object);
        const uint32 AuxSize = (Resolved.StableId.Len() + Resolved.Path.Len() + Resolved.ClassPath.Len()) * sizeof(TCHAR);
        UE_TRACE_LOG(GASNetTrace, ObjectIdentity, GASNetTraceChannel, AuxSize)
            << ObjectIdentity.LocalObjectId(LocalId)
            << ObjectIdentity.IdentityKind(static_cast<uint8>(Resolved.Kind))
            << ObjectIdentity.Confidence(static_cast<uint8>(Resolved.Confidence))
            << ObjectIdentity.ConnectionId(Resolved.ConnectionId)
            << ObjectIdentity.StableId(*Resolved.StableId)
            << ObjectIdentity.ObjectPath(*Resolved.Path)
            << ObjectIdentity.ClassPath(*Resolved.ClassPath);
#endif
    }

    void EmitSessionIfNeeded()
    {
#if UE_TRACE_ENABLED && !UE_BUILD_SHIPPING
        FSessionIdentity& State = Identity();
        if (State.bSessionEmitted || !GASNetTraceChannel)
        {
            return;
        }
        State.bSessionEmitted = true;
        const FString Capture = State.CaptureId.ToString(EGuidFormats::DigitsWithHyphensLower);
        const FString Role = IsRunningDedicatedServer() ? TEXT("DedicatedServer") : (IsRunningClientOnly() ? TEXT("Client") : TEXT("Game"));
        const uint32 AuxSize = (Capture.Len() + State.EndpointId.Len() + Role.Len()) * sizeof(TCHAR);
        UE_TRACE_LOG(GASNetTrace, Session, GASNetTraceChannel, AuxSize)
            << Session.SchemaVersion(2)
            << Session.CaptureId(*Capture)
            << Session.EndpointId(*State.EndpointId)
            << Session.ProcessRole(*Role);
#endif
    }
}

bool FGASNetTrace::IsEnabled()
{
#if UE_TRACE_ENABLED && !UE_BUILD_SHIPPING
    return bool(GASNetTraceChannel);
#else
    return false;
#endif
}

const FGuid& FGASNetTrace::GetCaptureId() { return Identity().CaptureId; }
const FString& FGASNetTrace::GetEndpointId() { return Identity().EndpointId; }

void FGASNetTrace::EmitCoverage(const UAbilitySystemComponent* ASC, EGASNetTraceCoverage CoverageValue)
{
#if UE_TRACE_ENABLED && !UE_BUILD_SHIPPING
    if (!GASNetTraceChannel || !ASC) return;
    EmitSessionIfNeeded();
    EmitObjectIdentityIfNeeded(ASC);
    EmitObjectIdentityIfNeeded(ASC->GetOwnerActor());
    EmitObjectIdentityIfNeeded(ASC->GetAvatarActor());
    const FString Path = ASC->GetPathName();
    UE_TRACE_LOG(GASNetTrace, Coverage, GASNetTraceChannel, Path.Len() * sizeof(TCHAR))
        << Coverage.Timestamp(FPlatformTime::Cycles64())
        << Coverage.ASCId(reinterpret_cast<uint64>(ASC))
        << Coverage.CoverageMask(static_cast<uint32>(CoverageValue))
        << Coverage.ASCPath(*Path);
#endif
}

void FGASNetTrace::EmitEvent(EGASNetTraceEventType Type, const UAbilitySystemComponent* ASC, const UObject* Subject,
    const FGASNetTraceContext& Context, FStringView Detail, double ValueA, double ValueB, uint8 Flags)
{
#if UE_TRACE_ENABLED && !UE_BUILD_SHIPPING
    if (!GASNetTraceChannel) return;
    EmitSessionIfNeeded();
    EmitObjectIdentityIfNeeded(ASC);
    EmitObjectIdentityIfNeeded(Subject);
    const UObject* Owner = ASC ? ASC->GetOwnerActor() : nullptr;
    const UObject* Avatar = ASC ? ASC->GetAvatarActor() : nullptr;
    EmitObjectIdentityIfNeeded(Owner);
    EmitObjectIdentityIfNeeded(Avatar);
    const FString ASCPath = GetPathNameSafe(ASC);
    const FString SubjectPath = GetPathNameSafe(Subject);
    const FString DetailString(Detail);
    const uint32 ConnectionId = Context.ConnectionId > 0 ? static_cast<uint32>(Context.ConnectionId) : ResolveConnectionId(Owner ? Owner : Avatar);
    UE_TRACE_LOG(GASNetTrace, Event, GASNetTraceChannel)
        << Event.EventId(Identity().NextEventId++)
        << Event.Timestamp(FPlatformTime::Cycles64())
        << Event.ASCId(reinterpret_cast<uint64>(ASC))
        << Event.SubjectId(reinterpret_cast<uint64>(Subject))
        << Event.OwnerId(reinterpret_cast<uint64>(Owner))
        << Event.AvatarId(reinterpret_cast<uint64>(Avatar))
        << Event.Type(static_cast<uint8>(Type))
        << Event.Flags(Flags)
        << Event.ConnectionId(ConnectionId)
        << Event.AbilitySpecHandle(Context.AbilitySpecHandle)
        << Event.PredictionCurrent(Context.PredictionCurrent)
        << Event.PredictionBase(Context.PredictionBase)
        << Event.PredictiveConnectionKey(Context.PredictiveConnectionKey)
        << Event.ValueA(ValueA)
        << Event.ValueB(ValueB)
        << Event.ASCPath(*ASCPath)
        << Event.SubjectPath(*SubjectPath)
        << Event.Detail(*DetailString);
#endif
}

void FGASNetTrace::EmitNetworkConfig(const UObject* WorldContext)
{
#if UE_TRACE_ENABLED && !UE_BUILD_SHIPPING
    if (!GASNetTraceChannel || !WorldContext) return;
    EmitSessionIfNeeded();
    int32 Lag = 0, Loss = 0, Order = 0, Dup = 0;
#if DO_ENABLE_NET_TEST
    if (const UWorld* World = WorldContext->GetWorld())
    {
        if (const UNetDriver* Driver = World->GetNetDriver())
        {
            Lag = Driver->PacketSimulationSettings.PktLag;
            Loss = Driver->PacketSimulationSettings.PktLoss;
            Order = Driver->PacketSimulationSettings.PktOrder;
            Dup = Driver->PacketSimulationSettings.PktDup;
        }
    }
#endif
    UE_TRACE_LOG(GASNetTrace, NetworkConfig, GASNetTraceChannel)
        << NetworkConfig.Timestamp(FPlatformTime::Cycles64())
        << NetworkConfig.PacketLagMs(Lag)
        << NetworkConfig.PacketLossPercent(Loss)
        << NetworkConfig.PacketOrder(Order)
        << NetworkConfig.PacketDupPercent(Dup);
#endif
}

void FGASNetTrace::EmitClockSync(uint32 Sequence, double ClientSend, double ServerReceive, double ServerSend, double ClientReceive)
{
#if UE_TRACE_ENABLED && !UE_BUILD_SHIPPING
    if (!GASNetTraceChannel) return;
    EmitSessionIfNeeded();
    UE_TRACE_LOG(GASNetTrace, ClockSync, GASNetTraceChannel)
        << ClockSync.Sequence(Sequence)
        << ClockSync.ClientSend(ClientSend)
        << ClockSync.ServerReceive(ServerReceive)
        << ClockSync.ServerSend(ServerSend)
        << ClockSync.ClientReceive(ClientReceive);
#endif
}
