#include "GASNetTraceServeCommandlet.h"

#include "GASNetTraceWorkspace.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"
#include "CoreGlobals.h"
#include "Misc/Parse.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    TUniquePtr<FHttpServerResponse> JsonResponse(const FString& Body, EHttpServerResponseCodes Code = EHttpServerResponseCodes::Ok)
    {
        TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(Body, TEXT("application/json; charset=utf-8"));
        Response->Code = Code;
        Response->Headers.Add(TEXT("Access-Control-Allow-Origin"), { TEXT("http://127.0.0.1:4173") });
        Response->Headers.Add(TEXT("Access-Control-Allow-Headers"), { TEXT("X-GAS-Net-Trace-Token, Content-Type") });
        Response->Headers.Add(TEXT("Cache-Control"), { TEXT("no-store") });
        return Response;
    }

    FString SerializeValue(const TSharedPtr<FJsonValue>& Value)
    {
        FString Json;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
        FJsonSerializer::Serialize(Value.ToSharedRef(), TEXT(""), Writer);
        return Json;
    }

    FString SerializeObject(const TSharedRef<FJsonObject>& Object)
    {
        FString Json;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
        FJsonSerializer::Serialize(Object, Writer);
        return Json;
    }

    bool IsAuthorized(const FHttpServerRequest& Request, const FString& Token)
    {
        if (const FString* QueryToken = Request.QueryParams.Find(TEXT("token")); QueryToken && *QueryToken == Token) return true;
        for (const TPair<FString, TArray<FString>>& Header : Request.Headers)
        {
            if (Header.Key.Equals(TEXT("X-GAS-Net-Trace-Token"), ESearchCase::IgnoreCase) && Header.Value.Contains(Token)) return true;
        }
        return false;
    }

    int32 QueryInt(const FHttpServerRequest& Request, const TCHAR* Name, int32 Default)
    {
        if (const FString* Value = Request.QueryParams.Find(Name)) return FCString::Atoi(**Value);
        return Default;
    }
}

UGASNetTraceServeCommandlet::UGASNetTraceServeCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
    ShowErrorCount = false;
    UseCommandletResultAsExitCode = true;
}

