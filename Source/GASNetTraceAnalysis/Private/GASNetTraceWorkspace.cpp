#include "GASNetTraceWorkspace.h"

#include "Algo/Sort.h"
#include "GASNetTraceProvider.h"
#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "TraceServices/AnalysisService.h"
#include "TraceServices/ITraceServicesModule.h"
#include "TraceServices/Model/NetProfiler.h"

namespace
{
    constexpr int32 MaxNetworkEvidence = 200000;

    struct FNetworkEvidence
    {
        FString Id;
        FString Endpoint;
        uint32 ConnectionIndex = 0;
        uint32 ConnectionId = 0;
        uint32 PacketIndex = 0;
        uint32 Sequence = 0;
        uint32 Bytes = 0;
        double Time = 0.0;
        bool bOutgoing = false;
        TArray<FString> ContentNames;
        TArray<uint64> NetObjectIds;
    };

    struct FEndpointSnapshot
    {
        uint16 SchemaVersion = 1;
        FString CaptureId;
        FString Endpoint;
        FString Role;
        FString TracePath;
        bool bClockSynchronized = false;
        double ClockOffset = 0.0;
        double ClockUncertainty = 0.0;
        bool bHasNetProfiler = false;
        TArray<FGASNetTraceAnalysisEvent> Events;
        TMap<uint64, FGASNetTraceObjectIdentity> Identities;
        TMap<uint64, uint32> Coverage;
        TArray<FGASNetTraceNetworkConfig> NetworkConfigs;
        TArray<FNetworkEvidence> Network;
        TArray<FString> Warnings;
    };

    FString NormalizePath(FString Path)
    {
        int32 Start = Path.Find(TEXT("UEDPIE_"));
        while (Start != INDEX_NONE)
        {
            int32 End = Start + 7;
            while (End < Path.Len() && FChar::IsDigit(Path[End])) ++End;
            if (End < Path.Len() && Path[End] == TCHAR('_')) ++End;
            Path.RemoveAt(Start, End - Start);
            Start = Path.Find(TEXT("UEDPIE_"));
        }
        return Path;
    }

    const TCHAR* EventTypeName(uint8 Type)
    {
        static const TCHAR* Names[] = {
            TEXT("AbilityAttempt"), TEXT("AbilityActivated"), TEXT("AbilityFailed"), TEXT("AbilityEnded"),
            TEXT("PredictionWindow"), TEXT("EffectAdded"), TEXT("EffectRemoved"), TEXT("EffectStack"),
            TEXT("TagChanged"), TEXT("AttributeChanged"), TEXT("GameplayCue"), TEXT("TargetData"),
            TEXT("AbilitySpec"), TEXT("ActorInfo"), TEXT("ClockSync"), TEXT("NetworkConfig") };
        return Type < UE_ARRAY_COUNT(Names) ? Names[Type] : TEXT("Unknown");
    }

    int32 EventLane(const FEndpointSnapshot& Endpoint, const FGASNetTraceAnalysisEvent& Event)
    {
        if (Event.Type == 0) return 0;
        if (Event.Type == 4 || (!Endpoint.Role.Contains(TEXT("Server")) && Event.Type >= 5)) return 1;
        if (Endpoint.Role.Contains(TEXT("Server")) && (Event.Type == 1 || Event.Type == 2 || Event.Type == 11)) return 3;
        if (Endpoint.Role.Contains(TEXT("Server"))) return 4;
        return 6;
    }

    FString EventKey(const FEndpointSnapshot& Endpoint, const FGASNetTraceAnalysisEvent& Event)
    {
        FString ASCIdentity = NormalizePath(Event.ASCPath);
        if (const FGASNetTraceObjectIdentity* Identity = Endpoint.Identities.Find(Event.ASCId))
        {
            if (!Identity->StableId.IsEmpty()) ASCIdentity = Identity->StableId;
        }
        return FString::Printf(TEXT("%s|%d|%d|%d|%llu"), *ASCIdentity, Event.AbilitySpecHandle,
            Event.PredictionCurrent, Event.PredictionBase, Event.PredictiveConnectionKey);
    }

    FString EventGlobalId(const FEndpointSnapshot& Endpoint, const FGASNetTraceAnalysisEvent& Event)
    {
        return FString::Printf(TEXT("%s:%llu"), *Endpoint.Endpoint, Event.EventId);
    }

    bool IsNetworkSemanticMatch(uint8 EventType, const FString& Content)
    {
        if (EventType <= 4) return Content.Contains(TEXT("Ability"), ESearchCase::IgnoreCase);
        if (EventType == 11) return Content.Contains(TEXT("TargetData"), ESearchCase::IgnoreCase);
        if (EventType == 10) return Content.Contains(TEXT("GameplayCue"), ESearchCase::IgnoreCase) || Content.Contains(TEXT("Cue"), ESearchCase::IgnoreCase);
        return Content.Contains(TEXT("Property"), ESearchCase::IgnoreCase) || Content.Contains(TEXT("Replicate"), ESearchCase::IgnoreCase);
    }

    bool HasSemanticContent(const FNetworkEvidence& Evidence, uint8 EventType)
    {
        for (const FString& Name : Evidence.ContentNames) if (IsNetworkSemanticMatch(EventType, Name)) return true;
        return false;
    }

    const FNetworkEvidence* FindNetworkEvidence(const FEndpointSnapshot& Endpoint, const FGASNetTraceAnalysisEvent& Event,
        double WorkspaceTime, double& OutConfidence, FString& OutBasis)
    {
        const FNetworkEvidence* Best = nullptr;
        double BestDelta = 0.250;
        uint64 StableNumericId = 0;
        if (const FGASNetTraceObjectIdentity* Identity = Endpoint.Identities.Find(Event.SubjectId))
        {
            FString Numeric = Identity->StableId;
            int32 Colon = INDEX_NONE;
            if (Numeric.FindLastChar(TCHAR(':'), Colon)) Numeric.RightChopInline(Colon + 1);
            LexFromString(StableNumericId, *Numeric);
        }
        for (const FNetworkEvidence& Candidate : Endpoint.Network)
        {
            const double Delta = FMath::Abs(Candidate.Time + Endpoint.ClockOffset - WorkspaceTime);
            if (Delta > BestDelta || !HasSemanticContent(Candidate, Event.Type)) continue;
            Best = &Candidate;
            BestDelta = Delta;
            const bool bObjectExact = StableNumericId != 0 && Candidate.NetObjectIds.Contains(StableNumericId);
            OutConfidence = bObjectExact ? 0.94 : 0.78;
            OutBasis = bObjectExact ? TEXT("packet-content+net-object+time") : TEXT("packet-content+time");
        }
        return Best;
    }

