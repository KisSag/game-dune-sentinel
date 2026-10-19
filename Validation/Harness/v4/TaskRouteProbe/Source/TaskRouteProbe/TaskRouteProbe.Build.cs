using UnrealBuildTool;

public class TaskRouteProbe : ModuleRules
{
    public TaskRouteProbe(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "Json", "RenderCore", "RHI" });
    }
}
