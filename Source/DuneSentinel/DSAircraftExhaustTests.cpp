#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "DSAircraftExhaustComponent.h"
#include "DSFlightPawn.h"
#include "DSEnemyAircraft.h"
#include "DSGameMode.h"
#include "DSImpactFX.h"
#include "DSProjectile.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/PrimitiveComponent.h"
#include "Components/MaterialBillboardComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "InputAction.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "MaterialShared.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "SceneInterface.h"
#include "RenderingThread.h"
#include "ShaderCompiler.h"
#include "UnrealClient.h"

namespace
{
// Observe ordinary flight and its real rendered exhaust in the delivered map.
// Framebuffer bytes never leave memory; only numeric evidence is persisted.
class FDSAircraftExhaustAcceptanceCommand final : public IAutomationLatentCommand
{
public:
    explicit FDSAircraftExhaustAcceptanceCommand(FAutomationTestBase* InTest) : Test(InTest) {}
    virtual ~FDSAircraftExhaustAcceptanceCommand() override { RemoveDelegates(); }

    virtual bool Update() override
    {
        if (bFinished) return true;
        if (GetCurrentRunTime() > 90.) return Finish(TEXT("Aircraft exhaust acceptance exceeded 90 seconds."));
        if (!bInitialized) return Initialize();
        if (!World.IsValid() || !Mode.IsValid() || !Mode->Player || !PC.IsValid() || !Camera.IsValid())
            return Finish(TEXT("The real gameplay observation world became unavailable."));
        if (Stage == EStage::Cleanup)
        {
            if (GFrameCounter < StageFrame + 3) return false;
            bool bUnregistered = true;
            for (const TWeakObjectPtr<UPrimitiveComponent>& Primitive : DestroyedPrimitives)
                bUnregistered &= !Primitive.IsValid() || !Primitive->IsRegistered();
            Check(TEXT("Destroyed hostile aircraft unregisters its entire visual pool"), bUnregistered);
            if (++AirframeIndex == 5) return Finish();
            BeginAirframe();
            return false;
        }
        if (!VisualActor.IsValid() || !Exhaust.IsValid()) return Finish(TEXT("The observed aircraft or its exhaust is unavailable."));
        if (Stage == EStage::DriveInput)
        {
            // Input is injected before the real controller ticks. Allow several
            // frames for Enhanced Input to dispatch the normal bound handlers.
            if (World->GetTimeSeconds() - StageTime < .25f || GFrameCounter < StageFrame + 4) return false;
            SettleFlightInput();
            return false;
        }
        if (Stage == EStage::RenderReady)
        {
            if (!RenderingResourcesReady()) return false;
            if (ConsecutiveReadyFrames < 3) return false;
            RecordReadiness();
            Exhaust->SetExhaustVisible(false);
            Stage = EStage::Baseline;
            ResetStageClock();
            return false;
        }
        if (World->GetTimeSeconds() - StageTime < .65f || GFrameCounter < StageFrame + 6) return false;
        // Visibility can schedule another render proxy or pipeline request.
        // Recheck the independent readiness criteria before either readback.
        if (!RenderingResourcesReady() || ConsecutiveReadyFrames < 3) return false;
        const bool bShouldBeVisible = Stage == EStage::Visible;
        if (Exhaust->IsExhaustVisible() != bShouldBeVisible ||
            (bShouldBeVisible ? Exhaust->GetVisiblePrimitiveCount() == 0 : Exhaust->GetVisiblePrimitiveCount() != 0))
            return Finish(TEXT("The isolated exhaust comparison has inconsistent visibility."));
        TArray<FColor>& Destination = bShouldBeVisible ? VisiblePixels : BaselinePixels;
        const EReadback Result = ReadViewport(Destination);
        if (Result == EReadback::Pending) return false;
        if (Result == EReadback::Failed) return Finish(*ReadbackError);
        if (!bShouldBeVisible)
        {
            if (!BuildRegion()) return Finish(TEXT("The complete exhaust observation region is not inside the view."));
            Exhaust->SetExhaustVisible(true);
            Stage = EStage::Visible;
            ConsecutiveReadyFrames = 0;
            ResetStageClock();
        }
        else
        {
            if (BaselinePixels.Num() != VisiblePixels.Num()) return Finish(TEXT("Viewport dimensions changed between exhaust frames."));
            RecordReadiness();
            CompareFrames();
            BaselinePixels.Reset();
            VisiblePixels.Reset();
            if (AirframeIndex < 3 && ++ThrottleState < 3) BeginDriveInput();
            else FinishAirframe();
        }
        return bFinished;
    }

private:
    enum class EStage { DriveInput, RenderReady, Baseline, Visible, Cleanup };
    enum class EReadback { Pending, Ready, Failed };
    struct FStateMeasurement
    {
        float Length = 0.f, Radius = 0.f, Strength = 0.f, Speed = 0.f;
        int32 ChangedPixels = 0;
        double LightEnergy = 0.;
    };
    struct FRenderedMaterialState
    {
        bool bNoFallbackMaterial = false, bShaderMap = false, bExpectedMaterial = false, bCompleteMap = false, bValidForRendering = false;
        int32 ShaderPlatform = -1, ShaderCount = 0, LocalVertexFactoryShaderCount = 0;
    };
    FAutomationTestBase* Test;
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<ADSGameMode> Mode;
    TWeakObjectPtr<APlayerController> PC;
    TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> Input;
    TWeakObjectPtr<ACameraActor> Camera;
    TWeakObjectPtr<AActor> OriginalViewTarget, VisualActor;
    TWeakObjectPtr<UDSAircraftExhaustComponent> Exhaust;
    TArray<TWeakObjectPtr<UPrimitiveComponent>> DestroyedPrimitives;
    TArray<FString> Checks, Observations;
    TArray<FString> ReadinessMaterialDiagnostics;
    TArray<FColor> BaselinePixels, VisiblePixels, PendingPixels;
    FStateMeasurement Measurements[3];
    FDelegateHandle PreTickHandle, ViewportRenderedHandle;
    FIntPoint ViewSize = FIntPoint::ZeroValue;
    FIntRect ObservationRegion;
    FString OutputDir, ReadbackError;
    EStage Stage = EStage::DriveInput;
    float StageTime = 0.f, InjectThrottle = 0.f, InjectBrake = 0.f;
    uint64 StageFrame = 0;
    double ReadbackRequestedAt = 0.;
    int32 AirframeIndex = 0, ThrottleState = 0, OriginalSelection = 0;
    int32 ReadinessWaitFrames = 0, MaterialWaitFrames = 0, PipelineWaitFrames = 0, ProxyWaitFrames = 0, MeshWaitFrames = 0;
    int32 CompilerWaitFrames = 0, RemainingShaderJobs = 0, PeakShaderJobs = 0;
    int32 ConsecutiveReadyFrames = 0, ReadinessMaterialCount = 0, ReadinessPrimitiveCount = 0;
    uint64 LastReadinessFrame = MAX_uint64;
    bool bInitialized = false, bStateCaptured = false, bFinished = false;
    bool bReadbackRequested = false, bReadbackReady = false;
    bool bOriginalModeTick = true, bOriginalPlayerTick = true, bOriginalAutoCamera = true;
    bool bOriginalHUD = true, bOriginalPlayerHidden = false;
    const FVector TestOrigin = FVector(-400000.f, -400000.f, 500000.f);