    void SnapshotNetwork(const TraceServices::INetProfilerProvider& Provider, FEndpointSnapshot& Snapshot)
    {
        Snapshot.bHasNetProfiler = Provider.GetNetTraceVersion() > 0;
        if (!Snapshot.bHasNetProfiler) return;

        TMap<uint32, FString> EventNames;
        Provider.ReadEventTypes([&EventNames](const TraceServices::FNetProfilerEventType* Types, uint64 Count)
        {
            for (uint64 Index = 0; Index < Count; ++Index) EventNames.Add(Types[Index].EventTypeIndex, Types[Index].Name);
        });

        Provider.ReadGameInstances([&](const TraceServices::FNetProfilerGameInstance& GameInstance)
        {
            TMap<uint32, uint64> ObjectIds;
            Provider.ReadObjects(GameInstance.GameInstanceIndex, [&ObjectIds](const TraceServices::FNetProfilerObjectInstance& Object)
            {
                ObjectIds.Add(Object.ObjectIndex, Object.NetObjectId);
            });
            Provider.ReadConnections(GameInstance.GameInstanceIndex, [&](const TraceServices::FNetProfilerConnection& Connection)
            {
                for (uint8 ModeValue = 0; ModeValue < TraceServices::ENetProfilerConnectionMode::Count; ++ModeValue)
                {
                    const auto Mode = static_cast<TraceServices::ENetProfilerConnectionMode>(ModeValue);
                    const uint32 PacketCount = Provider.GetPacketCount(Connection.ConnectionIndex, Mode);
                    if (PacketCount == 0) continue;
                    uint32 PacketIndex = 0;
                    Provider.EnumeratePackets(Connection.ConnectionIndex, Mode, 0, PacketCount - 1,
                        [&](const TraceServices::FNetProfilerPacket& Packet)
                    {
                        if (Snapshot.Network.Num() >= MaxNetworkEvidence) return;
                        FNetworkEvidence Evidence;
                        Evidence.Endpoint = Snapshot.Endpoint;
                        Evidence.ConnectionIndex = Connection.ConnectionIndex;
                        Evidence.ConnectionId = Connection.ConnectionId;
                        Evidence.PacketIndex = PacketIndex++;
                        Evidence.Sequence = Packet.SequenceNumber;
                        Evidence.Bytes = Packet.TotalPacketSizeInBytes;
                        Evidence.Time = Packet.TimeStamp;
                        Evidence.bOutgoing = Mode == TraceServices::ENetProfilerConnectionMode::Outgoing;
                        Evidence.Id = FString::Printf(TEXT("%s:%u:%s:%u"), *Snapshot.Endpoint, Connection.ConnectionId,
                            Evidence.bOutgoing ? TEXT("out") : TEXT("in"), Evidence.PacketIndex);
                        if (Packet.EventCount > 0)
                        {
                            Provider.EnumeratePacketContentEventsByIndex(Connection.ConnectionIndex, Mode,
                                Packet.StartEventIndex, Packet.StartEventIndex + Packet.EventCount - 1,
                                [&](const TraceServices::FNetProfilerContentEvent& Content)
                            {
                                if (const FString* Name = EventNames.Find(Content.EventTypeIndex)) Evidence.ContentNames.AddUnique(*Name);
                                if (const uint64* NetId = ObjectIds.Find(Content.ObjectInstanceIndex)) Evidence.NetObjectIds.AddUnique(*NetId);
                            });
                        }
                        Snapshot.Network.Add(MoveTemp(Evidence));
                    });
                }
            });
        });
        if (Snapshot.Network.Num() >= MaxNetworkEvidence)
            Snapshot.Warnings.Add(TEXT("network evidence truncated at 200000 packets"));
    }

    bool SnapshotSession(const FString& Path, FEndpointSnapshot& Out, FString& Error)
    {
        ITraceServicesModule& Module = FModuleManager::LoadModuleChecked<ITraceServicesModule>(TEXT("TraceServices"));
        const TSharedPtr<const TraceServices::IAnalysisSession> Session = Module.GetAnalysisService()->Analyze(*Path);
        if (!Session.IsValid()) { Error = FString::Printf(TEXT("Unable to analyze %s"), *Path); return false; }
        TraceServices::FAnalysisSessionReadScope Scope(*Session);
        const FGASNetTraceProvider* Provider = ReadGASNetTraceProvider(*Session);
        if (!Provider || Provider->GetCaptureId().IsEmpty() || Provider->GetEvents().IsEmpty())
        {
            Error = FString::Printf(TEXT("Trace has no usable GASNetTrace session/events: %s"), *Path);
            return false;
        }
        Out.TracePath = Path;
        Out.SchemaVersion = Provider->GetSchemaVersion();
        Out.CaptureId = Provider->GetCaptureId();
        Out.Endpoint = Provider->GetEndpointId();
        Out.Role = Provider->GetRole();
        Out.Events = Provider->GetEvents();
        Out.Identities = Provider->GetObjectIdentities();
        Out.Coverage = Provider->GetCoverage();
        Out.NetworkConfigs = Provider->GetNetworkConfigs();
        Out.bClockSynchronized = Provider->EstimateClock(Out.ClockOffset, Out.ClockUncertainty);
        if (const TraceServices::INetProfilerProvider* Net = TraceServices::ReadNetProfilerProvider(*Session)) SnapshotNetwork(*Net, Out);
        return true;
    }

