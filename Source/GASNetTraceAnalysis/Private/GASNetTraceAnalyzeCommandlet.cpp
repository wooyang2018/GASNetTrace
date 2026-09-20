#include "GASNetTraceAnalyzeCommandlet.h"

#include "GASNetTraceWorkspace.h"
#include "HAL/FileManager.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"

UGASNetTraceAnalyzeCommandlet::UGASNetTraceAnalyzeCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
    // Other trace channels may contain recoverable/corrupt tails. The workspace
    // result, not unrelated TraceServices log severity, owns this exit code.
    ShowErrorCount = false;
    UseCommandletResultAsExitCode = true;
}

int32 UGASNetTraceAnalyzeCommandlet::Main(const FString& Params)
{
    FString TracePath;
    FString TracePathsArgument;
    FString WorkspacePath;
    FString OutputDirectory;
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

    FParse::Value(*Params, TEXT("Output="), OutputDirectory);
    if (OutputDirectory.IsEmpty()) OutputDirectory = WorkspacePath.IsEmpty() ? FPaths::GetPath(TracePaths[0]) : FPaths::GetPath(FPaths::ConvertRelativePathToFull(WorkspacePath));
    OutputDirectory = FPaths::ConvertRelativePathToFull(OutputDirectory);
    for (const FString& Path : TracePaths)
    {
        if (!FPaths::FileExists(Path))
        {
            UE_LOG(LogTemp, Error, TEXT("Trace does not exist: %s"), *Path);
            return 3;
        }
    }
    IFileManager::Get().MakeDirectory(*OutputDirectory, true);

    const FGASNetTraceWorkspaceResult Result = WorkspacePath.IsEmpty()
        ? FGASNetTraceWorkspaceAnalyzer::AnalyzeFiles(TracePaths)
        : FGASNetTraceWorkspaceAnalyzer::AnalyzeWorkspaceFile(WorkspacePath);
    if (!Result.bSucceeded)
    {
        UE_LOG(LogTemp, Error, TEXT("Trace analysis failed: %s"), *Result.Error);
        return 4;
    }

    const FString ReportPath = FPaths::Combine(OutputDirectory, TEXT("gas-net-trace-report.json"));
    if (!FFileHelper::SaveStringToFile(Result.Json, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to write report: %s"), *ReportPath);
        return 5;
    }
    UE_LOG(LogTemp, Display, TEXT("GAS Net Trace workspace report (%lld events): %s"), Result.EventCount, *ReportPath);
    return 0;
}