    const TCHAR* StateName() const
    {
        return ThrottleState == 0 ? TEXT("cruise") : (ThrottleState == 1 ? TEXT("acceleration") : TEXT("braking"));
    }

    FString AirframeName() const
    {
        static const TCHAR* Names[] = { TEXT("player_F16"), TEXT("player_Rafale"), TEXT("player_F22"), TEXT("hostile_F22"), TEXT("hostile_Tu22") };
        return Names[FMath::Clamp(AirframeIndex, 0, 4)];
    }

    const TCHAR* ExpectedMeshName() const
    {
        static const TCHAR* Names[] = { TEXT("SM_AU_F16"), TEXT("SM_AU_Rafale"), TEXT("SM_AU_F22"), TEXT("SM_AU_F22"), TEXT("SM_AU_Tu22") };
        return Names[FMath::Clamp(AirframeIndex, 0, 4)];
    }

    TArray<FVector> MeasuredNozzleLips() const
    {
        // Independent source conversion measurements from
        // RawAssets/Aircraft/Downloaded/meshes.json. Do not obtain expected
        // geometry from the runtime sockets being tested. Tu-22 measurements
        // are retained in the adjacent bomber-meshes.json source report.
        if (AirframeIndex == 0) return { FVector(-692.355442, .000027, 0.) };
        if (AirframeIndex == 1) return { FVector(-660.867739, -79.402328, 0.), FVector(-660.867739, 79.402447, 0.) };
        if (AirframeIndex == 2 || AirframeIndex == 3)
            return { FVector(-726.082039, -72.604603, 0.), FVector(-726.082039, 72.604603, 0.) };
        return { FVector(-1939.948654, 79.498124, -.000012), FVector(-1939.948654, -79.497933, .000012) };
    }

    float MeasuredNozzleRadius() const
    {
        static const float Radii[] = { 59.038037f, 42.135217f, 47.946436f, 47.946436f, 54.562438f };
        return Radii[FMath::Clamp(AirframeIndex, 0, 4)];
    }

    void Check(const TCHAR* Name, bool bPassed)
    {
        Test->TestTrue(Name, bPassed);
        Checks.Add(FString::Printf(TEXT("{\"name\":\"%s\",\"passed\":%s}"), Name, bPassed ? TEXT("true") : TEXT("false")));
    }

    void CheckForAirframe(const TCHAR* Name, bool bPassed)
    {
        Check(*FString::Printf(TEXT("%s: %s"), *AirframeName(), Name), bPassed);
    }

    void ResetStageClock()
    {
        StageTime = World->GetTimeSeconds();
        StageFrame = GFrameCounter;
    }