    void AddDiagnostic(TArray<TSharedPtr<FJsonValue>>& Diagnostics, const TCHAR* Id, const TCHAR* Severity,
        const FString& Summary, const FString& EvidenceId = {}, double Confidence = 1.0, bool bDefinitive = true)
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("id"), Id);
        Json->SetStringField(TEXT("severity"), Severity);
        Json->SetStringField(TEXT("summary"), Summary);
        Json->SetStringField(TEXT("evidenceId"), EvidenceId);
        Json->SetNumberField(TEXT("confidence"), Confidence);
        Json->SetBoolField(TEXT("definitive"), bDefinitive && Confidence >= 0.75);
        Diagnostics.Add(MakeShared<FJsonValueObject>(Json));
    }

    TSharedRef<FJsonObject> Metric(const TArray<double>& Samples)
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetNumberField(TEXT("count"), Samples.Num());
        if (Samples.IsEmpty())
        {
            Json->SetNumberField(TEXT("p50Ms"), 0); Json->SetNumberField(TEXT("p95Ms"), 0); Json->SetNumberField(TEXT("maxMs"), 0);
            return Json;
        }
        TArray<double> Sorted = Samples;
        Sorted.Sort();
        auto Percentile = [&Sorted](double P) { return Sorted[FMath::Clamp(FMath::CeilToInt(P * Sorted.Num()) - 1, 0, Sorted.Num() - 1)] * 1000.0; };
        Json->SetNumberField(TEXT("p50Ms"), Percentile(0.50));
        Json->SetNumberField(TEXT("p95Ms"), Percentile(0.95));
        Json->SetNumberField(TEXT("maxMs"), Sorted.Last() * 1000.0);
        return Json;
    }
}

FGASNetTraceDiagnosticThresholds FGASNetTraceDiagnosticThresholds::FromJson(const TSharedPtr<FJsonObject>& Json)
{
    FGASNetTraceDiagnosticThresholds Result;
    if (!Json) return Result;
    auto Number = [&Json](const TCHAR* Name, double& Value) { double Parsed = 0; if (Json->TryGetNumberField(Name, Parsed)) Value = Parsed; };
    Number(TEXT("rollbackDeadlineSeconds"), Result.RollbackDeadlineSeconds);
    Number(TEXT("convergenceDeadlineSeconds"), Result.ConvergenceDeadlineSeconds);
    Number(TEXT("actorInfoDeadlineSeconds"), Result.ActorInfoDeadlineSeconds);
    Number(TEXT("attributeStableSeconds"), Result.AttributeStableSeconds);
    Number(TEXT("cueWindowSeconds"), Result.CueWindowSeconds);
    Number(TEXT("verdictDeadlineSeconds"), Result.VerdictDeadlineSeconds);
    Number(TEXT("networkProcessingDeadlineSeconds"), Result.NetworkProcessingDeadlineSeconds);
    Number(TEXT("replicationDeadlineSeconds"), Result.ReplicationDeadlineSeconds);
    Number(TEXT("attributeAbsoluteTolerance"), Result.AttributeAbsoluteTolerance);
    Number(TEXT("attributeRelativeTolerance"), Result.AttributeRelativeTolerance);
    Number(TEXT("rejectionRateThreshold"), Result.RejectionRateThreshold);
    Number(TEXT("rollbackRateThreshold"), Result.RollbackRateThreshold);
    Number(TEXT("convergenceP95Seconds"), Result.ConvergenceP95Seconds);
    Number(TEXT("clockUncertaintySeconds"), Result.ClockUncertaintySeconds);
    double Minimum = Result.MinimumRateSamples; Number(TEXT("minimumRateSamples"), Minimum); Result.MinimumRateSamples = FMath::Max(1, FMath::RoundToInt(Minimum));
    return Result;
}

TSharedRef<FJsonObject> FGASNetTraceDiagnosticThresholds::ToJson() const
{
    TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetNumberField(TEXT("rollbackDeadlineSeconds"), RollbackDeadlineSeconds);
    Json->SetNumberField(TEXT("convergenceDeadlineSeconds"), ConvergenceDeadlineSeconds);
    Json->SetNumberField(TEXT("actorInfoDeadlineSeconds"), ActorInfoDeadlineSeconds);
    Json->SetNumberField(TEXT("attributeStableSeconds"), AttributeStableSeconds);
    Json->SetNumberField(TEXT("cueWindowSeconds"), CueWindowSeconds);
    Json->SetNumberField(TEXT("verdictDeadlineSeconds"), VerdictDeadlineSeconds);
    Json->SetNumberField(TEXT("networkProcessingDeadlineSeconds"), NetworkProcessingDeadlineSeconds);
    Json->SetNumberField(TEXT("replicationDeadlineSeconds"), ReplicationDeadlineSeconds);
    Json->SetNumberField(TEXT("attributeAbsoluteTolerance"), AttributeAbsoluteTolerance);
    Json->SetNumberField(TEXT("attributeRelativeTolerance"), AttributeRelativeTolerance);
    Json->SetNumberField(TEXT("rejectionRateThreshold"), RejectionRateThreshold);
    Json->SetNumberField(TEXT("rollbackRateThreshold"), RollbackRateThreshold);
    Json->SetNumberField(TEXT("convergenceP95Seconds"), ConvergenceP95Seconds);
    Json->SetNumberField(TEXT("clockUncertaintySeconds"), ClockUncertaintySeconds);
    Json->SetNumberField(TEXT("minimumRateSamples"), MinimumRateSamples);
    return Json;
}

