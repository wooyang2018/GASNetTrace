using UnrealBuildTool;

public class GASNetTraceRuntime : ModuleRules
{
    public GASNetTraceRuntime(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core", "CoreUObject", "Engine", "GameplayAbilities", "GameplayTags", "GameplayTasks"
        });
        PrivateDependencyModuleNames.AddRange(new[] { "IrisCore", "NetCore", "TraceLog" });
    }
}
