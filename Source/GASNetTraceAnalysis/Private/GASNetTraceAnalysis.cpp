#include "GASNetTraceProvider.h"

#include "Features/IModularFeatures.h"
#include "HAL/FileManager.h"
#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Trace/Analyzer.h"
#include "TraceServices/Model/NetProfiler.h"
#include "TraceServices/ModuleService.h"

namespace
{
    const FName ProviderName(TEXT("GASNetTraceProvider"));

    class FGASNetTraceAnalyzer final : public UE::Trace::IAnalyzer
    {
    public:
        FGASNetTraceAnalyzer(TraceServices::IAnalysisSession& InSession, FGASNetTraceProvider& InProvider)
            : Session(InSession), Provider(InProvider) {}

        virtual void OnAnalysisBegin(const FOnAnalysisContext& Context) override
        {
            Context.InterfaceBuilder.RouteEvent(1, "GASNetTrace", "Session");
            Context.InterfaceBuilder.RouteEvent(2, "GASNetTrace", "Coverage");
            Context.InterfaceBuilder.RouteEvent(3, "GASNetTrace", "Event");
            Context.InterfaceBuilder.RouteEvent(4, "GASNetTrace", "ClockSync");
            Context.InterfaceBuilder.RouteEvent(5, "GASNetTrace", "EventV2");
            Context.InterfaceBuilder.RouteEvent(6, "GASNetTrace", "ObjectIdentity");
            Context.InterfaceBuilder.RouteEvent(7, "GASNetTrace", "NetworkConfig");
        }

        virtual bool OnEvent(uint16 RouteId, EStyle, const FOnEventContext& Context) override
        {
            TraceServices::FAnalysisSessionEditScope Scope(Session);
            const FEventData& Data = Context.EventData;
            if (RouteId == 1)
            {
                FWideStringView Capture, Endpoint, Role;
                Data.GetString("CaptureId", Capture);
                Data.GetString("EndpointId", Endpoint);
                Data.GetString("ProcessRole", Role);
                Provider.SetSession(Data.GetValue<uint16>("SchemaVersion"), FString(Capture), FString(Endpoint), FString(Role));
            }
            else if (RouteId == 2)
            {
                FWideStringView Path;
                Data.GetString("ASCPath", Path);
                Provider.SetCoverage(Data.GetValue<uint64>("ASCId"), Data.GetValue<uint32>("CoverageMask"), FString(Path));
            }
            else if (RouteId == 3 || RouteId == 5)
            {
                FGASNetTraceAnalysisEvent Event;
                Event.EventId = RouteId == 5 ? Data.GetValue<uint64>("EventId") : 0;
                const uint64 Timestamp = Data.GetValue<uint64>("Timestamp");
                Event.Time = Context.EventTime.AsSeconds(Timestamp);
                Event.ASCId = Data.GetValue<uint64>("ASCId");
                Event.SubjectId = Data.GetValue<uint64>("SubjectId");
                if (RouteId == 5)
                {
                    Event.OwnerId = Data.GetValue<uint64>("OwnerId");
                    Event.AvatarId = Data.GetValue<uint64>("AvatarId");
                    Event.ConnectionId = Data.GetValue<uint32>("ConnectionId");
                }
                Event.Type = Data.GetValue<uint8>("Type");
                Event.Flags = Data.GetValue<uint8>("Flags");
                Event.AbilitySpecHandle = Data.GetValue<int32>("AbilitySpecHandle");
                Event.PredictionCurrent = Data.GetValue<int32>("PredictionCurrent");
                Event.PredictionBase = Data.GetValue<int32>("PredictionBase");
                Event.PredictiveConnectionKey = Data.GetValue<uint64>("PredictiveConnectionKey");
                Event.ValueA = Data.GetValue<double>("ValueA");
                Event.ValueB = Data.GetValue<double>("ValueB");
                FWideStringView ASCPath, SubjectPath, Detail;
                Data.GetString("ASCPath", ASCPath);
                Data.GetString("SubjectPath", SubjectPath);
                Data.GetString("Detail", Detail);
                Event.ASCPath = FString(ASCPath);
                Event.SubjectPath = FString(SubjectPath);
                Event.Detail = FString(Detail);
                Provider.AppendEvent(MoveTemp(Event));
            }
            else if (RouteId == 4)
            {
                FGASNetTraceClockSample Sample;
                Sample.Sequence = Data.GetValue<uint32>("Sequence");
                Sample.ClientSend = Data.GetValue<double>("ClientSend");
                Sample.ServerReceive = Data.GetValue<double>("ServerReceive");
                Sample.ServerSend = Data.GetValue<double>("ServerSend");
                Sample.ClientReceive = Data.GetValue<double>("ClientReceive");
                Provider.AppendClockSample(Sample);
            }
            else if (RouteId == 6)
            {
                FGASNetTraceObjectIdentity Identity;
                Identity.LocalObjectId = Data.GetValue<uint64>("LocalObjectId");
                Identity.Kind = Data.GetValue<uint8>("IdentityKind");
                Identity.Confidence = Data.GetValue<uint8>("Confidence");
                Identity.ConnectionId = Data.GetValue<uint32>("ConnectionId");
                FWideStringView StableId, ObjectPath, ClassPath;
                Data.GetString("StableId", StableId);
                Data.GetString("ObjectPath", ObjectPath);
                Data.GetString("ClassPath", ClassPath);
                Identity.StableId = FString(StableId);
                Identity.ObjectPath = FString(ObjectPath);
                Identity.ClassPath = FString(ClassPath);
                Provider.SetObjectIdentity(MoveTemp(Identity));
            }
            else if (RouteId == 7)
            {
                FGASNetTraceNetworkConfig Config;
                Config.Time = Context.EventTime.AsSeconds(Data.GetValue<uint64>("Timestamp"));
                Config.PacketLagMs = Data.GetValue<int32>("PacketLagMs");
                Config.PacketLossPercent = Data.GetValue<int32>("PacketLossPercent");
                Config.PacketOrder = Data.GetValue<int32>("PacketOrder");
                Config.PacketDupPercent = Data.GetValue<int32>("PacketDupPercent");
                Provider.SetNetworkConfig(Config);
            }
            return true;
        }

