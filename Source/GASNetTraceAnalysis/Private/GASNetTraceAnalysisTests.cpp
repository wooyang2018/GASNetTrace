#if WITH_DEV_AUTOMATION_TESTS

#include "GASNetTraceProvider.h"
#include "GASNetTraceWorkspace.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"
#include "Trace/OutDataStream.h"
#include "Trace/TraceWriter.h"

namespace
{
    bool WriteGoldenTrace(const FString& Path, bool bImportantSession, bool bUnknownField)
    {
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
        UE::Trace::FFileOutDataStream Stream;
        if (!Stream.Open(*Path)) return false;
        UE::Trace::FTraceWriter Writer(Stream);
        Writer.Begin();
        const uint32 GoldenThreadId = Writer.RegisterThread(ANSITEXTVIEW("GASNetTraceGolden"));
        using namespace UE::Trace;
        const ETraceWriterEventFlags SessionFlags = bImportantSession ? ETraceWriterEventFlags::ImportantNoSync : ETraceWriterEventFlags::None;
        const uint32 Session = Writer.DeclareEvent(ANSITEXTVIEW("GASNetTrace"), ANSITEXTVIEW("Session"), SessionFlags)
            .Field(ANSITEXTVIEW("SchemaVersion"), ETraceWriterFieldType::Uint16)
            .Field(ANSITEXTVIEW("CaptureId"), ETraceWriterFieldType::WideString)
            .Field(ANSITEXTVIEW("EndpointId"), ETraceWriterFieldType::WideString)
            .Field(ANSITEXTVIEW("ProcessRole"), ETraceWriterFieldType::WideString).End();

        auto& Declaration = Writer.DeclareEvent(ANSITEXTVIEW("GASNetTrace"), ANSITEXTVIEW("Event"));
        Declaration.Field(ANSITEXTVIEW("EventId"), ETraceWriterFieldType::Uint64);
        Declaration.Field(ANSITEXTVIEW("Timestamp"), ETraceWriterFieldType::Uint64)
            .Field(ANSITEXTVIEW("ASCId"), ETraceWriterFieldType::Uint64)
            .Field(ANSITEXTVIEW("SubjectId"), ETraceWriterFieldType::Uint64);
        Declaration.Field(ANSITEXTVIEW("OwnerId"), ETraceWriterFieldType::Uint64)
            .Field(ANSITEXTVIEW("AvatarId"), ETraceWriterFieldType::Uint64);
        Declaration.Field(ANSITEXTVIEW("Type"), ETraceWriterFieldType::Uint8)
            .Field(ANSITEXTVIEW("Flags"), ETraceWriterFieldType::Uint8);
        Declaration.Field(ANSITEXTVIEW("ConnectionId"), ETraceWriterFieldType::Uint32);
        Declaration.Field(ANSITEXTVIEW("AbilitySpecHandle"), ETraceWriterFieldType::Int32)
            .Field(ANSITEXTVIEW("PredictionCurrent"), ETraceWriterFieldType::Int32)
            .Field(ANSITEXTVIEW("PredictionBase"), ETraceWriterFieldType::Int32)
            .Field(ANSITEXTVIEW("PredictiveConnectionKey"), ETraceWriterFieldType::Uint64)
            .Field(ANSITEXTVIEW("ValueA"), ETraceWriterFieldType::Float64)
            .Field(ANSITEXTVIEW("ValueB"), ETraceWriterFieldType::Float64)
            .Field(ANSITEXTVIEW("ASCPath"), ETraceWriterFieldType::WideString)
            .Field(ANSITEXTVIEW("SubjectPath"), ETraceWriterFieldType::WideString)
            .Field(ANSITEXTVIEW("Detail"), ETraceWriterFieldType::WideString);
        if (bUnknownField) Declaration.Field(ANSITEXTVIEW("FutureField"), ETraceWriterFieldType::Uint32);
        const uint32 Event = Declaration.End();

        // Event declarations are emitted on the writer's metadata thread, so restore the data thread.
        Writer.SetCurrentThread(GoldenThreadId);
        Writer.WriteEvent(Session).Field(ANSITEXTVIEW("SchemaVersion"), static_cast<uint16>(2))
            .Field(ANSITEXTVIEW("CaptureId"), TEXT("11111111-2222-3333-4444-555555555555"))
            .Field(ANSITEXTVIEW("EndpointId"), TEXT("Golden"))
            .Field(ANSITEXTVIEW("ProcessRole"), TEXT("Game")).End();
        auto& EventBuilder = Writer.WriteEvent(Event);
        EventBuilder.Field(ANSITEXTVIEW("EventId"), static_cast<uint64>(9007199254740993ull));
        EventBuilder.Field(ANSITEXTVIEW("Timestamp"), static_cast<uint64>(1))
            .Field(ANSITEXTVIEW("ASCId"), static_cast<uint64>(7)).Field(ANSITEXTVIEW("SubjectId"), static_cast<uint64>(8));
        EventBuilder.Field(ANSITEXTVIEW("OwnerId"), static_cast<uint64>(9)).Field(ANSITEXTVIEW("AvatarId"), static_cast<uint64>(10));
        EventBuilder.Field(ANSITEXTVIEW("Type"), static_cast<uint8>(13)).Field(ANSITEXTVIEW("Flags"), static_cast<uint8>(0));
        EventBuilder.Field(ANSITEXTVIEW("ConnectionId"), static_cast<uint32>(1));
        EventBuilder.Field(ANSITEXTVIEW("AbilitySpecHandle"), static_cast<int32>(17))
            .Field(ANSITEXTVIEW("PredictionCurrent"), static_cast<int32>(84)).Field(ANSITEXTVIEW("PredictionBase"), static_cast<int32>(81))
            .Field(ANSITEXTVIEW("PredictiveConnectionKey"), static_cast<uint64>(9007199254740995ull))
            .Field(ANSITEXTVIEW("ValueA"), 1.0).Field(ANSITEXTVIEW("ValueB"), 2.0)
            .Field(ANSITEXTVIEW("ASCPath"), TEXT("/Game/Golden.ASC"))
            .Field(ANSITEXTVIEW("SubjectPath"), TEXT("/Game/Golden.Actor"))
            .Field(ANSITEXTVIEW("Detail"), TEXT("Golden"));
        if (bUnknownField) EventBuilder.Field(ANSITEXTVIEW("FutureField"), static_cast<uint32>(42));
        EventBuilder.End();
        Writer.End();
        return true;
    }