    bool Initialize()
    {
        if (!GEngine) return false;
        UWorld* GameWorld = nullptr;
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
            if (Context.WorldType == EWorldType::Game && Context.World() && Context.World()->HasBegunPlay()) GameWorld = Context.World();
        if (!GameWorld || !GEngine->GameViewport || !GEngine->GameViewport->Viewport) return false;
        World = GameWorld;
        Mode = GameWorld->GetAuthGameMode<ADSGameMode>();
        PC = UGameplayStatics::GetPlayerController(GameWorld, 0);
        if (!Mode.IsValid() || !Mode->Player || !PC.IsValid() || !PC->GetLocalPlayer())
            return Finish(TEXT("DesertDefense must provide its actual game mode, player and local controller."));
        Input = PC->GetLocalPlayer()->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
        if (!Input.IsValid() || Mode->Aircraft.Num() != 3) return Finish(TEXT("The three playable aircraft and Enhanced Input are required."));
        OriginalSelection = Mode->SelectedAircraft;
        bOriginalModeTick = Mode->IsActorTickEnabled();
        bOriginalPlayerTick = Mode->Player->IsActorTickEnabled();
        bOriginalPlayerHidden = Mode->Player->IsHidden();
        bOriginalAutoCamera = PC->bAutoManageActiveCameraTarget;
        OriginalViewTarget = PC->GetViewTarget();
        if (PC->GetHUD()) bOriginalHUD = PC->GetHUD()->bShowHUD;
        bStateCaptured = true;
        OutputDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Validation/AircraftExhaust") /
            (FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")) + TEXT("-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8)));
        IFileManager::Get().MakeDirectory(*OutputDir, true);
        Mode->SetActorTickEnabled(false);
        Mode->Player->SetActorTickEnabled(false);
        if (PC->GetHUD()) PC->GetHUD()->bShowHUD = false;
        PC->bAutoManageActiveCameraTarget = false;
        Camera = GameWorld->SpawnActor<ACameraActor>();
        if (!Camera.IsValid()) return Finish(TEXT("Could not create the numeric observation camera."));
        Camera->GetCameraComponent()->SetFieldOfView(60.f);
        PreTickHandle = FWorldDelegates::OnWorldPreActorTick.AddRaw(this, &FDSAircraftExhaustAcceptanceCommand::BeforeTick);
        ViewportRenderedHandle = UGameViewportClient::OnViewportRendered().AddRaw(this, &FDSAircraftExhaustAcceptanceCommand::AfterViewportRendered);
        bInitialized = true;
        BeginAirframe();
        return false;
    }

    void BeforeTick(UWorld* TickWorld, ELevelTick, float)
    {
        if (TickWorld != World.Get() || !Input.IsValid() || !Mode.IsValid() || !Mode->Player) return;
        if (Mode->Player->ThrottleAction) Input->InjectInputForAction(Mode->Player->ThrottleAction, FInputActionValue(InjectThrottle), {}, {});
        if (Mode->Player->BrakeAction) Input->InjectInputForAction(Mode->Player->BrakeAction, FInputActionValue(InjectBrake), {}, {});
    }

    void RemoveDelegates()
    {
        if (PreTickHandle.IsValid()) FWorldDelegates::OnWorldPreActorTick.Remove(PreTickHandle);
        if (ViewportRenderedHandle.IsValid()) UGameViewportClient::OnViewportRendered().Remove(ViewportRenderedHandle);
        PreTickHandle.Reset();
        ViewportRenderedHandle.Reset();
        bReadbackRequested = false;
    }

    void ClearCombatActors()
    {
        for (TActorIterator<ADSEnemyAircraft> It(World.Get()); It; ++It) It->Destroy();
        for (TActorIterator<ADSProjectile> It(World.Get()); It; ++It) It->Destroy();
        for (TActorIterator<ADSBomb> It(World.Get()); It; ++It) It->Destroy();
        for (TActorIterator<ADSImpactFX> It(World.Get()); It; ++It) It->Destroy();
        Mode->Enemies.Reset();
    }

    void BeginAirframe()
    {
        ThrottleState = 0;
        ReadinessWaitFrames = MaterialWaitFrames = PipelineWaitFrames = ProxyWaitFrames = MeshWaitFrames = CompilerWaitFrames = PeakShaderJobs = 0;
        for (FStateMeasurement& Value : Measurements) Value = FStateMeasurement();
        InjectThrottle = InjectBrake = 0.f;
        Mode->ReturnToMenu();
        if (AirframeIndex < 3) Mode->SelectAircraft(AirframeIndex);
        Mode->StartSortie();
        ClearCombatActors();
        Mode->RadioLine.Empty();
        if (PC->PlayerCameraManager) PC->PlayerCameraManager->StopCameraFade();
        Mode->Player->SetActorTickEnabled(false);
        Mode->Player->SetActorLocationAndRotation(TestOrigin, FRotator::ZeroRotator);
        Mode->Player->SetActorHiddenInGame(AirframeIndex >= 3);
        if (AirframeIndex < 3)
        {
            VisualActor = Mode->Player.Get();
            Exhaust = Mode->Player->GetAircraftExhaust();
        }
        else
        {
            Mode->Player->Park();
            Mode->Player->SetActorLocation(TestOrigin + FVector(0.f, 200000.f, 0.f));
            FActorSpawnParameters Params;
            Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            ADSEnemyAircraft* Enemy = World->SpawnActor<ADSEnemyAircraft>(TestOrigin, FRotator::ZeroRotator, Params);
            if (!Enemy) { Finish(TEXT("Could not spawn the hostile aircraft.")); return; }
            Enemy->Initialize(AirframeIndex == 4, Mode->AirportLocation);
            Enemy->SetActorTickEnabled(false);
            Mode->Enemies.Add(Enemy);
            Enemy->Tick(.02f);
            Enemy->SetActorLocationAndRotation(TestOrigin, FRotator::ZeroRotator);
            VisualActor = Enemy;
            Exhaust = Enemy->GetAircraftExhaust();
        }
        if (!Exhaust.IsValid()) { Finish(TEXT("An aircraft has no engine exhaust component.")); return; }
        // The same fixed oblique chase view is retained for all three power
        // settings. The nose is to the right and the trailing flame is clear.
        // The longer bomber's nozzles sit farther aft. Translate the whole
        // observation rig without changing its distance, angle or field of view.
        const FVector CameraShift(AirframeIndex == 4 ? -1200.f : 0.f, 0.f, 0.f);
        const FVector Center = TestOrigin + CameraShift + FVector(-1700.f, 0.f, 0.f);
        const FVector Position = TestOrigin + CameraShift + FVector(-4300.f, -7200.f, 2100.f);
        Camera->SetActorLocationAndRotation(Position, (Center - Position).Rotation());
        PC->SetViewTarget(Camera.Get());
        if (PC->PlayerCameraManager) PC->PlayerCameraManager->UpdateCamera(0.f);
        BeginDriveInput();
    }

    void BeginDriveInput()
    {
        InjectThrottle = ThrottleState == 1 ? 1.f : 0.f;
        InjectBrake = ThrottleState == 2 ? 1.f : 0.f;
        Stage = EStage::DriveInput;
        ConsecutiveReadyFrames = 0;
        ResetStageClock();
    }

    void SettleFlightInput()
    {
        if (AirframeIndex < 3)
        {
            const float PreviousSpeed = Mode->Player->Speed;
            // Advance normal aircraft Tick with the dispatched input values,
            // keeping the collision-safe high-altitude pose between steps.
            for (int32 I = 0; I < 60; ++I)
            {
                Mode->Player->Tick(1.f / 60.f);
                Mode->Player->SetActorLocationAndRotation(TestOrigin, FRotator::ZeroRotator);
            }
            Measurements[ThrottleState].Speed = Mode->Player->Speed;
            if (ThrottleState == 1) CheckForAirframe(TEXT("Enhanced Input throttle increases actual flight speed"), Mode->Player->Speed > PreviousSpeed + 10.f);
            if (ThrottleState == 2) CheckForAirframe(TEXT("Enhanced Input brake decreases actual flight speed"), Mode->Player->Speed < PreviousSpeed - 10.f);
        }
        FStateMeasurement& Value = Measurements[ThrottleState];
        Value.Length = Exhaust->GetFlameLengthCm();
        Value.Radius = Exhaust->GetCoreRadiusCm();
        Value.Strength = Exhaust->GetGlowStrength();
        CheckForAirframe(*FString::Printf(TEXT("%s exhaust has authored materials and visible nonzero geometry"), StateName()),
            Exhaust->HasReadyMaterials() && Exhaust->IsExhaustVisible() && Exhaust->GetVisiblePrimitiveCount() > 0 &&
            Value.Length > 10.f && Value.Radius > 1.f && Value.Strength > 0.f);
        if (ThrottleState == 0) RunAttachmentAndPoolChecks();
        // Keep real exhaust visible while its shaders and render proxies become
        // ready. Measurements never poll pixels to decide whether to retry.
        Stage = EStage::RenderReady;
        ConsecutiveReadyFrames = 0;
        ResetStageClock();
    }

    bool RenderingResourcesReady()
    {
        TArray<USceneComponent*> Children;
        Exhaust->GetChildrenComponents(true, Children);
        TSet<UMaterialInterface*> Materials;
        bool bMaterialsReady = World->Scene != nullptr, bPipelinesReady = true, bProxiesReady = true, bMeshesReady = true;
        const bool bRequireVisibleProxy = Stage != EStage::Baseline;
        ReadinessMaterialDiagnostics.Reset();
        ReadinessPrimitiveCount = 0;
        for (USceneComponent* Child : Children)
        {
            UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Child);
            if (!Primitive) continue;
            ++ReadinessPrimitiveCount;
            bPipelinesReady &= !Primitive->CheckPSOPrecachingAndBoostPriority();
            bProxiesReady &= Primitive->IsRegistered() && Primitive->IsRenderStateCreated() &&
                (!bRequireVisibleProxy || Primitive->GetSceneProxy() != nullptr);
            if (UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Primitive))
                bMeshesReady &= Mesh->GetStaticMesh() && !Mesh->GetStaticMesh()->IsCompiling();
            const TArray<UMaterialInterface*> PrimitiveMaterials = ActualMaterials(Primitive);
            bMaterialsReady &= !PrimitiveMaterials.IsEmpty();
            for (UMaterialInterface* Interface : PrimitiveMaterials)
                if (Interface) Materials.Add(Interface);
                else bMaterialsReady = false;
        }
        if (UStaticMeshComponent* Mesh = AirframeMesh())
            bMeshesReady &= Mesh->GetStaticMesh() && !Mesh->GetStaticMesh()->IsCompiling() && Mesh->GetSceneProxy() != nullptr;
        else bMeshesReady = false;
        ReadinessMaterialCount = Materials.Num();
        bMaterialsReady &= Materials.Num() > 0;
        TArray<UMaterialInterface*> ActualInterfaces = Materials.Array();
        TArray<const FMaterialRenderProxy*> RenderProxies;
        TArray<UMaterial*> ExpectedMaterials;
        TArray<FRenderedMaterialState> RenderStates;
        for (UMaterialInterface* Interface : ActualInterfaces)
        {
            RenderProxies.Add(Interface->GetRenderProxy());
            ExpectedMaterials.Add(Interface->GetMaterial());
        }
        RenderStates.SetNum(ActualInterfaces.Num());
        if (World->Scene)
        {
            const ERHIFeatureLevel::Type FeatureLevel = World->Scene->GetFeatureLevel();
            // Query precisely the proxy used by mesh and billboard draws. This
            // API forbids fallback materials, and render resources are only
            // inspected on their owning thread. The flush also makes the small
            // result array safe to consume on the game thread below.
            ENQUEUE_RENDER_COMMAND(DSObserveAircraftExhaustMaterials)(
                [RenderProxies, ExpectedMaterials, FeatureLevel, &RenderStates](FRHICommandListImmediate&)
                {
                    for (int32 I = 0; I < RenderProxies.Num(); ++I)
                    {
                        const FMaterial* Material = RenderProxies[I] ? RenderProxies[I]->GetMaterialNoFallback(FeatureLevel) : nullptr;
                        FRenderedMaterialState& State = RenderStates[I];
                        State.bNoFallbackMaterial = Material != nullptr;
                        if (Material)
                        {
                            const FMaterialShaderMap* ShaderMap = Material->GetRenderingThreadShaderMap();
                            State.bShaderMap = ShaderMap != nullptr;
                            State.bExpectedMaterial = Material->GetMaterialInterface() == ExpectedMaterials[I];
                            State.bCompleteMap = Material->IsRenderingThreadShaderMapComplete();
                            State.ShaderPlatform = static_cast<int32>(Material->GetShaderPlatform());
                            if (ShaderMap)
                            {
                                State.bValidForRendering = ShaderMap->IsValidForRendering();
                                if (State.bValidForRendering)
                                {
                                    int32 PipelineCount = 0;
                                    ShaderMap->CountNumShaders(State.ShaderCount, PipelineCount);
                                    const FMeshMaterialShaderMap* LocalFactory = ShaderMap->GetMeshShaderMap(FHashedName(TEXT("FLocalVertexFactory")));
                                    State.LocalVertexFactoryShaderCount = LocalFactory ? LocalFactory->GetNumShaders() : 0;
                                }
                            }
                        }
                    }
                });
            FlushRenderingCommands();
        }
        for (int32 I = 0; I < ActualInterfaces.Num(); ++I)
        {
            UMaterialInterface* Interface = ActualInterfaces[I];
            UMaterial* Material = Interface->GetMaterial();
            const FMaterialResource* Resource = World->Scene ? Interface->GetMaterialResource(World->Scene->GetShaderPlatform()) : nullptr;
            // Retain the game-thread resource state for diagnosis, separate
            // from the actual draw proxy observed above.
            bool bCompilationFinished = Resource != nullptr;
#if WITH_EDITOR
            // This call also polls/finalizes pending cached shaders. Evaluate it
            // before the predicates; a global incomplete flag must not short
            // circuit the operation that can make this resource complete.
            bCompilationFinished &= Resource && Resource->IsCompilationFinished();
#endif
            const bool bValidMap = Resource && Resource->HasValidGameThreadShaderMap();
            const bool bCompleteMap = Resource && Resource->IsGameThreadShaderMapComplete();
            const FRenderedMaterialState& RenderState = RenderStates[I];
            bMaterialsReady &= RenderState.bNoFallbackMaterial && RenderState.bShaderMap && RenderState.bExpectedMaterial &&
                RenderState.bValidForRendering && RenderState.ShaderCount > 0 && RenderState.LocalVertexFactoryShaderCount > 0;
            ReadinessMaterialDiagnostics.Add(FString::Printf(TEXT("{\"instance\":\"%s\",\"class\":\"%s\",\"base_material\":\"%s\",\"scene_shader_platform\":%d,\"scene_feature_level\":%d,\"has_resource\":%s,\"compilation_finished\":%s,\"valid_shader_map\":%s,\"complete_shader_map\":%s,\"base_all_levels_complete\":%s,\"base_any_level_compiling\":%s,\"render_no_fallback_material\":%s,\"render_shader_map\":%s,\"render_expected_material\":%s,\"render_complete_shader_map\":%s,\"render_shader_platform\":%d,\"render_valid_for_rendering\":%s,\"render_shader_count\":%d,\"render_local_vertex_factory_shader_count\":%d}"),
                *Interface->GetPathName(), *Interface->GetClass()->GetName(), Material ? *Material->GetPathName() : TEXT("missing"),
                World->Scene ? static_cast<int32>(World->Scene->GetShaderPlatform()) : -1, World->Scene ? static_cast<int32>(World->Scene->GetFeatureLevel()) : -1,
                Resource ? TEXT("true") : TEXT("false"), bCompilationFinished ? TEXT("true") : TEXT("false"),
                bValidMap ? TEXT("true") : TEXT("false"), bCompleteMap ? TEXT("true") : TEXT("false"),
                Material && Material->IsComplete() ? TEXT("true") : TEXT("false"), Material && Material->IsCompiling() ? TEXT("true") : TEXT("false"),
                RenderState.bNoFallbackMaterial ? TEXT("true") : TEXT("false"), RenderState.bShaderMap ? TEXT("true") : TEXT("false"),
                RenderState.bExpectedMaterial ? TEXT("true") : TEXT("false"), RenderState.bCompleteMap ? TEXT("true") : TEXT("false"), RenderState.ShaderPlatform,
                RenderState.bValidForRendering ? TEXT("true") : TEXT("false"), RenderState.ShaderCount, RenderState.LocalVertexFactoryShaderCount));
        }
        bool bCompilerIdle = true;
        RemainingShaderJobs = 0;