    private:
        TraceServices::IAnalysisSession& Session;
        FGASNetTraceProvider& Provider;
    };

    class FGASNetTraceTraceModule final : public TraceServices::IModule
    {
    public:
        virtual void GetModuleInfo(TraceServices::FModuleInfo& Out) override
        {
            Out.Name = TEXT("TraceModule_GASNetTrace");
            Out.DisplayName = TEXT("GAS Net Trace");
        }
        virtual void OnAnalysisBegin(TraceServices::IAnalysisSession& Session) override
        {
            TSharedPtr<FGASNetTraceProvider> Provider = MakeShared<FGASNetTraceProvider>();
            Session.AddProvider(ProviderName, Provider);
            Session.AddAnalyzer(new FGASNetTraceAnalyzer(Session, *Provider));
        }
        virtual void GetLoggers(TArray<const TCHAR*>& OutLoggers) override { OutLoggers.Add(TEXT("GASNetTrace")); }
        virtual const TCHAR* GetCommandLineArgument() override { return TEXT("gasnettrace"); }
        virtual void GenerateReports(const TraceServices::IAnalysisSession& Session, const TCHAR*, const TCHAR* OutputDirectory) override
        {
            const FGASNetTraceProvider* Provider = ReadGASNetTraceProvider(Session);
            if (!Provider) return;
            const TraceServices::INetProfilerProvider* NetProvider = TraceServices::ReadNetProfilerProvider(Session);
            TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
            Root->SetStringField(TEXT("schema"), TEXT("gasnettrace.report.v1"));
            Root->SetStringField(TEXT("captureId"), Provider->GetCaptureId());
            Root->SetStringField(TEXT("endpointId"), Provider->GetEndpointId());
            Root->SetNumberField(TEXT("eventCount"), Provider->GetEvents().Num());
            Root->SetBoolField(TEXT("netProfilerAvailable"), NetProvider != nullptr);
            double Offset = 0.0, Error = 0.0;
            Root->SetBoolField(TEXT("clockSynchronized"), Provider->EstimateClock(Offset, Error));
            Root->SetNumberField(TEXT("clockOffsetSeconds"), Offset);
            Root->SetNumberField(TEXT("clockUncertaintySeconds"), Error);
            TArray<TSharedPtr<FJsonValue>> EventValues;
            int32 AttemptCount = 0;
            int32 FailureCount = 0;
            TSet<FString> VerdictKeys;
            for (const FGASNetTraceAnalysisEvent& Event : Provider->GetEvents())
            {
                TSharedRef<FJsonObject> JsonEvent = MakeShared<FJsonObject>();
                JsonEvent->SetNumberField(TEXT("time"), Event.Time);
                JsonEvent->SetNumberField(TEXT("type"), Event.Type);
                JsonEvent->SetStringField(TEXT("asc"), Event.ASCPath);
                JsonEvent->SetStringField(TEXT("subject"), Event.SubjectPath);
                JsonEvent->SetStringField(TEXT("detail"), Event.Detail);
                JsonEvent->SetNumberField(TEXT("spec"), Event.AbilitySpecHandle);
                JsonEvent->SetNumberField(TEXT("predictionCurrent"), Event.PredictionCurrent);
                JsonEvent->SetNumberField(TEXT("predictionBase"), Event.PredictionBase);
                JsonEvent->SetStringField(TEXT("predictiveConnectionKey"), LexToString(Event.PredictiveConnectionKey));
                EventValues.Add(MakeShared<FJsonValueObject>(JsonEvent));
                const FString Key = FString::Printf(TEXT("%s|%d|%d|%d|%llu"), *Event.ASCPath,
                    Event.AbilitySpecHandle, Event.PredictionCurrent, Event.PredictionBase, Event.PredictiveConnectionKey);
                if (Event.Type == 0) ++AttemptCount;
                if (Event.Type == 1 || Event.Type == 2) VerdictKeys.Add(Key);
                if (Event.Type == 2) ++FailureCount;
            }
            Root->SetArrayField(TEXT("events"), MoveTemp(EventValues));

            TArray<TSharedPtr<FJsonValue>> Diagnostics;
            for (const FGASNetTraceAnalysisEvent& Event : Provider->GetEvents())
            {
                if (Event.Type != 0 || Event.PredictionCurrent == 0) continue;
                const FString Key = FString::Printf(TEXT("%s|%d|%d|%d|%llu"), *Event.ASCPath,
                    Event.AbilitySpecHandle, Event.PredictionCurrent, Event.PredictionBase, Event.PredictiveConnectionKey);
                if (!VerdictKeys.Contains(Key))
                {
                    TSharedRef<FJsonObject> D006 = MakeShared<FJsonObject>();
                    D006->SetStringField(TEXT("id"), TEXT("D006"));
                    D006->SetStringField(TEXT("severity"), TEXT("warning"));
                    D006->SetStringField(TEXT("summary"), TEXT("Prediction attempt has no verdict in this endpoint trace"));
                    D006->SetStringField(TEXT("evidenceKey"), Key);
                    Diagnostics.Add(MakeShared<FJsonValueObject>(D006));
                }
            }
            if (AttemptCount > 0 && static_cast<double>(FailureCount) / AttemptCount > 0.2)
            {
                TSharedRef<FJsonObject> D008 = MakeShared<FJsonObject>();
                D008->SetStringField(TEXT("id"), TEXT("D008"));
                D008->SetStringField(TEXT("severity"), TEXT("warning"));
                D008->SetStringField(TEXT("summary"), TEXT("Prediction rejection rate exceeds 20% in this trace"));
                D008->SetNumberField(TEXT("rate"), static_cast<double>(FailureCount) / AttemptCount);
                Diagnostics.Add(MakeShared<FJsonValueObject>(D008));
            }
            if (!Provider->EstimateClock(Offset, Error))
            {
                TSharedRef<FJsonObject> D009 = MakeShared<FJsonObject>();
                D009->SetStringField(TEXT("id"), TEXT("D009"));
                D009->SetStringField(TEXT("severity"), TEXT("info"));
                D009->SetStringField(TEXT("summary"), TEXT("No complete clock sample; cross-endpoint millisecond conclusions are forbidden"));
                Diagnostics.Add(MakeShared<FJsonValueObject>(D009));
            }
            Root->SetArrayField(TEXT("diagnostics"), MoveTemp(Diagnostics));
            FString Json;
            const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
            FJsonSerializer::Serialize(Root, Writer);
            const FString Path = FPaths::Combine(OutputDirectory, TEXT("gas-net-trace-report.json"));
            FFileHelper::SaveStringToFile(Json, *Path);
        }
    };

