using UnrealBuildTool;

public class GASNetTraceAnalysis : ModuleRules
{
    public GASNetTraceAnalysis(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "TraceServices" });
        PrivateDependencyModuleNames.AddRange(new[] { "TraceAnalysis", "Json", "JsonUtilities", "HTTPServer" });
    }
}