FGASNetTraceWorkspaceResult FGASNetTraceWorkspaceAnalyzer::AnalyzeFiles(const TArray<FString>& TracePaths,
    const FGASNetTraceDiagnosticThresholds& Thresholds)
{
    FGASNetTraceWorkspaceResult Result;
    if (TracePaths.IsEmpty()) { Result.Error = TEXT("Workspace contains no traces"); return Result; }

    TArray<FEndpointSnapshot> Endpoints;
    for (const FString& InputPath : TracePaths)
    {
        FEndpointSnapshot Snapshot;
        FString Error;
        const FString Path = FPaths::ConvertRelativePathToFull(InputPath);
        if (!SnapshotSession(Path, Snapshot, Error)) { Result.Error = Error; return Result; }
        if (Result.CaptureId.IsEmpty()) Result.CaptureId = Snapshot.CaptureId;
        if (!Result.CaptureId.Equals(Snapshot.CaptureId, ESearchCase::IgnoreCase))
        {
            Result.Error = TEXT("Workspace traces have different CaptureIds"); return Result;
        }
        Result.EventCount += Snapshot.Events.Num();
        Endpoints.Add(MoveTemp(Snapshot));
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("schema"), TEXT("gasnettrace.workspace.v2"));
    Root->SetStringField(TEXT("captureId"), Result.CaptureId);
    Root->SetNumberField(TEXT("eventCount"), static_cast<double>(Result.EventCount));
    Root->SetObjectField(TEXT("thresholds"), Thresholds.ToJson());

    TArray<TSharedPtr<FJsonValue>> EndpointValues, EventValues, NetworkValues, CoverageValues, Diagnostics, Chains;
    TMap<FString, TArray<TPair<int32, int32>>> EventsByKey;
    TMap<FString, FString> EventToPacket;
    TArray<double> PredictionRtt, VerdictDelay, ReplicationDelay, ConvergenceDelay, ActorInfoRebind;
    int32 AttemptCount = 0, FailureCount = 0, CancelledCount = 0;

    for (int32 EndpointIndex = 0; EndpointIndex < Endpoints.Num(); ++EndpointIndex)
    {
        FEndpointSnapshot& Endpoint = Endpoints[EndpointIndex];
        TSharedRef<FJsonObject> EndpointJson = MakeShared<FJsonObject>();
        EndpointJson->SetStringField(TEXT("id"), Endpoint.Endpoint);
        EndpointJson->SetStringField(TEXT("role"), Endpoint.Role);
        EndpointJson->SetStringField(TEXT("tracePath"), Endpoint.TracePath);
        EndpointJson->SetNumberField(TEXT("schemaVersion"), Endpoint.SchemaVersion);
        EndpointJson->SetBoolField(TEXT("clockSynchronized"), Endpoint.bClockSynchronized);
        EndpointJson->SetNumberField(TEXT("clockOffsetSeconds"), Endpoint.ClockOffset);
        EndpointJson->SetNumberField(TEXT("clockUncertaintySeconds"), Endpoint.ClockUncertainty);
        EndpointJson->SetBoolField(TEXT("netProfilerAvailable"), Endpoint.bHasNetProfiler);
        EndpointJson->SetNumberField(TEXT("eventCount"), Endpoint.Events.Num());
        EndpointValues.Add(MakeShared<FJsonValueObject>(EndpointJson));

        for (const TPair<uint64, uint32>& Coverage : Endpoint.Coverage)
        {
            TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("endpoint"), Endpoint.Endpoint);
            Json->SetStringField(TEXT("ascId"), LexToString(Coverage.Key));
            Json->SetNumberField(TEXT("mask"), Coverage.Value);
            if (const FGASNetTraceObjectIdentity* Identity = Endpoint.Identities.Find(Coverage.Key)) Json->SetStringField(TEXT("asc"), Identity->ObjectPath);
            CoverageValues.Add(MakeShared<FJsonValueObject>(Json));
        }

        for (int32 EventIndex = 0; EventIndex < Endpoint.Events.Num(); ++EventIndex)
        {
            const FGASNetTraceAnalysisEvent& Event = Endpoint.Events[EventIndex];
            const double WorkspaceTime = Event.Time + (Endpoint.bClockSynchronized ? Endpoint.ClockOffset : 0.0);
            const FString GlobalId = EventGlobalId(Endpoint, Event);
            const FString Key = EventKey(Endpoint, Event);
            EventsByKey.FindOrAdd(Key).Add({ EndpointIndex, EventIndex });
            TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("id"), GlobalId);
            Json->SetStringField(TEXT("endpoint"), Endpoint.Endpoint);
            Json->SetStringField(TEXT("role"), Endpoint.Role);
            Json->SetStringField(TEXT("type"), EventTypeName(Event.Type));
            Json->SetNumberField(TEXT("typeId"), Event.Type);
            Json->SetNumberField(TEXT("lane"), EventLane(Endpoint, Event));
            Json->SetNumberField(TEXT("time"), WorkspaceTime);
            Json->SetNumberField(TEXT("localTime"), Event.Time);
            Json->SetStringField(TEXT("asc"), NormalizePath(Event.ASCPath));
            Json->SetStringField(TEXT("subject"), NormalizePath(Event.SubjectPath));
            Json->SetStringField(TEXT("detail"), Event.Detail);
            Json->SetNumberField(TEXT("spec"), Event.AbilitySpecHandle);
            Json->SetNumberField(TEXT("predictionCurrent"), Event.PredictionCurrent);
            Json->SetNumberField(TEXT("predictionBase"), Event.PredictionBase);
            Json->SetStringField(TEXT("predictiveConnectionKey"), LexToString(Event.PredictiveConnectionKey));
            Json->SetNumberField(TEXT("connectionId"), Event.ConnectionId);
            Json->SetNumberField(TEXT("valueA"), Event.ValueA);
            Json->SetNumberField(TEXT("valueB"), Event.ValueB);
            Json->SetNumberField(TEXT("flags"), Event.Flags);
            Json->SetBoolField(TEXT("timingReliable"), Endpoint.Role.Contains(TEXT("Server")) || Endpoint.bClockSynchronized);
            Json->SetNumberField(TEXT("clockUncertaintySeconds"), Endpoint.ClockUncertainty);
            if (const FGASNetTraceObjectIdentity* Identity = Endpoint.Identities.Find(Event.ASCId))
            {
                Json->SetStringField(TEXT("ascStableId"), Identity->StableId);
                Json->SetNumberField(TEXT("identityConfidence"), Identity->Confidence);
            }
            double Confidence = 0.0; FString Basis;
            if (const FNetworkEvidence* Evidence = FindNetworkEvidence(Endpoint, Event, WorkspaceTime, Confidence, Basis))
            {
                Json->SetStringField(TEXT("networkEvidenceId"), Evidence->Id);
                Json->SetNumberField(TEXT("networkConfidence"), Confidence);
                Json->SetStringField(TEXT("networkMatchBasis"), Basis);
                EventToPacket.Add(GlobalId, Evidence->Id);
                const double EvidenceTime = Evidence->Time + (Endpoint.bClockSynchronized ? Endpoint.ClockOffset : 0.0);
                const bool bServer = Endpoint.Role.Contains(TEXT("Server")) || Endpoint.Endpoint.Equals(TEXT("Server"), ESearchCase::IgnoreCase);
                if (bServer && (Event.Type == 5 || Event.Type == 8 || Event.Type == 9) && EvidenceTime >= WorkspaceTime)
                    ReplicationDelay.Add(EvidenceTime - WorkspaceTime);
            }
            EventValues.Add(MakeShared<FJsonValueObject>(Json));
            if (Event.Type == 0) ++AttemptCount;
            if (Event.Type == 2) ++FailureCount;
            if (Event.Type == 3 && (Event.Flags & 1) != 0) ++CancelledCount;
        }

        for (const FNetworkEvidence& Evidence : Endpoint.Network)
        {
            TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("id"), Evidence.Id);
            Json->SetStringField(TEXT("endpoint"), Evidence.Endpoint);
            Json->SetNumberField(TEXT("connectionId"), Evidence.ConnectionId);
            Json->SetStringField(TEXT("direction"), Evidence.bOutgoing ? TEXT("out") : TEXT("in"));
            Json->SetNumberField(TEXT("packetIndex"), Evidence.PacketIndex);
            Json->SetNumberField(TEXT("sequence"), Evidence.Sequence);
            Json->SetNumberField(TEXT("time"), Evidence.Time + (Endpoint.bClockSynchronized ? Endpoint.ClockOffset : 0.0));
            Json->SetNumberField(TEXT("bytes"), Evidence.Bytes);
            TArray<TSharedPtr<FJsonValue>> Content;
            for (const FString& Name : Evidence.ContentNames) Content.Add(MakeShared<FJsonValueString>(Name));
            Json->SetArrayField(TEXT("content"), MoveTemp(Content));
            TArray<TSharedPtr<FJsonValue>> RelatedEvents;
            for (const TPair<FString, FString>& Link : EventToPacket)
                if (Link.Value == Evidence.Id) RelatedEvents.Add(MakeShared<FJsonValueString>(Link.Key));
            Json->SetArrayField(TEXT("relatedEventIds"), MoveTemp(RelatedEvents));
            NetworkValues.Add(MakeShared<FJsonValueObject>(Json));
        }
    }

    for (const TPair<FString, TArray<TPair<int32, int32>>>& Pair : EventsByKey)
    {
        TArray<TPair<int32, int32>> Items = Pair.Value;
        Items.Sort([&Endpoints](const auto& A, const auto& B)
        {
            const FEndpointSnapshot& EA = Endpoints[A.Key]; const FEndpointSnapshot& EB = Endpoints[B.Key];
            return EA.Events[A.Value].Time + EA.ClockOffset < EB.Events[B.Value].Time + EB.ClockOffset;
        });
        // A semantic key can legitimately repeat (and v1 often has a zero
        // PredictionKey). Split it into temporal activation episodes instead
        // of manufacturing one enormous causal chain.
        TArray<TArray<TPair<int32, int32>>> Episodes;
        double LastAnchorTime = -TNumericLimits<double>::Max();
        for (const TPair<int32, int32>& Item : Items)
        {
            const FEndpointSnapshot& Endpoint = Endpoints[Item.Key];
            const FGASNetTraceAnalysisEvent& Event = Endpoint.Events[Item.Value];
            const double Time = Event.Time + (Endpoint.bClockSynchronized ? Endpoint.ClockOffset : 0.0);
            const bool bAnchor = Event.Type == 0 || Event.Type == 1 || Event.Type == 2;
            if (bAnchor && !Episodes.IsEmpty() && Time - LastAnchorTime > 0.4) Episodes.AddDefaulted();
            if (Episodes.IsEmpty()) Episodes.AddDefaulted();
            Episodes.Last().Add(Item);
            if (bAnchor) LastAnchorTime = Time;
        }

        int32 EpisodeIndex = 0;
        for (const TArray<TPair<int32, int32>>& Episode : Episodes)
        {
            bool bHasAttempt = false, bHasVerdict = false;
            double AttemptTime = TNumericLimits<double>::Max(), VerdictTime = 0.0;
            TArray<TSharedPtr<FJsonValue>> StageValues, EdgeValues;
            FString PreviousId;
            for (const TPair<int32, int32>& Item : Episode)
            {
                const FEndpointSnapshot& Endpoint = Endpoints[Item.Key];
                const FGASNetTraceAnalysisEvent& Event = Endpoint.Events[Item.Value];
                const double Time = Event.Time + (Endpoint.bClockSynchronized ? Endpoint.ClockOffset : 0.0);
                const FString Id = EventGlobalId(Endpoint, Event);
                const bool bServer = Endpoint.Role.Contains(TEXT("Server")) || Endpoint.Endpoint.Equals(TEXT("Server"), ESearchCase::IgnoreCase);
                if (Event.Type == 0) { bHasAttempt = true; AttemptTime = FMath::Min(AttemptTime, Time); }
                if (bServer && (Event.Type == 1 || Event.Type == 2)) { bHasVerdict = true; VerdictTime = Time; }
                if (!bServer && Event.Type == 1) AttemptTime = FMath::Min(AttemptTime, Time);
                TSharedRef<FJsonObject> Stage = MakeShared<FJsonObject>();
                Stage->SetStringField(TEXT("eventId"), Id);
                Stage->SetStringField(TEXT("endpoint"), Endpoint.Endpoint);
                Stage->SetStringField(TEXT("type"), EventTypeName(Event.Type));
                Stage->SetNumberField(TEXT("time"), Time);
                Stage->SetNumberField(TEXT("lane"), EventLane(Endpoint, Event));
                StageValues.Add(MakeShared<FJsonValueObject>(Stage));
                if (!PreviousId.IsEmpty())
                {
                    const bool bReliable = Endpoint.bClockSynchronized || bServer;
                    TSharedRef<FJsonObject> Edge = MakeShared<FJsonObject>();
                    Edge->SetStringField(TEXT("sourceId"), PreviousId); Edge->SetStringField(TEXT("targetId"), Id);
                    Edge->SetStringField(TEXT("evidenceType"), TEXT("semantic-key+time"));
                    Edge->SetNumberField(TEXT("confidence"), bReliable ? 0.90 : 0.65);
                    Edge->SetStringField(TEXT("level"), bReliable ? TEXT("strong") : TEXT("weak"));
                    EdgeValues.Add(MakeShared<FJsonValueObject>(Edge));
                }
                PreviousId = Id;
            }
            if (AttemptTime < TNumericLimits<double>::Max() && bHasVerdict && VerdictTime >= AttemptTime)
            {
                PredictionRtt.Add(VerdictTime - AttemptTime); VerdictDelay.Add(VerdictTime - AttemptTime);
            }
            if (bHasAttempt || bHasVerdict)
            {
                TSharedRef<FJsonObject> Chain = MakeShared<FJsonObject>();
                Chain->SetStringField(TEXT("id"), FString::Printf(TEXT("chain-%u-%d"), GetTypeHash(Pair.Key), EpisodeIndex));
                Chain->SetStringField(TEXT("key"), Pair.Key);
                Chain->SetArrayField(TEXT("stages"), MoveTemp(StageValues));
                Chain->SetArrayField(TEXT("edges"), MoveTemp(EdgeValues));
                Chain->SetBoolField(TEXT("complete"), bHasAttempt && bHasVerdict && Episode.Num() >= 4);
                Chains.Add(MakeShared<FJsonValueObject>(Chain));
            }
            if (bHasAttempt && !bHasVerdict)
                AddDiagnostic(Diagnostics, TEXT("D006"), TEXT("warning"), TEXT("Prediction attempt has no matching server verdict"), Pair.Key, 0.88);
            ++EpisodeIndex;
        }
        if (Episodes.Num() > 1 && !Pair.Key.Contains(TEXT("|0|0|")))
            AddDiagnostic(Diagnostics, TEXT("D006"), TEXT("warning"), TEXT("Prediction identity was reused across activation episodes"), Pair.Key, 0.92);
    }

    // D003/D009 are evidence-quality invariants and must run even when no ability chain is present.
    for (const FEndpointSnapshot& Endpoint : Endpoints)
    {
        int32 ActorInfoCount = 0;
        for (const FGASNetTraceAnalysisEvent& Event : Endpoint.Events) if (Event.Type == 13) ++ActorInfoCount;
        if (ActorInfoCount == 0)
            AddDiagnostic(Diagnostics, TEXT("D003"), TEXT("warning"), Endpoint.Endpoint + TEXT(": no ActorInfo snapshot"), Endpoint.Endpoint, 0.80);
        const bool bServer = Endpoint.Role.Contains(TEXT("Server")) || Endpoint.Endpoint.Equals(TEXT("Server"), ESearchCase::IgnoreCase);
        if ((!bServer && (!Endpoint.bClockSynchronized || Endpoint.ClockUncertainty > Thresholds.ClockUncertaintySeconds)) ||
            !Endpoint.bHasNetProfiler || Endpoint.Identities.IsEmpty())
        {
            AddDiagnostic(Diagnostics, TEXT("D009"), TEXT("info"), Endpoint.Endpoint +
                TEXT(": evidence incomplete or clock uncertainty exceeds threshold; deterministic root causes are suppressed"),
                Endpoint.Endpoint, 0.55, false);
        }
    }

    TMap<FString, TArray<double>> ActorInfoTimes;
    for (const FEndpointSnapshot& Endpoint : Endpoints)
        for (const FGASNetTraceAnalysisEvent& Event : Endpoint.Events)
            if (Event.Type == 13) ActorInfoTimes.FindOrAdd(NormalizePath(Event.ASCPath)).Add(Event.Time + (Endpoint.bClockSynchronized ? Endpoint.ClockOffset : 0.0));
    for (auto& Pair : ActorInfoTimes)
    {
        Pair.Value.Sort();
        if (Pair.Value.Num() > 1 && Pair.Value.Last() - Pair.Value[0] <= 5.0)
        {
            const double Delay = Pair.Value.Last() - Pair.Value[0];
            ActorInfoRebind.Add(Delay);
            if (Delay > Thresholds.ActorInfoDeadlineSeconds)
                AddDiagnostic(Diagnostics, TEXT("D003"), TEXT("warning"), TEXT("ActorInfo rebind observations exceed configured deadline"), Pair.Key, 0.78);
        }
    }

    // D004 compares the latest stable attribute observation per endpoint/ASC/attribute.
    TMap<FString, TMap<FString, const FGASNetTraceAnalysisEvent*>> Attributes;
    for (const FEndpointSnapshot& Endpoint : Endpoints)
        for (const FGASNetTraceAnalysisEvent& Event : Endpoint.Events)
            if (Event.Type == 9)
            {
                const FString Key = NormalizePath(Event.ASCPath) + TEXT("|") + Event.Detail;
                const FGASNetTraceAnalysisEvent*& Latest = Attributes.FindOrAdd(Key).FindOrAdd(Endpoint.Endpoint);
                if (!Latest || Event.Time > Latest->Time) Latest = &Event;
            }
    for (const auto& Pair : Attributes)
    {
        if (Pair.Value.Num() < 2) continue;
        double Min = TNumericLimits<double>::Max(), Max = -TNumericLimits<double>::Max();
        for (const auto& Value : Pair.Value) { Min = FMath::Min(Min, Value.Value->ValueB); Max = FMath::Max(Max, Value.Value->ValueB); }
        const double Tolerance = FMath::Max(Thresholds.AttributeAbsoluteTolerance, FMath::Max(FMath::Abs(Min), FMath::Abs(Max)) * Thresholds.AttributeRelativeTolerance);
        if (Max - Min > Tolerance) AddDiagnostic(Diagnostics, TEXT("D004"), TEXT("error"), TEXT("Stable attribute values diverge across endpoints"), Pair.Key, 0.86);
    }

    // D005 detects duplicate Cue lifecycle observations on the same endpoint and semantic key.
    TMap<FString, TArray<double>> CueTimes;
    for (const FEndpointSnapshot& Endpoint : Endpoints)
        for (const FGASNetTraceAnalysisEvent& Event : Endpoint.Events)
            if (Event.Type == 10) CueTimes.FindOrAdd(Endpoint.Endpoint + TEXT("|") + EventKey(Endpoint, Event) + TEXT("|") + Event.Detail).Add(Event.Time);
    int32 DuplicateCueCount = 0;
    for (auto& Pair : CueTimes)
    {
        Pair.Value.Sort();
        bool bDuplicate = false;
        for (int32 Index = 1; Index < Pair.Value.Num(); ++Index)
            if (Pair.Value[Index] - Pair.Value[Index - 1] <= Thresholds.CueWindowSeconds) { bDuplicate = true; break; }
        if (bDuplicate)
        {
            ++DuplicateCueCount;
            AddDiagnostic(Diagnostics, TEXT("D005"), TEXT("warning"), TEXT("Duplicate GameplayCue lifecycle evidence inside configured window"), Pair.Key, 0.82);
        }
    }

    // D007: semantic network evidence without nearby GAS evidence, and authoritative state without a packet edge.
    for (const FEndpointSnapshot& Endpoint : Endpoints)
    {
        for (const FNetworkEvidence& Net : Endpoint.Network)
        {
            bool bGASSemantic = false;
            for (const FString& Name : Net.ContentNames)
                bGASSemantic |= Name.Contains(TEXT("Ability"), ESearchCase::IgnoreCase) || Name.Contains(TEXT("TargetData"), ESearchCase::IgnoreCase);
            if (!bGASSemantic) continue;
            bool bNearby = false;
            for (const FGASNetTraceAnalysisEvent& Event : Endpoint.Events)
                if (FMath::Abs(Event.Time - Net.Time) <= Thresholds.NetworkProcessingDeadlineSeconds) { bNearby = true; break; }
            if (!bNearby) AddDiagnostic(Diagnostics, TEXT("D007"), TEXT("warning"), TEXT("GAS network content has no nearby GAS processing event"), Net.Id, 0.78);
        }
        if (Endpoint.Role.Contains(TEXT("Server")))
        {
            for (const FGASNetTraceAnalysisEvent& Event : Endpoint.Events)
                if ((Event.Type == 5 || Event.Type == 8 || Event.Type == 9) && !EventToPacket.Contains(EventGlobalId(Endpoint, Event)))
                    AddDiagnostic(Diagnostics, TEXT("D007"), TEXT("warning"), TEXT("Authoritative GAS state has no replication packet evidence"), EventGlobalId(Endpoint, Event), 0.65, false);
        }
    }

    if (AttemptCount >= Thresholds.MinimumRateSamples)
    {
        const double RejectionRate = static_cast<double>(FailureCount) / AttemptCount;
        const double RollbackRate = static_cast<double>(CancelledCount) / AttemptCount;
        if (RejectionRate > Thresholds.RejectionRateThreshold || RollbackRate > Thresholds.RollbackRateThreshold)
            AddDiagnostic(Diagnostics, TEXT("D008"), TEXT("warning"),
                FString::Printf(TEXT("Rates exceed threshold: rejection %.1f%%, rollback %.1f%%"), RejectionRate * 100.0, RollbackRate * 100.0),
                TEXT("aggregate"), 0.95);
    }
    // D001/D002 are evaluated in bounded windows. A future unrelated state
    // event must never be treated as residue or convergence evidence.
    for (const FEndpointSnapshot& Server : Endpoints)
    {
        if (!Server.Role.Contains(TEXT("Server")) && !Server.Endpoint.Equals(TEXT("Server"), ESearchCase::IgnoreCase)) continue;
        for (const FGASNetTraceAnalysisEvent& Verdict : Server.Events)
        {
            if (Verdict.Type != 1 && Verdict.Type != 2) continue;
            const double VerdictTime = Verdict.Time;
            bool bClientState = false;
            double FirstClientStateTime = TNumericLimits<double>::Max();
            for (const FEndpointSnapshot& Client : Endpoints)
            {
                if (&Client == &Server) continue;
                const bool bTimingReliable = Client.bClockSynchronized && Client.ClockUncertainty <= Thresholds.ClockUncertaintySeconds;
                for (const FGASNetTraceAnalysisEvent& Event : Client.Events)
                {
                    const double Time = Event.Time + (Client.bClockSynchronized ? Client.ClockOffset : 0.0);
                    const bool bSamePrediction = Verdict.PredictionCurrent != 0 && Event.PredictionCurrent == Verdict.PredictionCurrent &&
                        Event.PredictionBase == Verdict.PredictionBase && Event.PredictiveConnectionKey == Verdict.PredictiveConnectionKey;
                    const bool bSameASC = NormalizePath(Event.ASCPath) == NormalizePath(Verdict.ASCPath);
                    if (!bSamePrediction && !bSameASC) continue;
                    if (Verdict.Type == 1 && Event.Type >= 5 && Event.Type <= 10 && Time >= VerdictTime &&
                        Time <= VerdictTime + Thresholds.ConvergenceDeadlineSeconds)
                    {
                        bClientState = true;
                        FirstClientStateTime = FMath::Min(FirstClientStateTime, Time);
                    }
                    if (Verdict.Type == 2 && (Event.Type == 5 || Event.Type == 10) && Time <= VerdictTime &&
                        Time >= VerdictTime - Thresholds.RollbackDeadlineSeconds)
                    {
                        bool bRemoved = false;
                        for (const FGASNetTraceAnalysisEvent& Later : Client.Events)
                        {
                            const double LaterTime = Later.Time + (Client.bClockSynchronized ? Client.ClockOffset : 0.0);
                            if (LaterTime < VerdictTime || LaterTime > VerdictTime + Thresholds.RollbackDeadlineSeconds) continue;
                            if (Event.Type == 5 && Later.Type == 6 && NormalizePath(Later.SubjectPath) == NormalizePath(Event.SubjectPath)) bRemoved = true;
                            if (Event.Type == 10 && Later.Type == 10 && Later.Detail != Event.Detail && NormalizePath(Later.SubjectPath) == NormalizePath(Event.SubjectPath)) bRemoved = true;
                        }
                        if (!bRemoved) AddDiagnostic(Diagnostics, TEXT("D001"), TEXT("error"),
                            TEXT("Predicted effect/cue has no rollback evidence after server rejection"), EventGlobalId(Client, Event),
                            bTimingReliable ? 0.82 : 0.55, bTimingReliable);
                    }
                }
            }
            if (Verdict.Type == 1 && bClientState && FirstClientStateTime >= VerdictTime)
                ConvergenceDelay.Add(FirstClientStateTime - VerdictTime);
            if (Verdict.Type == 1 && !bClientState)
            {
                const bool bIdentityReliable = !Server.Identities.IsEmpty();
                AddDiagnostic(Diagnostics, TEXT("D002"), TEXT("error"), TEXT("Successful server verdict has no client convergence evidence inside deadline"),
                    EventGlobalId(Server, Verdict), bIdentityReliable ? 0.80 : 0.55, bIdentityReliable);
            }
        }
    }

    if (ConvergenceDelay.Num() >= Thresholds.MinimumRateSamples)
    {
        TArray<double> Sorted = ConvergenceDelay;
        Sorted.Sort();
        const double P95 = Sorted[FMath::Clamp(FMath::CeilToInt(0.95 * Sorted.Num()) - 1, 0, Sorted.Num() - 1)];
        if (P95 > Thresholds.ConvergenceP95Seconds)
            AddDiagnostic(Diagnostics, TEXT("D008"), TEXT("warning"),
                FString::Printf(TEXT("Convergence P95 %.1f ms exceeds threshold %.1f ms"), P95 * 1000.0, Thresholds.ConvergenceP95Seconds * 1000.0),
                TEXT("aggregate"), 0.95);
    }

    TSharedRef<FJsonObject> Metrics = MakeShared<FJsonObject>();
    Metrics->SetObjectField(TEXT("predictionRtt"), Metric(PredictionRtt));
    Metrics->SetObjectField(TEXT("serverVerdict"), Metric(VerdictDelay));
    Metrics->SetObjectField(TEXT("replicationDelay"), Metric(ReplicationDelay));
    Metrics->SetObjectField(TEXT("convergenceDelay"), Metric(ConvergenceDelay));
    Metrics->SetObjectField(TEXT("actorInfoRebindTime"), Metric(ActorInfoRebind));
    Metrics->SetNumberField(TEXT("rollbackRate"), AttemptCount > 0 ? static_cast<double>(CancelledCount) / AttemptCount : 0.0);
    Metrics->SetNumberField(TEXT("cueDuplicationRate"), CueTimes.Num() > 0 ? static_cast<double>(DuplicateCueCount) / CueTimes.Num() : 0.0);

    Root->SetArrayField(TEXT("endpoints"), MoveTemp(EndpointValues));
    Root->SetArrayField(TEXT("events"), MoveTemp(EventValues));
    Root->SetArrayField(TEXT("networkEvidence"), MoveTemp(NetworkValues));
    Root->SetArrayField(TEXT("causalityChains"), MoveTemp(Chains));
    Root->SetArrayField(TEXT("diagnostics"), MoveTemp(Diagnostics));
    Root->SetArrayField(TEXT("coverage"), MoveTemp(CoverageValues));
    Root->SetObjectField(TEXT("metrics"), Metrics);
    TArray<TSharedPtr<FJsonValue>> Warnings;
    for (const FEndpointSnapshot& Endpoint : Endpoints)
        for (const FString& Warning : Endpoint.Warnings) Warnings.Add(MakeShared<FJsonValueString>(Endpoint.Endpoint + TEXT(": ") + Warning));
    Root->SetArrayField(TEXT("analysisWarnings"), MoveTemp(Warnings));

    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Result.Json);
    FJsonSerializer::Serialize(Root, Writer);
    Result.bSucceeded = true;
    return Result;
}