    class FGASNetTraceAnalysisModule final : public IModuleInterface
    {
    public:
        virtual void StartupModule() override
        {
            IModularFeatures::Get().RegisterModularFeature(TraceServices::ModuleFeatureName, &TraceModule);
        }
        virtual void ShutdownModule() override
        {
            IModularFeatures::Get().UnregisterModularFeature(TraceServices::ModuleFeatureName, &TraceModule);
        }
    private:
        FGASNetTraceTraceModule TraceModule;
    };
}

void FGASNetTraceProvider::SetSession(uint16 InSchemaVersion, FString InCaptureId, FString InEndpointId, FString InRole)
{
    SchemaVersion = InSchemaVersion; CaptureId = MoveTemp(InCaptureId); EndpointId = MoveTemp(InEndpointId); Role = MoveTemp(InRole);
}
void FGASNetTraceProvider::AppendEvent(FGASNetTraceAnalysisEvent&& Event)
{
    if (Event.EventId == 0) Event.EventId = NextLegacyEventId++;
    Events.Add(MoveTemp(Event));
}
void FGASNetTraceProvider::AppendClockSample(const FGASNetTraceClockSample& Sample) { ClockSamples.Add(Sample); }
void FGASNetTraceProvider::SetCoverage(uint64 ASCId, uint32 Mask, FString Path)
{
    Coverage.FindOrAdd(ASCId) |= Mask; ASCPaths.FindOrAdd(ASCId) = MoveTemp(Path);
}
void FGASNetTraceProvider::SetObjectIdentity(FGASNetTraceObjectIdentity&& Identity)
{
    ObjectIdentities.Add(Identity.LocalObjectId, MoveTemp(Identity));
}
void FGASNetTraceProvider::SetNetworkConfig(const FGASNetTraceNetworkConfig& Config) { NetworkConfigs.Add(Config); }
bool FGASNetTraceProvider::EstimateClock(double& OutOffset, double& OutUncertainty) const
{
    double BestRtt = TNumericLimits<double>::Max();
    bool bFound = false;
    for (const FGASNetTraceClockSample& S : ClockSamples)
    {
        if (S.ClientReceive <= S.ClientSend || S.ServerSend < S.ServerReceive) continue;
        const double Rtt = (S.ClientReceive - S.ClientSend) - (S.ServerSend - S.ServerReceive);
        if (Rtt >= 0.0 && Rtt < BestRtt)
        {
            BestRtt = Rtt;
            OutOffset = ((S.ServerReceive - S.ClientSend) + (S.ServerSend - S.ClientReceive)) * 0.5;
            OutUncertainty = Rtt * 0.5;
            bFound = true;
        }
    }
    return bFound;
}

FName GetGASNetTraceProviderName() { return ProviderName; }
const FGASNetTraceProvider* ReadGASNetTraceProvider(const TraceServices::IAnalysisSession& Session)
{
    return Session.ReadProvider<FGASNetTraceProvider>(ProviderName);
}

IMPLEMENT_MODULE(FGASNetTraceAnalysisModule, GASNetTraceAnalysis)