#if WITH_EDITOR
        if (GShaderCompilingManager)
        {
            // On-demand job caching permits valid render maps without a fully
            // finalized material map. Wait for requested jobs while the real
            // effects are visible, rather than requiring unused permutations.
            bCompilerIdle = !GShaderCompilingManager->IsCompiling();
            RemainingShaderJobs = GShaderCompilingManager->GetNumRemainingJobs();
            PeakShaderJobs = FMath::Max(PeakShaderJobs, RemainingShaderJobs);
        }
#endif
        const bool bReady = ReadinessPrimitiveCount == Exhaust->GetVisualPoolSize() && bMaterialsReady && bPipelinesReady && bProxiesReady && bMeshesReady && bCompilerIdle;
        if (LastReadinessFrame != GFrameCounter)
        {
            LastReadinessFrame = GFrameCounter;
            if (bReady) ++ConsecutiveReadyFrames;
            else
            {
                ConsecutiveReadyFrames = 0;
                ++ReadinessWaitFrames;
                MaterialWaitFrames += bMaterialsReady ? 0 : 1;
                PipelineWaitFrames += bPipelinesReady ? 0 : 1;
                ProxyWaitFrames += bProxiesReady ? 0 : 1;
                MeshWaitFrames += bMeshesReady ? 0 : 1;
                CompilerWaitFrames += bCompilerIdle ? 0 : 1;
            }
        }
        if (!bReady && World->GetTimeSeconds() - StageTime > 20.f)
        {
            RecordReadiness();
            Finish(TEXT("Aircraft exhaust rendering resources did not become ready within twenty seconds; no visual measurement was accepted."));
        }
        return bReady;
    }

    void RecordReadiness()
    {
        const IConsoleVariable* JobCache = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ShaderCompiler.JobCacheDDC"));
        Observations.Add(FString::Printf(TEXT("{\"case\":\"independent_render_readiness\",\"airframe\":\"%s\",\"power\":\"%s\",\"actual_material_instances\":%d,\"actual_primitives\":%d,\"consecutive_ready_frames\":%d,\"cumulative_wait_frames\":%d,\"incomplete_material_frames\":%d,\"pending_pipeline_frames\":%d,\"missing_proxy_frames\":%d,\"pending_mesh_frames\":%d,\"shader_job_wait_frames\":%d,\"remaining_shader_jobs\":%d,\"peak_shader_jobs\":%d,\"shader_job_cache_ddc\":%d,\"materials\":[%s]}"),
            *AirframeName(), StateName(), ReadinessMaterialCount, ReadinessPrimitiveCount, ConsecutiveReadyFrames,
            ReadinessWaitFrames, MaterialWaitFrames, PipelineWaitFrames, ProxyWaitFrames, MeshWaitFrames,
            CompilerWaitFrames, RemainingShaderJobs, PeakShaderJobs, JobCache ? JobCache->GetInt() : -1, *FString::Join(ReadinessMaterialDiagnostics, TEXT(","))));
    }

    TArray<UMaterialInterface*> ActualMaterials(UPrimitiveComponent* Primitive) const
    {
        TArray<UMaterialInterface*> Result;
        // Billboard components expose their material slots through Elements;
        // their inherited GetNumMaterials returns zero despite active cores.
        if (const UMaterialBillboardComponent* Billboard = Cast<UMaterialBillboardComponent>(Primitive))
            for (const FMaterialSpriteElement& Element : Billboard->Elements) Result.Add(Element.Material.Get());
        else
            for (int32 Slot = 0; Slot < Primitive->GetNumMaterials(); ++Slot) Result.Add(Primitive->GetMaterial(Slot));
        return Result;
    }

    UStaticMeshComponent* AirframeMesh() const
    {
        if (const ADSFlightPawn* Player = Cast<ADSFlightPawn>(VisualActor.Get())) return Player->GetAirframeMesh();
        if (const ADSEnemyAircraft* Enemy = Cast<ADSEnemyAircraft>(VisualActor.Get())) return Enemy->Mesh;
        return nullptr;
    }

    void RunAttachmentAndPoolChecks()
    {
        const TArray<FVector> Nozzles = Exhaust->GetNozzleWorldLocations();
        const TArray<FVector> MeasuredLips = MeasuredNozzleLips();
        const int32 ExpectedNozzles = MeasuredLips.Num();
        UStaticMeshComponent* Mesh = AirframeMesh();
        const bool bExpectedMesh = Mesh && Mesh->GetStaticMesh() && Mesh->GetStaticMesh()->GetName() == ExpectedMeshName();
        CheckForAirframe(TEXT("Normal flight uses the expected detailed imported airframe"), bExpectedMesh);
        bool bRearAndOnBody = bExpectedMesh && Nozzles.Num() == ExpectedNozzles && Nozzles.Num() == Exhaust->GetNozzleCount();
        if (Mesh && Mesh->GetStaticMesh())
        {
            const FBox LocalBounds = Mesh->GetStaticMesh()->GetBoundingBox();
            for (const FVector& Position : Nozzles)
            {
                const FVector Local = Mesh->GetComponentTransform().InverseTransformPosition(Position);
                // Source bounds are independent of the effect's placement
                // configuration: reject front-mounted or far detached jets.
                bRearAndOnBody &= Local.X < LocalBounds.GetCenter().X - LocalBounds.GetExtent().X * .2f &&
                    Local.X >= LocalBounds.Min.X - 100.f && Local.X <= LocalBounds.Max.X &&
                    FMath::Abs(Local.Y) <= LocalBounds.GetExtent().Y &&
                    Local.Z >= LocalBounds.Min.Z - 60.f && Local.Z <= LocalBounds.Max.Z + 60.f;
            }
        }
        CheckForAirframe(TEXT("Nozzles lie at the rear of the actual loaded airframe"), bRearAndOnBody);
        // Permit only the authored two-centimetre rear clearance; reject
        // misplaced, duplicated or swapped left/right nozzle roots.
        bool bAtMeasuredLips = bExpectedMesh && Nozzles.Num() == ExpectedNozzles;
        double MaxLipError = 0.;
        for (int32 I = 0; bAtMeasuredLips && I < Nozzles.Num(); ++I)
        {
            const FVector Local = Mesh->GetComponentTransform().InverseTransformPosition(Nozzles[I]);
            MaxLipError = FMath::Max(MaxLipError, FVector::Distance(Local, MeasuredLips[I]));
        }
        CheckForAirframe(TEXT("Every engine root matches its independently measured source nozzle rim"), bAtMeasuredLips && MaxLipError <= 3.);
        const TArray<float>& Radii = Exhaust->GetNozzleRadiiCm();
        bool bMeasuredRadii = Radii.Num() == ExpectedNozzles;
        for (float Radius : Radii) bMeasuredRadii &= FMath::Abs(Radius - MeasuredNozzleRadius()) < .01f;
        CheckForAirframe(TEXT("Flame dimensions use independently measured source nozzle radii"), bMeasuredRadii);
        TArray<USceneComponent*> Children;
        Exhaust->GetChildrenComponents(true, Children);
        TArray<FVector> LocalVisualPositions;
        bool bAuthoredAdditiveLight = !Children.IsEmpty(), bFlamesBehindNozzles = true;
        int32 ActualPrimitiveCount = 0, ActualFlameCards = 0;
        for (USceneComponent* Child : Children)
        {
            LocalVisualPositions.Add(VisualActor->GetActorTransform().InverseTransformPosition(Child->GetComponentLocation()));
            UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Child);
            if (!Primitive) continue;
            ++ActualPrimitiveCount;
            bAuthoredAdditiveLight &= Primitive->GetCollisionEnabled() == ECollisionEnabled::NoCollision;
            const TArray<UMaterialInterface*> PrimitiveMaterials = ActualMaterials(Primitive);
            bAuthoredAdditiveLight &= !PrimitiveMaterials.IsEmpty();
            for (UMaterialInterface* Interface : PrimitiveMaterials)
            {
                UMaterial* Material = Interface ? Interface->GetMaterial() : nullptr;
                bAuthoredAdditiveLight &= Material && Material->GetPathName().StartsWith(TEXT("/Game/Art/Effects/M_AircraftExhaust")) &&
                    Material->GetBlendMode() == BLEND_Additive && Material->GetShadingModels().IsUnlit();
            }
            if (Cast<UStaticMeshComponent>(Primitive))
            {
                ++ActualFlameCards;
                const FVector Local = VisualActor->GetActorTransform().InverseTransformPosition(Child->GetComponentLocation());
                bFlamesBehindNozzles &= Local.X < MeasuredLips[0].X - 10.f &&
                    FVector::DotProduct(Child->GetForwardVector(), VisualActor->GetActorForwardVector()) > .999;
            }
        }
        CheckForAirframe(TEXT("Rendered primitives use dedicated additive unlit exhaust materials without collision"),
            bAuthoredAdditiveLight && ActualPrimitiveCount == ExpectedNozzles * 3 && ActualPrimitiveCount == Exhaust->GetVisualPoolSize());
        CheckForAirframe(TEXT("Actual flame cards extend aft along the aircraft longitudinal axis"), bFlamesBehindNozzles && ActualFlameCards == ExpectedNozzles * 2);
        TArray<FVector> LocalNozzles;
        for (const FVector& Position : Nozzles) LocalNozzles.Add(VisualActor->GetActorTransform().InverseTransformPosition(Position));
        const FTransform Rolled(FRotator(24.f, 63.f, 71.f), TestOrigin + FVector(1300.f, -700.f, 1900.f));
        VisualActor->SetActorTransform(Rolled);
        const TArray<FVector> RolledNozzles = Exhaust->GetNozzleWorldLocations();
        double MaxAttachmentError = 0.;
        bool bRigidAttachment = RolledNozzles.Num() == LocalNozzles.Num();
        for (int32 I = 0; bRigidAttachment && I < LocalNozzles.Num(); ++I)
            MaxAttachmentError = FMath::Max(MaxAttachmentError, FVector::Distance(RolledNozzles[I], Rolled.TransformPosition(LocalNozzles[I])));
        for (int32 I = 0; I < Children.Num(); ++I)
            MaxAttachmentError = FMath::Max(MaxAttachmentError, FVector::Distance(Children[I]->GetComponentLocation(), Rolled.TransformPosition(LocalVisualPositions[I])));
        CheckForAirframe(TEXT("Exhaust nozzles follow aircraft translation pitch yaw and roll"), bRigidAttachment && MaxAttachmentError < .5);
        VisualActor->SetActorLocationAndRotation(TestOrigin, FRotator::ZeroRotator);
        const int32 OriginalComponents = VisualActor->GetComponents().Num();
        const int32 OriginalPool = Exhaust->GetVisualPoolSize();
        bool bBounded = OriginalPool == ExpectedNozzles * 3;
        for (int32 I = 0; I < 240; ++I)
        {
            Exhaust->UpdateExhaust(.02f, I % 3 == 0 ? 1.f : 0.f, I % 3 == 1 ? 1.f : 0.f);
            bBounded &= VisualActor->GetComponents().Num() == OriginalComponents && Exhaust->GetVisualPoolSize() == OriginalPool;
        }
        CheckForAirframe(TEXT("Repeated engine transitions reuse a bounded visual component pool"), bBounded);
        for (int32 I = 0; I < 90; ++I) Exhaust->UpdateExhaust(1.f / 60.f, 0.f, 0.f);
        Measurements[0].Length = Exhaust->GetFlameLengthCm();
        Measurements[0].Radius = Exhaust->GetCoreRadiusCm();
        Measurements[0].Strength = Exhaust->GetGlowStrength();
        Observations.Add(FString::Printf(TEXT("{\"case\":\"attachment_and_pool\",\"airframe\":\"%s\",\"nozzles\":%d,\"source_rim_error_cm\":%.6f,\"attachment_error_cm\":%.6f,\"actor_components\":%d,\"exhaust_pool\":%d,\"transition_updates\":240}"),
            *AirframeName(), Nozzles.Num(), MaxLipError, MaxAttachmentError, OriginalComponents, OriginalPool));
    }

    EReadback ReadViewport(TArray<FColor>& Pixels)
    {
        if (bReadbackReady)
        {
            bReadbackReady = false;
            if (!ReadbackError.IsEmpty()) return EReadback::Failed;
            Pixels = MoveTemp(PendingPixels);
            return EReadback::Ready;
        }
        if (!bReadbackRequested)
        {
            PendingPixels.Reset();
            ReadbackError.Empty();
            ReadbackRequestedAt = FPlatformTime::Seconds();
            bReadbackRequested = true;
        }
        else if (FPlatformTime::Seconds() - ReadbackRequestedAt > 5.)
        {
            ReadbackError = TEXT("The actual viewport did not service its numeric readback within five seconds.");
            return EReadback::Failed;
        }
        return EReadback::Pending;
    }

    void AfterViewportRendered(FViewport* Viewport)
    {
        if (!bReadbackRequested || bFinished || !GEngine || !GEngine->GameViewport || Viewport != GEngine->GameViewport->Viewport) return;
        bReadbackRequested = false;
        bReadbackReady = true;
        ViewSize = Viewport->GetSizeXY();
        // Execute while the actual scene back buffer is still valid. There is
        // deliberately no screenshot request, encoder or image file writer.
        if (ViewSize.X < 1280 || ViewSize.Y < 720 || !Viewport->ReadPixels(PendingPixels) || PendingPixels.Num() != ViewSize.X * ViewSize.Y)
        {
            ReadbackError = TEXT("The actual viewport readback failed or had an unexpected size.");
            return;
        }
        int32 NonBlack = 0, Different = 0;
        const FColor First = PendingPixels[0];
        for (const FColor& Pixel : PendingPixels)
        {
            if (FMath::Max3(Pixel.R, Pixel.G, Pixel.B) >= 8) ++NonBlack;
            if (FMath::Max3(FMath::Abs(int32(Pixel.R) - First.R), FMath::Abs(int32(Pixel.G) - First.G), FMath::Abs(int32(Pixel.B) - First.B)) >= 8) ++Different;
        }
        if (NonBlack < PendingPixels.Num() / 4 || Different < PendingPixels.Num() / 100)
            ReadbackError = TEXT("A blank or constant framebuffer cannot substantiate daylight visibility.");
    }

    bool BuildRegion()
    {
        // Fix the volume across all throttle settings; otherwise an enlarged
        // region itself could manufacture an apparent footprint difference.
        FVector2D Min(1.e9, 1.e9), Max(-1.e9, -1.e9);
        for (const FVector& Nozzle : Exhaust->GetNozzleWorldLocations())
            for (const float X : { -4000.f, 150.f })
                for (const float Y : { -650.f, 650.f })
                    for (const float Z : { -650.f, 650.f })
                    {
                        FVector2D Screen;
                        if (!PC->ProjectWorldLocationToScreen(Nozzle + FVector(X, Y, Z), Screen)) return false;
                        Min.X = FMath::Min(Min.X, Screen.X); Min.Y = FMath::Min(Min.Y, Screen.Y);
                        Max.X = FMath::Max(Max.X, Screen.X); Max.Y = FMath::Max(Max.Y, Screen.Y);
                    }
        ObservationRegion = FIntRect(FMath::FloorToInt(Min.X) - 5, FMath::FloorToInt(Min.Y) - 5,
            FMath::CeilToInt(Max.X) + 5, FMath::CeilToInt(Max.Y) + 5);
        return ObservationRegion.Min.X > 0 && ObservationRegion.Min.Y > 0 && ObservationRegion.Max.X < ViewSize.X &&
            ObservationRegion.Max.Y < ViewSize.Y && ObservationRegion.Width() > 40 && ObservationRegion.Height() > 20;
    }

    void CompareFrames()
    {
        const auto Delta = [this](int32 X, int32 Y)
        {
            const int32 Index = Y * ViewSize.X + X;
            const FColor& A = VisiblePixels[Index];
            const FColor& B = BaselinePixels[Index];
            return FMath::Max3(FMath::Abs(int32(A.R) - B.R), FMath::Abs(int32(A.G) - B.G), FMath::Abs(int32(A.B) - B.B));
        };
        FStateMeasurement& Value = Measurements[ThrottleState];
        int32 Peak = 0;
        double Sum = 0., ControlSum = 0.;
        for (int32 Y = ObservationRegion.Min.Y; Y < ObservationRegion.Max.Y; ++Y)
            for (int32 X = ObservationRegion.Min.X; X < ObservationRegion.Max.X; ++X)
            {
                const int32 Difference = Delta(X, Y);
                if (Difference >= 16) ++Value.ChangedPixels;
                Peak = FMath::Max(Peak, Difference);
                Sum += Difference;
                const int32 Index = Y * ViewSize.X + X;
                const FColor& A = VisiblePixels[Index];
                const FColor& B = BaselinePixels[Index];
                const double LightIncrease = .2126 * (int32(A.R) - B.R) + .7152 * (int32(A.G) - B.G) + .0722 * (int32(A.B) - B.B);
                // Count added light rather than dark silhouette changes: a
                // fallback grey material cannot masquerade as a bright jet.
                if (LightIncrease > 8.) Value.LightEnergy += LightIncrease;
            }
        for (int32 Y = 30; Y < 110; ++Y)
            for (int32 X = 30; X < 110; ++X) ControlSum += Delta(X, Y);
        const double Mean = Sum / FMath::Max(1, ObservationRegion.Area());
        const double ControlMean = ControlSum / 6400.;
        CheckForAirframe(*FString::Printf(TEXT("%s engine exhaust visibly adds light in the daylight viewport"), StateName()),
            Value.ChangedPixels >= 35 && Peak >= 32 && Mean > ControlMean + .08 && Value.LightEnergy >= 1200.);
        Observations.Add(FString::Printf(TEXT("{\"case\":\"isolated_daylight_exhaust\",\"airframe\":\"%s\",\"power\":\"%s\",\"comparison\":\"same airframe and camera with exhaust hidden then visible\",\"roi\":[%d,%d,%d,%d],\"pixels_changed_at_least_16\":%d,\"peak_channel_difference\":%d,\"mean_channel_difference\":%.6f,\"control_mean_difference\":%.6f,\"added_luminance_sum\":%.3f,\"flame_length_cm\":%.3f,\"core_radius_cm\":%.3f,\"glow_strength\":%.3f,\"flight_speed_mps\":%.3f}"),
            *AirframeName(), StateName(), ObservationRegion.Min.X, ObservationRegion.Min.Y, ObservationRegion.Max.X, ObservationRegion.Max.Y,
            Value.ChangedPixels, Peak, Mean, ControlMean, Value.LightEnergy, Value.Length, Value.Radius, Value.Strength, Value.Speed));
    }

    void FinishAirframe()
    {
        if (AirframeIndex < 3)
        {
            const FStateMeasurement& Cruise = Measurements[0];
            const FStateMeasurement& Boost = Measurements[1];
            const FStateMeasurement& Brake = Measurements[2];
            CheckForAirframe(TEXT("Acceleration enlarges and brightens exhaust while braking reduces it without extinguishing"),
                Boost.Length > Cruise.Length * 1.3f && Boost.Radius > Cruise.Radius * 1.1f && Boost.Strength > Cruise.Strength * 1.25f &&
                Brake.Length < Cruise.Length * .85f && Brake.Radius < Cruise.Radius * .95f && Brake.Strength < Cruise.Strength * .85f && Brake.Strength > 0.f);
            CheckForAirframe(TEXT("Rendered acceleration footprint and added light exceed cruise and braking"),
                Boost.ChangedPixels > Cruise.ChangedPixels * 1.2 && Cruise.ChangedPixels > Brake.ChangedPixels * 1.15 &&
                Boost.LightEnergy > Cruise.LightEnergy * 1.2 && Cruise.LightEnergy > Brake.LightEnergy * 1.15);
            Mode->Player->Park();
            CheckForAirframe(TEXT("Parking hides every engine exhaust primitive"), !Exhaust->IsExhaustVisible() && Exhaust->GetVisiblePrimitiveCount() == 0);
            Mode->Player->BeginFlight();
            Mode->Player->Tick(.02f);
            CheckForAirframe(TEXT("A fresh flight restores engine exhaust"), Exhaust->IsExhaustVisible() && Exhaust->GetVisiblePrimitiveCount() > 0);
            Mode->Player->ApplyCombatDamage(Mode->Player->Health + 1.f);
            CheckForAirframe(TEXT("Player destruction immediately extinguishes engine exhaust"), !Exhaust->IsExhaustVisible() && Exhaust->GetVisiblePrimitiveCount() == 0);
            Mode->ReturnToMenu();
            CheckForAirframe(TEXT("Return to menu leaves parked aircraft exhaust hidden"), !Exhaust->IsExhaustVisible() && Exhaust->GetVisiblePrimitiveCount() == 0);
            ++AirframeIndex;
            BeginAirframe();
        }
        else
        {
            ADSEnemyAircraft* Enemy = Cast<ADSEnemyAircraft>(VisualActor.Get());
            if (!Enemy) { Finish(TEXT("The observed hostile actor changed type.")); return; }
            TArray<UPrimitiveComponent*> Primitives;
            Enemy->GetComponents(Primitives);
            DestroyedPrimitives.Reset();
            for (UPrimitiveComponent* Primitive : Primitives) DestroyedPrimitives.Add(Primitive);
            Enemy->ApplyCombatDamage(Enemy->Health + 1.f, true);
            CheckForAirframe(TEXT("Hostile destruction immediately extinguishes engine exhaust"), !Exhaust->IsExhaustVisible() && Exhaust->GetVisiblePrimitiveCount() == 0);
            Enemy->Destroy();
            Stage = EStage::Cleanup;
            ResetStageClock();
        }
    }

    bool Finish(const TCHAR* Error = nullptr)
    {
        bFinished = true;
        InjectThrottle = InjectBrake = 0.f;
        RemoveDelegates();
        if (Error) Test->AddError(Error);
        if (bStateCaptured && Mode.IsValid())
        {
            if (Input.IsValid() && Mode->Player)
            {
                Input->InjectInputForAction(Mode->Player->ThrottleAction, FInputActionValue(0.f), {}, {});
                Input->InjectInputForAction(Mode->Player->BrakeAction, FInputActionValue(0.f), {}, {});
                Mode->Player->ClearFlightInput();
            }
            ClearCombatActors();
            Mode->ReturnToMenu();
            Mode->SelectAircraft(OriginalSelection);
            Mode->SetActorTickEnabled(bOriginalModeTick);
            if (Mode->Player)
            {
                Mode->Player->SetActorTickEnabled(bOriginalPlayerTick);
                Mode->Player->SetActorHiddenInGame(bOriginalPlayerHidden);
            }
        }
        if (bStateCaptured && PC.IsValid())
        {
            PC->bAutoManageActiveCameraTarget = bOriginalAutoCamera;
            if (OriginalViewTarget.IsValid()) PC->SetViewTarget(OriginalViewTarget.Get());
            if (PC->GetHUD()) PC->GetHUD()->bShowHUD = bOriginalHUD;
        }
        if (Camera.IsValid()) Camera->Destroy();
        if (!OutputDir.IsEmpty())
        {
            const FString Report = FString::Printf(TEXT("{\"schema\":\"aircraft-exhaust-acceptance/1\",\"status\":\"%s\",\"fixture\":\"real DesertDefense world; normal aircraft and Enhanced Input actions; no AutoPlay\",\"visual_evidence\":\"numeric isolated daylight viewport contrast only; no human image review\",\"media_files_written\":0,\"viewport\":[%d,%d],\"checks\":[%s],\"observations\":[%s]}\n"),
                Test->HasAnyErrors() ? TEXT("failed") : TEXT("passed"), ViewSize.X, ViewSize.Y, *FString::Join(Checks, TEXT(",")), *FString::Join(Observations, TEXT(",")));
            Test->TestTrue(TEXT("Numeric aircraft exhaust acceptance report saved"), FFileHelper::SaveStringToFile(Report, *(OutputDir / TEXT("report.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
            Test->AddInfo(TEXT("Aircraft exhaust acceptance JSON: ") + OutputDir);
        }
        return true;
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDSAircraftExhaustAcceptanceTest, "DuneSentinel.Rendering.AircraftExhaustAcceptance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FDSAircraftExhaustAcceptanceTest::RunTest(const FString& Parameters)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("DSAircraftExhaustValidation")) ||
        FParse::Param(FCommandLine::Get(), TEXT("AutoPlay")) || FParse::Param(FCommandLine::Get(), TEXT("CaptureFrames")))
    {
        AddError(TEXT("Use dedicated -game -DSAircraftExhaustValidation without AutoPlay or CaptureFrames on DesertDefense."));
        return false;
    }
    ADD_LATENT_AUTOMATION_COMMAND(FDSAircraftExhaustAcceptanceCommand(this));
    return true;
}
#endif