FGASNetTraceWorkspaceResult FGASNetTraceWorkspaceAnalyzer::AnalyzeWorkspaceFile(const FString& WorkspacePath)
{
    FGASNetTraceWorkspaceResult Failure;
    FString Source;
    const FString AbsolutePath = FPaths::ConvertRelativePathToFull(WorkspacePath);
    if (!FFileHelper::LoadFileToString(Source, *AbsolutePath))
    {
        Failure.Error = TEXT("Unable to read workspace file: ") + AbsolutePath;
        return Failure;
    }
    TSharedPtr<FJsonObject> Workspace;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Source), Workspace) || !Workspace.IsValid())
    {
        Failure.Error = TEXT("Workspace is not valid JSON: ") + AbsolutePath;
        return Failure;
    }
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Workspace->TryGetArrayField(TEXT("tracePaths"), Values) || !Values || Values->IsEmpty())
    {
        Failure.Error = TEXT("Workspace has no tracePaths");
        return Failure;
    }
    TArray<FString> TracePaths;
    const FString Directory = FPaths::GetPath(AbsolutePath);
    for (const TSharedPtr<FJsonValue>& Value : *Values)
    {
        FString Path;
        if (!Value.IsValid() || !Value->TryGetString(Path) || Path.IsEmpty()) continue;
        if (FPaths::IsRelative(Path)) Path = FPaths::Combine(Directory, Path);
        TracePaths.Add(FPaths::ConvertRelativePathToFull(Path));
    }
    const TSharedPtr<FJsonObject>* ThresholdJson = nullptr;
    const FGASNetTraceDiagnosticThresholds Thresholds = Workspace->TryGetObjectField(TEXT("thresholds"), ThresholdJson) && ThresholdJson
        ? FGASNetTraceDiagnosticThresholds::FromJson(*ThresholdJson) : FGASNetTraceDiagnosticThresholds{};
    FGASNetTraceWorkspaceResult Result = AnalyzeFiles(TracePaths, Thresholds);
    FString ExpectedCaptureId;
    if (Result.bSucceeded && Workspace->TryGetStringField(TEXT("captureId"), ExpectedCaptureId) && !ExpectedCaptureId.IsEmpty() &&
        !ExpectedCaptureId.Equals(Result.CaptureId, ESearchCase::IgnoreCase))
    {
        Result.bSucceeded = false;
        Result.Error = TEXT("Workspace CaptureId does not match trace metadata");
        Result.Json.Reset();
    }
    return Result;
}