    bool WriteEmptyTrace(const FString& Path)
    {
        UE::Trace::FFileOutDataStream Stream;
        if (!Stream.Open(*Path)) return false;
        UE::Trace::FTraceWriter Writer(Stream);
        Writer.Begin();
        Writer.End();
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASNetTraceClockEstimateTest,
    "GASNetTrace.Analysis.ClockEstimateUsesLowestRTT",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGASNetTraceClockEstimateTest::RunTest(const FString&)
{
    FGASNetTraceProvider Provider;
    Provider.AppendClockSample({ 1, 1.0, 1.080, 1.081, 1.201 });
    Provider.AppendClockSample({ 2, 2.0, 2.050, 2.051, 2.101 });
    double Offset = 0.0;
    double Uncertainty = 0.0;
    TestTrue(TEXT("complete sample set produces an estimate"), Provider.EstimateClock(Offset, Uncertainty));
    TestTrue(TEXT("lowest RTT sample selected"), FMath::IsNearlyEqual(Uncertainty, 0.05, 0.001));
    TestTrue(TEXT("offset estimated from four timestamps"), FMath::IsNearlyEqual(Offset, 0.0, 0.001));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASNetTraceSchemaCompatibilityTest,
    "GASNetTrace.Analysis.SchemaV2",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGASNetTraceSchemaCompatibilityTest::RunTest(const FString&)
{
    FGASNetTraceProvider Provider;
    Provider.SetSession(TEXT("capture"), TEXT("Server"), TEXT("DedicatedServer"));
    FGASNetTraceAnalysisEvent Event; Event.EventId = 9007199254740993ull; Event.OwnerId = 7; Event.AvatarId = 8;
    Provider.AppendEvent(MoveTemp(Event));
    Provider.AppendEvent(FGASNetTraceAnalysisEvent{});
    TestEqual(TEXT("schema is v2"), Provider.GetSchemaVersion(), static_cast<uint16>(2));
    TestEqual(TEXT("v2 event ID retained"), Provider.GetEvents()[0].EventId, 9007199254740993ull);
    TestEqual(TEXT("missing ID receives a local monotonic value"), Provider.GetEvents()[1].EventId, static_cast<uint64>(2));
    FGASNetTraceObjectIdentity Identity; Identity.LocalObjectId = 8; Identity.Kind = 2; Identity.Confidence = 3; Identity.StableId = TEXT("iris:18446744073709551615");
    Provider.SetObjectIdentity(MoveTemp(Identity));
    TestTrue(TEXT("Iris identity retained"), Provider.GetObjectIdentities().Contains(8));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASNetTraceThresholdRoundTripTest,
    "GASNetTrace.Analysis.ThresholdRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGASNetTraceThresholdRoundTripTest::RunTest(const FString&)
{
    FGASNetTraceDiagnosticThresholds Expected;
    Expected.ClockUncertaintySeconds = 0.017;
    Expected.MinimumRateSamples = 31;
    const FGASNetTraceDiagnosticThresholds Actual = FGASNetTraceDiagnosticThresholds::FromJson(Expected.ToJson());
    TestTrue(TEXT("clock threshold round trips"), FMath::IsNearlyEqual(Actual.ClockUncertaintySeconds, 0.017));
    TestEqual(TEXT("sample threshold round trips"), Actual.MinimumRateSamples, 31);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASNetTraceGoldenFilesTest,
    "GASNetTrace.Analysis.GoldenFiles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGASNetTraceGoldenFilesTest::RunTest(const FString&)
{
    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("GASNetTrace/GoldenTests"));
    const FString GoldenPath = FPaths::Combine(Directory, TEXT("v2-unknown-field.utrace"));
    const FString LatePath = FPaths::Combine(Directory, TEXT("v2-non-important-session.utrace"));
    const FString EmptyPath = FPaths::Combine(Directory, TEXT("empty.utrace"));
    const FString DamagedPath = FPaths::Combine(Directory, TEXT("v2-damaged-tail.utrace"));
    TestTrue(TEXT("write v2 future-field golden"), WriteGoldenTrace(GoldenPath, true, true));
    TestTrue(TEXT("write missing Important metadata golden"), WriteGoldenTrace(LatePath, false, false));
    TestTrue(TEXT("write empty golden"), WriteEmptyTrace(EmptyPath));
    TestTrue(TEXT("copy damaged-tail golden"), IFileManager::Get().Copy(*DamagedPath, *GoldenPath) == COPY_OK);
    const uint8 DamagedTail[] = { 0xde, 0xad, 0xbe, 0xef, 0x47, 0x4e, 0x54 };
    TestTrue(TEXT("append damaged tail"), FFileHelper::SaveArrayToFile(MakeArrayView(DamagedTail), *DamagedPath, &IFileManager::Get(), FILEWRITE_Append));
    const FGASNetTraceWorkspaceResult Golden = FGASNetTraceWorkspaceAnalyzer::AnalyzeFiles({ GoldenPath });
    const FGASNetTraceWorkspaceResult Late = FGASNetTraceWorkspaceAnalyzer::AnalyzeFiles({ LatePath });
    const FGASNetTraceWorkspaceResult Empty = FGASNetTraceWorkspaceAnalyzer::AnalyzeFiles({ EmptyPath });
    const FGASNetTraceWorkspaceResult Damaged = FGASNetTraceWorkspaceAnalyzer::AnalyzeFiles({ DamagedPath });
    TestTrue(TEXT("v2 ignores unknown field"), Golden.bSucceeded);
    TestTrue(TEXT("full-file analysis recovers non-Important session"), Late.bSucceeded);
    TestFalse(TEXT("empty trace has no usable GAS events"), Empty.bSucceeded);
    TestTrue(TEXT("valid GAS events survive damaged non-GAS tail"), Damaged.bSucceeded);
    TestEqual(TEXT("stable report serialization"), Golden.Json, FGASNetTraceWorkspaceAnalyzer::AnalyzeFiles({ GoldenPath }).Json);
    TSharedPtr<FJsonObject> Report;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Golden.Json);
    TestTrue(TEXT("v2 report is valid JSON"), FJsonSerializer::Deserialize(Reader, Report) && Report.IsValid());
    if (Report)
    {
        TestEqual(TEXT("report schema is v2"), Report->GetStringField(TEXT("schema")), FString(TEXT("gasnettrace.workspace.v2")));
        const TArray<TSharedPtr<FJsonValue>>& Events = Report->GetArrayField(TEXT("events"));
        TestEqual(TEXT("golden has one event"), Events.Num(), 1);
        if (!Events.IsEmpty()) TestEqual(TEXT("64-bit event ID is serialized losslessly inside global ID"), Events[0]->AsObject()->GetStringField(TEXT("id")), FString(TEXT("Golden:9007199254740993")));
    }
    return true;
}

#endif