int32 UGASNetTraceServeCommandlet::Main(const FString& Params)
{
    FString TracePath;
    FString TracePathsArgument;
    FString WorkspacePath;
    FParse::Value(*Params, TEXT("InputTrace="), TracePath);
    FParse::Value(*Params, TEXT("InputTraces="), TracePathsArgument);
    FParse::Value(*Params, TEXT("Workspace="), WorkspacePath);
    if (TracePath.IsEmpty() && TracePathsArgument.IsEmpty() && WorkspacePath.IsEmpty())
    {
        UE_LOG(LogTemp, Error, TEXT("Missing -Workspace, -InputTrace, or -InputTraces"));
        return 2;
    }

    TArray<FString> TracePaths;
    if (!TracePath.IsEmpty()) TracePaths.Add(TracePath);
    if (!TracePathsArgument.IsEmpty()) TracePathsArgument.ParseIntoArray(TracePaths, TEXT(";"), true);
    for (FString& Path : TracePaths) Path = FPaths::ConvertRelativePathToFull(Path.TrimQuotes());

    const FGASNetTraceWorkspaceResult Analysis = WorkspacePath.IsEmpty()
        ? FGASNetTraceWorkspaceAnalyzer::AnalyzeFiles(TracePaths)
        : FGASNetTraceWorkspaceAnalyzer::AnalyzeWorkspaceFile(WorkspacePath);
    if (!Analysis.bSucceeded)
    {
        UE_LOG(LogTemp, Error, TEXT("Workspace analysis failed: %s"), *Analysis.Error);
        return 3;
    }

    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Analysis.Json);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) return 4;

    int32 Port = 4174;
    int32 DurationSeconds = 0;
    FParse::Value(*Params, TEXT("Port="), Port);
    FParse::Value(*Params, TEXT("Duration="), DurationSeconds);
    Port = FMath::Clamp(Port, 1024, 65535);
    const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);

    const TSharedPtr<IHttpRouter> Router = FHttpServerModule::Get().GetHttpRouter(Port, true);
    if (!Router.IsValid()) return 5;
    TArray<FHttpRouteHandle> Handles;

    auto BindArray = [&](const TCHAR* Route, const TCHAR* Field, bool bPaged)
    {
        Handles.Add(Router->BindRoute(FHttpPath(Route), EHttpServerRequestVerbs::VERB_GET,
            FHttpRequestHandler::CreateLambda([Root, Token, Field = FString(Field), bPaged](const FHttpServerRequest& Request, const FHttpResultCallback& Complete)
        {
            if (!IsAuthorized(Request, Token))
            {
                Complete(JsonResponse(TEXT("{\"error\":\"unauthorized\"}"), EHttpServerResponseCodes::Denied));
                return true;
            }
            const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
            if (!Root->TryGetArrayField(Field, Values) || !Values)
            {
                Complete(JsonResponse(TEXT("[]")));
                return true;
            }
            if (!bPaged)
            {
                Complete(JsonResponse(SerializeValue(MakeShared<FJsonValueArray>(*Values))));
                return true;
            }
            const int32 Offset = FMath::Clamp(QueryInt(Request, TEXT("offset"), 0), 0, Values->Num());
            const int32 Limit = FMath::Clamp(QueryInt(Request, TEXT("limit"), 500), 1, 2000);
            TArray<TSharedPtr<FJsonValue>> Page;
            for (int32 Index = Offset; Index < FMath::Min(Offset + Limit, Values->Num()); ++Index) Page.Add((*Values)[Index]);
            TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetNumberField(TEXT("offset"), Offset);
            Result->SetNumberField(TEXT("limit"), Limit);
            Result->SetNumberField(TEXT("total"), Values->Num());
            Result->SetArrayField(TEXT("items"), MoveTemp(Page));
            Complete(JsonResponse(SerializeObject(Result)));
            return true;
        })));
    };

    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/health")), EHttpServerRequestVerbs::VERB_GET,
        FHttpRequestHandler::CreateLambda([Token, CaptureId = Analysis.CaptureId, Count = Analysis.EventCount](const FHttpServerRequest& Request, const FHttpResultCallback& Complete)
    {
        if (!IsAuthorized(Request, Token)) Complete(JsonResponse(TEXT("{\"error\":\"unauthorized\"}"), EHttpServerResponseCodes::Denied));
        else Complete(JsonResponse(FString::Printf(TEXT("{\"status\":\"ready\",\"captureId\":\"%s\",\"eventCount\":\"%lld\"}"), *CaptureId, Count)));
        return true;
    })));
    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/workspace")), EHttpServerRequestVerbs::VERB_GET,
        FHttpRequestHandler::CreateLambda([Root, Token](const FHttpServerRequest& Request, const FHttpResultCallback& Complete)
    {
        if (!IsAuthorized(Request, Token))
        {
            Complete(JsonResponse(TEXT("{\"error\":\"unauthorized\"}"), EHttpServerResponseCodes::Denied));
            return true;
        }
        TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
        Summary->SetStringField(TEXT("schema"), Root->GetStringField(TEXT("schema")));
        Summary->SetStringField(TEXT("captureId"), Root->GetStringField(TEXT("captureId")));
        for (const TCHAR* Field : { TEXT("endpoints"), TEXT("coverage"), TEXT("analysisWarnings") })
        {
            const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
            if (Root->TryGetArrayField(Field, Values) && Values) Summary->SetArrayField(Field, *Values);
        }
        if (const TSharedPtr<FJsonObject>* Thresholds = nullptr; Root->TryGetObjectField(TEXT("thresholds"), Thresholds) && Thresholds) Summary->SetObjectField(TEXT("thresholds"), *Thresholds);
        Complete(JsonResponse(SerializeObject(Summary)));
        return true;
    })));
    BindArray(TEXT("/api/events"), TEXT("events"), true);
    BindArray(TEXT("/api/network"), TEXT("networkEvidence"), true);
    BindArray(TEXT("/api/chains"), TEXT("causalityChains"), true);
    BindArray(TEXT("/api/diagnostics"), TEXT("diagnostics"), false);

    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/packet")), EHttpServerRequestVerbs::VERB_GET,
        FHttpRequestHandler::CreateLambda([Root, Token](const FHttpServerRequest& Request, const FHttpResultCallback& Complete)
    {
        if (!IsAuthorized(Request, Token))
        {
            Complete(JsonResponse(TEXT("{\"error\":\"unauthorized\"}"), EHttpServerResponseCodes::Denied));
            return true;
        }
        const FString* Id = Request.QueryParams.Find(TEXT("id"));
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (Id && Root->TryGetArrayField(TEXT("networkEvidence"), Values) && Values)
            for (const TSharedPtr<FJsonValue>& Value : *Values)
                if (Value.IsValid() && Value->AsObject()->GetStringField(TEXT("id")) == *Id)
                {
                    Complete(JsonResponse(SerializeValue(Value)));
                    return true;
                }
        Complete(JsonResponse(TEXT("{\"error\":\"packet_not_found\"}"), EHttpServerResponseCodes::NotFound));
        return true;
    })));

    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/metrics")), EHttpServerRequestVerbs::VERB_GET,
        FHttpRequestHandler::CreateLambda([Root, Token](const FHttpServerRequest& Request, const FHttpResultCallback& Complete)
    {
        if (!IsAuthorized(Request, Token)) Complete(JsonResponse(TEXT("{\"error\":\"unauthorized\"}"), EHttpServerResponseCodes::Denied));
        else if (const TSharedPtr<FJsonObject>* Metrics = nullptr; Root->TryGetObjectField(TEXT("metrics"), Metrics) && Metrics) Complete(JsonResponse(SerializeObject(Metrics->ToSharedRef())));
        else Complete(JsonResponse(TEXT("{}")));
        return true;
    })));
    Handles.Add(Router->BindRoute(FHttpPath(TEXT("/api/export")), EHttpServerRequestVerbs::VERB_GET,
        FHttpRequestHandler::CreateLambda([Token, Json = Analysis.Json](const FHttpServerRequest& Request, const FHttpResultCallback& Complete)
    {
        Complete(IsAuthorized(Request, Token) ? JsonResponse(Json) : JsonResponse(TEXT("{\"error\":\"unauthorized\"}"), EHttpServerResponseCodes::Denied));
        return true;
    })));

    if (Handles.ContainsByPredicate([](const FHttpRouteHandle& Handle){ return !Handle.IsValid(); })) return 6;
    FHttpServerModule::Get().StartAllListeners();
    UE_LOG(LogTemp, Display, TEXT("GASNETTRACE_SERVICE_READY url=http://127.0.0.1:%d token=%s events=%lld"), Port, *Token, Analysis.EventCount);

    const double Started = FPlatformTime::Seconds();
    while (!IsEngineExitRequested() && (DurationSeconds <= 0 || FPlatformTime::Seconds() - Started < DurationSeconds))
    {
        FTSTicker::GetCoreTicker().Tick(0.01f);
        FPlatformProcess::Sleep(0.01f);
    }
    FHttpServerModule::Get().StopAllListeners();
    return 0;
}
