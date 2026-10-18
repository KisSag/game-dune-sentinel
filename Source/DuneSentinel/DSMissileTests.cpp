#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "DSFlightPawn.h"
#include "DSEnemyAircraft.h"
#include "DSGameMode.h"
#include "DSMissileFXComponent.h"
#include "DSProjectile.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "InputAction.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

namespace
{
// Dedicated development fixture: ordinary input, firing, projectile movement and
// collision in the real map. The only persisted evidence is the numeric report.
class FDSMissileAcceptanceCommand final : public IAutomationLatentCommand
{
public:
    explicit FDSMissileAcceptanceCommand(FAutomationTestBase* InTest) : Test(InTest) {}
    virtual ~FDSMissileAcceptanceCommand() override { RemoveDelegate(); }

    virtual bool Update() override
    {
        if (bFinished) return true;
        if (GetCurrentRunTime() > 60.0) return Finish(TEXT("Missile acceptance exceeded 60 seconds."));
        if (!bInitialized) return Initialize();
        if (!World.IsValid() || !Mode.IsValid() || !PC.IsValid() || !Mode->Player)
            return Finish(TEXT("The real gameplay world became unavailable."));
        const float Now = World->GetTimeSeconds();
        if (Stage == EStage::InputWarmup)
        {
            if (Now - StageTime < .5f) return false;
            bInjectMissile = true;
            Stage = EStage::InputLaunch;
            StageTime = Now;
        }
        else if (Stage == EStage::InputLaunch)
        {
            ADSProjectile* Shot = FindPlayerMissile();
            if (!Shot) return Now - StageTime > 3.f ? Finish(TEXT("Actual missile input failed to launch without a target.")) : false;
            bInjectMissile = false;
            Check(TEXT("Enhanced Input launches without a selected target"), Shot->IsMissile() && !Shot->HasGuidanceTarget());
            Check(TEXT("Input launch consumes exactly one missile"), Mode->Player->MissileAmmo == InitialAmmo - 1);
            Shot->Destroy();
            RunMechanics();
            BeginVisualCase(false);
        }
        else if (Stage == EStage::BaselineFrame)
        {
            if (Now - StageTime < .65f || GFrameCounter < StageFrame + 5) return false;
            if (!EffectsMatchVisibility(false, false)) return Finish(TEXT("Both effect groups must be hidden in the baseline."));
            const EReadback Result = ReadViewport(BaselinePixels);
            if (Result == EReadback::Pending) return false;
            if (Result == EReadback::Failed) return Finish(*ReadbackError);
            if (!BuildRegions()) return Finish(TEXT("The flame and contrail were outside the observation view."));
            UDSMissileFXComponent* FX = VisualMissile->GetMissileFX();
            // The other effect remains hidden for this entire comparison, so
            // smoke cannot count as flame evidence or flame as cloud evidence.
            FX->SetExhaustVisible(VisualEffectIndex == 0);
            FX->SetContrailVisible(VisualEffectIndex == 1);
            Stage = EStage::EffectFrame;
            StageTime = Now;
            StageFrame = GFrameCounter;
        }
        else if (Stage == EStage::EffectFrame)
        {
            if (Now - StageTime < .65f || GFrameCounter < StageFrame + 5) return false;
            if (!EffectsMatchVisibility(VisualEffectIndex == 0, VisualEffectIndex == 1))
                return Finish(TEXT("An isolated effect comparison contained the other effect group."));
            const EReadback Result = ReadViewport(VisiblePixels);
            if (Result == EReadback::Pending) return false;
            if (Result == EReadback::Failed) return Finish(*ReadbackError);
            if (BaselinePixels.Num() != VisiblePixels.Num()) return Finish(TEXT("Viewport dimensions changed during the numeric contrast comparison."));
            CompareRegion(BaselinePixels, VisualEffectIndex);
            VisiblePixels.Reset();
            BaselinePixels.Reset();
            if (VisualEffectIndex == 0)
            {
                // Acquire a fresh settled baseline after removing flame; the
                // contrail-only frame never inherits the flame comparison.
                VisualMissile->GetMissileFX()->SetExhaustVisible(false);
                VisualMissile->GetMissileFX()->SetContrailVisible(false);
                VisualEffectIndex = 1;
                Stage = EStage::BaselineFrame;
                StageTime = Now;
                StageFrame = GFrameCounter;
                return false;
            }
            TArray<UPrimitiveComponent*> Primitives;
            VisualMissile->GetComponents(Primitives);
            CleanupComponents.Reset();
            for (UPrimitiveComponent* Primitive : Primitives) CleanupComponents.Add(Primitive);
            VisualMissile->Destroy();
            Stage = EStage::Cleanup;
            StageFrame = GFrameCounter;
        }
        else if (Stage == EStage::Cleanup)
        {
            if (GFrameCounter < StageFrame + 3) return false;
            bool bClean = true;
            for (const TWeakObjectPtr<UPrimitiveComponent>& Primitive : CleanupComponents)
                bClean &= !Primitive.IsValid() || !Primitive->IsRegistered();
            Check(bHostileVisual ? TEXT("Hostile missile destruction unregisters every visual component") :
                TEXT("Friendly missile destruction unregisters every visual component"), bClean);
            if (!bHostileVisual) BeginVisualCase(true);
            else return Finish();
        }
        return false;
    }

private:
    enum class EStage { InputWarmup, InputLaunch, BaselineFrame, EffectFrame, Cleanup };
    enum class EReadback { Pending, Ready, Failed };
    FAutomationTestBase* Test;
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<ADSGameMode> Mode;
    TWeakObjectPtr<APlayerController> PC;
    TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> Input;
    TWeakObjectPtr<ACameraActor> Camera;
    TWeakObjectPtr<AActor> OriginalViewTarget;
    TWeakObjectPtr<ADSProjectile> VisualMissile;
    TArray<TWeakObjectPtr<UPrimitiveComponent>> CleanupComponents;
    TArray<FString> Checks, Observations;
    TArray<FColor> VisiblePixels, BaselinePixels, PendingPixels;
    FIntRect FlameRegion, CloudRegion;
    FIntPoint ViewSize = FIntPoint::ZeroValue;
    FString OutputDir, ReadbackError;
    FDelegateHandle PreTickHandle, ViewportRenderedHandle;
    EStage Stage = EStage::InputWarmup;
    float StageTime = 0.f;
    uint64 StageFrame = 0;
    double ReadbackRequestedAt = 0.;
    int32 InitialAmmo = 0, OriginalSelection = 0, VisualEffectIndex = 0;
    bool bInitialized = false, bStateCaptured = false, bFinished = false, bInjectMissile = false, bHostileVisual = false;
    bool bReadbackRequested = false, bReadbackReady = false;
    bool bOriginalModeTick = true, bOriginalPlayerTick = true, bOriginalAutoCamera = true;
    const FVector TestOrigin = FVector(-400000.f, -400000.f, 250000.f);

    void Check(const TCHAR* Name, bool bPassed)
    {
        Test->TestTrue(Name, bPassed);
        Checks.Add(FString::Printf(TEXT("{\"name\":\"%s\",\"passed\":%s}"), Name, bPassed ? TEXT("true") : TEXT("false")));
    }

    bool Initialize()
    {
        UWorld* GameWorld = nullptr;
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
            if (Context.WorldType == EWorldType::Game && Context.World() && Context.World()->HasBegunPlay())
                GameWorld = Context.World();
        if (!GameWorld || !GEngine->GameViewport || !GEngine->GameViewport->Viewport) return false;
        World = GameWorld;
        Mode = GameWorld->GetAuthGameMode<ADSGameMode>();
        PC = UGameplayStatics::GetPlayerController(GameWorld, 0);
        if (!Mode.IsValid() || !Mode->Player || !PC.IsValid() || !PC->GetLocalPlayer())
            return Finish(TEXT("DesertDefense must provide its real mode, player and local controller."));
        Input = PC->GetLocalPlayer()->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
        if (!Input.IsValid()) return Finish(TEXT("Enhanced Input is unavailable."));
        OriginalSelection = Mode->SelectedAircraft;
        bOriginalModeTick = Mode->IsActorTickEnabled();
        bOriginalPlayerTick = Mode->Player->IsActorTickEnabled();
        bOriginalAutoCamera = PC->bAutoManageActiveCameraTarget;
        OriginalViewTarget = PC->GetViewTarget();
        // Track restoration before the first gameplay mutation, including
        // failure paths before the observation camera has been created.
        bStateCaptured = true;
        OutputDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Validation/Missiles") /
            (FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")) + TEXT("-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8)));
        IFileManager::Get().MakeDirectory(*OutputDir, true);
        Mode->ReturnToMenu();
        Mode->StartSortie();
        for (TActorIterator<ADSEnemyAircraft> It(GameWorld); It; ++It) It->Destroy();
        Mode->Enemies.Reset();
        Mode->RadioLine.Empty();
        Mode->SetActorTickEnabled(false);
        Mode->Player->SetActorTickEnabled(false);
        PreparePlayer();
        InitialAmmo = Mode->Player->MissileAmmo;
        Camera = GameWorld->SpawnActor<ACameraActor>();
        if (!Camera.IsValid()) return Finish(TEXT("Could not create the observation camera."));
        Camera->GetCameraComponent()->SetFieldOfView(60.f);
        PC->bAutoManageActiveCameraTarget = false;
        StageTime = GameWorld->GetTimeSeconds();
        PreTickHandle = FWorldDelegates::OnWorldPreActorTick.AddRaw(this, &FDSMissileAcceptanceCommand::BeforeTick);
        ViewportRenderedHandle = UGameViewportClient::OnViewportRendered().AddRaw(this, &FDSMissileAcceptanceCommand::AfterViewportRendered);
        bInitialized = true;
        return false;
    }

    void BeforeTick(UWorld* TickWorld, ELevelTick, float)
    {
        if (TickWorld == World.Get() && Input.IsValid() && Mode.IsValid() && Mode->Player && Mode->Player->MissileAction)
            Input->InjectInputForAction(Mode->Player->MissileAction, FInputActionValue(bInjectMissile), {}, {});
    }

    void RemoveDelegate()
    {
        if (PreTickHandle.IsValid()) FWorldDelegates::OnWorldPreActorTick.Remove(PreTickHandle);
        if (ViewportRenderedHandle.IsValid()) UGameViewportClient::OnViewportRendered().Remove(ViewportRenderedHandle);
        PreTickHandle.Reset();
        ViewportRenderedHandle.Reset();
        bReadbackRequested = false;
    }

    void PreparePlayer()
    {
        for (TActorIterator<ADSProjectile> It(World.Get()); It; ++It) It->Destroy();
        Mode->Player->Park();
        Mode->Player->BeginFlight();
        Mode->Player->SetActorLocationAndRotation(TestOrigin, FRotator::ZeroRotator);
        Mode->Player->Target.Reset();
        Mode->Player->LockProgress = 0.f;
        Mode->Player->MissileAmmo = 8;
    }

    ADSProjectile* FindPlayerMissile() const
    {
        for (TActorIterator<ADSProjectile> It(World.Get()); It; ++It)
            if (It->IsMissile() && It->GetOwner() == Mode->Player.Get()) return *It;
        return nullptr;
    }

    ADSEnemyAircraft* MakeTarget(const FVector& Position)
    {
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        ADSEnemyAircraft* Enemy = World->SpawnActor<ADSEnemyAircraft>(Position, FRotator::ZeroRotator, Params);
        if (Enemy)
        {
            Enemy->Initialize(false, Mode->AirportLocation);
            Enemy->Speed = 0.f;
            Enemy->SetActorTickEnabled(false);
            Mode->Enemies.Add(Enemy);
        }
        return Enemy;
    }

    ADSProjectile* Launch(ADSEnemyAircraft* Target, float Lock)
    {
        PreparePlayer();
        Mode->Player->Target = Target;
        Mode->Player->LockProgress = Lock;
        Mode->Player->FireMissile();
        ADSProjectile* Shot = FindPlayerMissile();
        if (Shot) Shot->SetActorTickEnabled(false);
        return Shot;
    }

    ADSProjectile* MakeProjectile(const FVector& Position, AActor* Target, bool bFriendly, float Speed)
    {
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        ADSProjectile* Shot = World->SpawnActor<ADSProjectile>(Position, FRotator::ZeroRotator, Params);
        if (Shot)
        {
            Shot->Initialize(bFriendly ? static_cast<AActor*>(Mode->Player.Get()) : nullptr, Target, bFriendly, true, 70.f, Speed);
            Shot->SetActorTickEnabled(false);
        }
        return Shot;
    }

    void RunMechanics()
    {
        ADSEnemyAircraft* Target = MakeTarget(TestOrigin + FVector(150000.f, 70000.f, 10000.f));
        if (!Target) { Test->AddError(TEXT("Could not create the real missile target.")); return; }
        ADSProjectile* Shot = Launch(Target, .5f);
        Check(TEXT("Partial lock launches an unguided missile"), Shot && !Shot->HasGuidanceTarget());
        Check(TEXT("Successful launch consumes one round"), Mode->Player->MissileAmmo == 7);
        Mode->Player->FireMissile();
        Check(TEXT("Immediate repeat is blocked by the normal cooldown"), Mode->Player->MissileAmmo == 7);
        Mode->Player->Tick(1.19f);
        Mode->Player->FireMissile();
        Check(TEXT("Cooldown still blocks firing before 1.2 seconds"), Mode->Player->MissileAmmo == 7);
        Mode->Player->Tick(.02f);
        Mode->Player->FireMissile();
        Check(TEXT("Cooldown permits the next shot after 1.2 seconds"), Mode->Player->MissileAmmo == 6);

        Target->bDying = true;
        Shot = Launch(Target, 1.f);
        Check(TEXT("Dying selected target does not prevent unguided firing"), Shot && !Shot->HasGuidanceTarget());
        Target->bDying = false;
        Target->Health = 0.f;
        Shot = Launch(Target, 1.f);
        Check(TEXT("Zero-health selected target cannot guide a missile"), Shot && !Shot->HasGuidanceTarget());
        Target->Health = 65.f;

        Shot = Launch(nullptr, 0.f);
        if (Shot)
        {
            const FVector Origin = Shot->GetActorLocation(), Direction = Shot->GetTravelDirection();
            Mode->Player->Target = Target;
            Mode->Player->LockProgress = 1.f;
            double MaxLateral = 0.;
            int32 Steps = 0;
            for (; Steps < 100 && !Shot->IsActorBeingDestroyed(); ++Steps)
            {
                Shot->Tick(.37f);
                const FVector Offset = Shot->GetActorLocation() - Origin;
                MaxLateral = FMath::Max(MaxLateral, (Offset - Direction * FVector::DotProduct(Offset, Direction)).Size());
            }
            Check(TEXT("Later target selection never guides an already unguided missile"), !Shot->HasGuidanceTarget());
            Check(TEXT("Unguided missile remains on its initial straight flight line"), MaxLateral < 1.f);
            Check(TEXT("Unguided missile expires exactly at the 8 km maximum path"), Shot->IsActorBeingDestroyed() &&
                FMath::IsNearlyEqual(Shot->GetDistanceTravelledCm(), 800000.f, 2.f) &&
                FMath::IsNearlyEqual(Shot->GetMaxTravelDistanceCm(), 800000.f, .1f) &&
                FMath::IsNearlyEqual(static_cast<float>(FVector::Distance(Origin, Shot->GetActorLocation())), 800000.f, 2.f));
            Observations.Add(FString::Printf(TEXT("{\"case\":\"unguided_range\",\"steps\":%d,\"distance_cm\":%.3f,\"max_lateral_error_cm\":%.6f}"),
                Steps, Shot->GetDistanceTravelledCm(), MaxLateral));
        }
        else Check(TEXT("No-lock direct firing produces a projectile"), false);

        Shot = Launch(Target, 1.f);
        Check(TEXT("Completed valid lock supplies guidance at launch"), Shot && Shot->HasGuidanceTarget());
        if (Shot)
        {
            const FVector InitialDirection = Shot->GetTravelDirection();
            Shot->Tick(.1f);
            const TArray<FVector> ExistingCloud = Shot->GetMissileFX()->GetContrailWorldLocations();
            Shot->Tick(.1f);
            Check(TEXT("Guided missile turns toward the locked off-axis aircraft"), FVector::DotProduct(InitialDirection, Shot->GetTravelDirection()) < .999f);
            const TArray<FVector> LaterCloud = Shot->GetMissileFX()->GetContrailWorldLocations();
            int32 Retained = 0;
            for (const FVector& Before : ExistingCloud)
                for (const FVector& After : LaterCloud) if (Before.Equals(After, .1f)) { ++Retained; break; }
            Check(TEXT("Contrail remains in world space while its missile turns"), ExistingCloud.Num() > 0 && Retained == ExistingCloud.Num());
            Target->Destroy();
            Shot->Tick(.1f);
            const FVector LostDirection = Shot->GetTravelDirection();
            const FVector LostOrigin = Shot->GetActorLocation();
            Shot->Tick(.2f);
            const FVector AfterLoss = Shot->GetActorLocation() - LostOrigin;
            Check(TEXT("Destroyed target releases guidance and retains the last heading"), !Shot->HasGuidanceTarget() &&
                FVector::DotProduct(LostDirection, Shot->GetTravelDirection()) > .99999f &&
                FVector::CrossProduct(AfterLoss, LostDirection).Size() < 1.f);
        }

        Target = MakeTarget(TestOrigin + FVector(1500000.f, 700000.f, 10000.f));
        Shot = Launch(Target, 1.f);
        if (Shot && Target)
        {
            const FVector Origin = Shot->GetActorLocation();
            FVector Previous = Origin;
            double PathLength = 0.;
            for (int32 I = 0; I < 100 && !Shot->IsActorBeingDestroyed(); ++I)
            {
                Shot->Tick(.37f);
                PathLength += FVector::Distance(Previous, Shot->GetActorLocation());
                Previous = Shot->GetActorLocation();
            }
            Check(TEXT("Curved guided flight consumes actual path distance and clamps its final movement"), Shot->IsActorBeingDestroyed() &&
                FMath::Abs(PathLength - 800000.) < 3. && FMath::Abs(PathLength - Shot->GetDistanceTravelledCm()) < 3.);
            Check(TEXT("Guided range is not measured as displacement from the launcher"), FVector::Distance(Origin, Shot->GetActorLocation()) < PathLength - 100.f);
            Observations.Add(FString::Printf(TEXT("{\"case\":\"guided_range\",\"path_cm\":%.3f,\"displacement_cm\":%.3f}"),
                PathLength, FVector::Distance(Origin, Shot->GetActorLocation())));
        }
        else Check(TEXT("Guided range fixture launched successfully"), false);

        PreparePlayer();
        Mode->Player->MissileAmmo = 0;
        Mode->Player->FireMissile();
        Check(TEXT("Empty ammunition cannot spawn a missile"), !FindPlayerMissile() && Mode->Player->MissileAmmo == 0);
        Mode->Player->MissileAmmo = 8;
        Mode->Player->Park();
        Mode->Player->FireMissile();
        Check(TEXT("Parked aircraft cannot fire missiles"), !FindPlayerMissile() && Mode->Player->MissileAmmo == 8);

        PreparePlayer();
        Target = MakeTarget(TestOrigin + FVector(16000.f, -350.f, -100.f));
        Mode->Player->FireMissile();
        Shot = FindPlayerMissile();
        if (Shot && Target)
        {
            Shot->SetActorTickEnabled(false);
            Shot->Tick(.4f);
            Check(TEXT("Unguided missile sweep damages an aircraft before maximum range"), Shot->IsActorBeingDestroyed() &&
                (Target->IsActorBeingDestroyed() || Target->bDying || Target->Health < 65.f) && Shot->GetDistanceTravelledCm() < 30000.f);
        }
        else Check(TEXT("Collision acceptance fixture launched successfully"), false);

        for (TActorIterator<ADSEnemyAircraft> It(World.Get()); It; ++It) It->Destroy();
        Mode->Enemies.Reset();
        PreparePlayer();
        // A slow missile must not silently retain the former ten-second timeout.
        Shot = MakeProjectile(TestOrigin + FVector(0.f, 80000.f, 20000.f), nullptr, true, 100.f);
        if (Shot)
        {
            const int32 InitialComponents = Shot->GetComponents().Num();
            int32 PeakComponents = InitialComponents, PeakCloud = 0;
            for (int32 I = 0; I < 45 && !Shot->IsActorBeingDestroyed(); ++I)
            {
                Shot->Tick(.25f);
                PeakComponents = FMath::Max(PeakComponents, Shot->GetComponents().Num());
                PeakCloud = FMath::Max(PeakCloud, Shot->GetMissileFX()->GetActiveContrailCount());
            }
            Check(TEXT("Missile lifetime follows travel distance instead of the former ten seconds"), !Shot->IsActorBeingDestroyed() && Shot->GetDistanceTravelledCm() > 110000.f);
            Check(TEXT("Repeated contrail emission keeps a fixed component pool"), PeakComponents == InitialComponents && PeakCloud > 0 && PeakCloud <= UDSMissileFXComponent::GetContrailPoolSize());
            UDSMissileFXComponent* FX = Shot->GetMissileFX();
            const FVector Here = Shot->GetActorLocation();
            FX->UpdateFlight(Here, Here, Shot->GetTravelDirection(), UDSMissileFXComponent::GetContrailLifetimeSeconds() + .1f);
            Check(TEXT("Old contrail samples expire when no new flight segment is emitted"), FX->GetActiveContrailCount() == 0);
            FX->ResetEffects();
            Check(TEXT("Effect reset removes flame and every contrail sample"), !FX->IsExhaustVisible() && FX->GetActiveContrailCount() == 0);
            Observations.Add(FString::Printf(TEXT("{\"case\":\"pool_lifetime\",\"components_initial\":%d,\"components_peak\":%d,\"cloud_peak\":%d}"), InitialComponents, PeakComponents, PeakCloud));
            Shot->Destroy();
        }
        else Check(TEXT("Slow-missile lifetime fixture launched successfully"), false);

        Shot = MakeProjectile(TestOrigin + FVector(0.f, 110000.f, 30000.f), nullptr, true, 850.f);
        if (Shot)
        {
            const int32 InitialComponents = Shot->GetComponents().Num();
            int32 PeakCloud = 0;
            bool bStableComponents = true;
            for (int32 I = 0; I < 70; ++I)
            {
                Shot->Tick(.05f);
                PeakCloud = FMath::Max(PeakCloud, Shot->GetMissileFX()->GetActiveContrailCount());
                bStableComponents &= Shot->GetComponents().Num() == InitialComponents;
            }
            Check(TEXT("Full-speed contrail recycles its bounded pool after more than one pool of samples"),
                bStableComponents && PeakCloud >= 50 && PeakCloud <= UDSMissileFXComponent::GetContrailPoolSize() &&
                Shot->GetDistanceTravelledCm() / UDSMissileFXComponent::GetContrailSampleSpacingCm() > UDSMissileFXComponent::GetContrailPoolSize());
            Observations.Add(FString::Printf(TEXT("{\"case\":\"full_speed_pool\",\"components\":%d,\"cloud_peak\":%d,\"distance_cm\":%.3f}"),
                InitialComponents, PeakCloud, Shot->GetDistanceTravelledCm()));
            Shot->Destroy();
        }
        else Check(TEXT("Full-speed pool fixture launched successfully"), false);
    }

    void BeginVisualCase(bool bHostile)
    {
        bHostileVisual = bHostile;
        PreparePlayer();
        VisualMissile = MakeProjectile(TestOrigin + FVector(0.f, 150000.f, 50000.f), nullptr, !bHostile, 850.f);
        if (!VisualMissile.IsValid()) { Finish(TEXT("Could not spawn the rendered missile.")); return; }
        for (int32 I = 0; I < 5; ++I) VisualMissile->Tick(.05f);
        UDSMissileFXComponent* FX = VisualMissile->GetMissileFX();
        Check(bHostile ? TEXT("Hostile missile renders a complete exhaust and contrail") : TEXT("Friendly missile renders a complete exhaust and contrail"),
            FX && FX->HasReadyMaterials() && FX->GetExhaustCoreStrength() > 0.f && FX->IsExhaustVisible() &&
            FX->GetVisibleExhaustPlaneCount() == 3 && FX->GetExhaustLengthCm() >= 1300.f && FX->GetActiveContrailCount() >= 6);
        const FVector Center = VisualMissile->GetActorLocation() - FVector(9000.f, 0.f, 0.f);
        const FVector Position = Center + FVector(0.f, -34000.f, 8000.f);
        Camera->SetActorLocationAndRotation(Position, (Center - Position).Rotation());
        PC->SetViewTarget(Camera.Get());
        if (PC->PlayerCameraManager) PC->PlayerCameraManager->UpdateCamera(0.f);
        FX->SetExhaustVisible(false);
        FX->SetContrailVisible(false);
        VisualEffectIndex = 0;
        Stage = EStage::BaselineFrame;
        StageTime = World->GetTimeSeconds();
        StageFrame = GFrameCounter;
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
            ReadbackError = TEXT("The actual viewport did not render the pending readback within five seconds.");
            return EReadback::Failed;
        }
        return EReadback::Pending;
    }

    void AfterViewportRendered(FViewport* Viewport)
    {
        if (!bReadbackRequested || bFinished || !GEngine || !GEngine->GameViewport ||
            Viewport != GEngine->GameViewport->Viewport) return;
        bReadbackRequested = false;
        bReadbackReady = true;
        ViewSize = Viewport->GetSizeXY();
        // This delegate runs after the canvas flush and before the scene
        // viewport releases its render-thread back buffer at end of frame.
        // ReadPixels queues and flushes the read while that texture is valid.
        // The game-thread texture may be null for a normal Slate back buffer.
        if (ViewSize.X < 1280 || ViewSize.Y < 720 || !Viewport->ReadPixels(PendingPixels) ||
            PendingPixels.Num() != ViewSize.X * ViewSize.Y)
        {
            ReadbackError = TEXT("Actual viewport readback failed or returned an unexpected image size.");
            return;
        }
        int32 NonBlackPixels = 0, DifferentPixels = 0;
        const FColor First = PendingPixels[0];
        for (const FColor& Pixel : PendingPixels)
        {
            if (FMath::Max3(Pixel.R, Pixel.G, Pixel.B) >= 8) ++NonBlackPixels;
            if (FMath::Max3(FMath::Abs(int32(Pixel.R) - First.R), FMath::Abs(int32(Pixel.G) - First.G),
                FMath::Abs(int32(Pixel.B) - First.B)) >= 8) ++DifferentPixels;
        }
        const bool bHasSceneContent = NonBlackPixels > PendingPixels.Num() / 4 && DifferentPixels > PendingPixels.Num() / 100;
        Observations.Add(FString::Printf(TEXT("{\"case\":\"viewport_readback\",\"phase\":\"%s\",\"effect\":\"%s\",\"side\":\"%s\",\"render_frame\":%llu,\"nonblack_pixels\":%d,\"pixels_different_from_first\":%d,\"valid_scene_content\":%s}"),
            Stage == EStage::BaselineFrame ? TEXT("baseline") : TEXT("effect"), VisualEffectIndex == 0 ? TEXT("flame") : TEXT("contrail"),
            bHostileVisual ? TEXT("hostile") : TEXT("friendly"), static_cast<unsigned long long>(GFrameCounter),
            NonBlackPixels, DifferentPixels, bHasSceneContent ? TEXT("true") : TEXT("false")));
        if (!bHasSceneContent) ReadbackError = TEXT("The framebuffer was blank or constant; it cannot substantiate visual acceptance.");
    }

    bool EffectsMatchVisibility(bool bExhaust, bool bCloud) const
    {
        TArray<UPrimitiveComponent*> Primitives;
        VisualMissile->GetComponents(Primitives);
        int32 ExhaustVisible = 0, CloudVisible = 0;
        for (const UPrimitiveComponent* Primitive : Primitives)
        {
            if (!Primitive->IsVisible() || Primitive->bHiddenInGame) continue;
            if (Primitive->GetName().StartsWith(TEXT("MissileExhaust"))) ++ExhaustVisible;
            if (Primitive->GetName().StartsWith(TEXT("MissileContrail"))) ++CloudVisible;
        }
        return (bExhaust ? ExhaustVisible == 4 : ExhaustVisible == 0) &&
            (bCloud ? CloudVisible >= 6 : CloudVisible == 0);
    }

    bool RegionAround(const FVector& Center, float RadiusCm, FIntRect& Out)
    {
        FVector2D CenterPx, EdgePx;
        if (!PC->ProjectWorldLocationToScreen(Center, CenterPx) ||
            !PC->ProjectWorldLocationToScreen(Center + Camera->GetActorRightVector() * RadiusCm, EdgePx)) return false;
        const int32 Radius = FMath::CeilToInt(FVector2D::Distance(CenterPx, EdgePx)) + 5;
        Out = FIntRect(FMath::Clamp(FMath::FloorToInt(CenterPx.X) - Radius, 0, ViewSize.X),
            FMath::Clamp(FMath::FloorToInt(CenterPx.Y) - Radius, 0, ViewSize.Y),
            FMath::Clamp(FMath::CeilToInt(CenterPx.X) + Radius, 0, ViewSize.X),
            FMath::Clamp(FMath::CeilToInt(CenterPx.Y) + Radius, 0, ViewSize.Y));
        return Out.Width() > 5 && Out.Height() > 5;
    }

    bool BuildRegions()
    {
        UDSMissileFXComponent* FX = VisualMissile->GetMissileFX();
        const FVector FlameCenter = FX->GetExhaustLocation() - VisualMissile->GetTravelDirection() * 600.f;
        const TArray<FVector> Cloud = FX->GetContrailWorldLocations();
        if (Cloud.Num() < 4) return false;
        // The older half of the trail is spatially separate from the exhaust.
        return RegionAround(FlameCenter, 1500.f, FlameRegion) && RegionAround(Cloud[2], 3300.f, CloudRegion);
    }

    void CompareRegion(const TArray<FColor>& Hidden, int32 RegionIndex)
    {
        const auto PixelDifference = [this, &Hidden](int32 X, int32 Y)
        {
            const int32 Index = Y * ViewSize.X + X;
            const FColor& A = VisiblePixels[Index];
            const FColor& B = Hidden[Index];
            return FMath::Max3(FMath::Abs(int32(A.R) - B.R), FMath::Abs(int32(A.G) - B.G), FMath::Abs(int32(A.B) - B.B));
        };
        const TCHAR* Names[] = { TEXT("flame"), TEXT("contrail") };
        const FIntRect Regions[] = { FlameRegion, CloudRegion };
        {
            const FIntRect& Region = Regions[RegionIndex];
            int32 Changed = 0, Peak = 0;
            double Sum = 0.;
            for (int32 Y = Region.Min.Y; Y < Region.Max.Y; ++Y)
                for (int32 X = Region.Min.X; X < Region.Max.X; ++X)
                {
                    const int32 Delta = PixelDifference(X, Y);
                    if (Delta >= 12) ++Changed;
                    Peak = FMath::Max(Peak, Delta);
                    Sum += Delta;
                }
            // A nearby empty patch distinguishes local effects from exposure,
            // camera fade or whole-frame temporal changes between readbacks.
            const int32 ControlX = FMath::Clamp(Region.Min.X, 0, ViewSize.X - 60);
            const int32 ControlY = FMath::Clamp(Region.Min.Y - 100, 0, ViewSize.Y - 60);
            double ControlSum = 0.;
            for (int32 Y = ControlY; Y < ControlY + 60; ++Y)
                for (int32 X = ControlX; X < ControlX + 60; ++X) ControlSum += PixelDifference(X, Y);
            const double MeanDifference = Sum / FMath::Max(Region.Area(), 1);
            const double ControlMean = ControlSum / 3600.;
            const bool bContrast = Changed >= (RegionIndex == 0 ? 12 : 80) && Peak >= 24 && MeanDifference > ControlMean + .3;
            const FString Label = FString::Printf(TEXT("%s isolated %s changes viewport pixels while the other effect stays hidden"), bHostileVisual ? TEXT("Hostile") : TEXT("Friendly"), Names[RegionIndex]);
            Check(*Label, bContrast);
            Observations.Add(FString::Printf(TEXT("{\"case\":\"%s_%s_contrast\",\"comparison\":\"isolated effect versus both effects hidden\",\"roi\":[%d,%d,%d,%d],\"pixels_changed_at_least_12\":%d,\"peak_channel_difference\":%d,\"mean_channel_difference\":%.6f,\"empty_patch_mean_difference\":%.6f}"),
                bHostileVisual ? TEXT("hostile") : TEXT("friendly"), Names[RegionIndex], Region.Min.X, Region.Min.Y, Region.Max.X, Region.Max.Y,
                Changed, Peak, MeanDifference, ControlMean));
        }
    }

    bool Finish(const TCHAR* Error = nullptr)
    {
        bFinished = true;
        bInjectMissile = false;
        RemoveDelegate();
        if (Error) Test->AddError(Error);
        if (bStateCaptured && Mode.IsValid())
        {
            if (Input.IsValid() && Mode->Player && Mode->Player->MissileAction)
                Input->InjectInputForAction(Mode->Player->MissileAction, FInputActionValue(false), {}, {});
            Mode->ReturnToMenu();
            Mode->SelectAircraft(OriginalSelection);
            Mode->SetActorTickEnabled(bOriginalModeTick);
            if (Mode->Player) Mode->Player->SetActorTickEnabled(bOriginalPlayerTick);
        }
        if (bStateCaptured && PC.IsValid())
        {
            PC->bAutoManageActiveCameraTarget = bOriginalAutoCamera;
            if (OriginalViewTarget.IsValid()) PC->SetViewTarget(OriginalViewTarget.Get());
        }
        if (Camera.IsValid()) Camera->Destroy();
        if (!OutputDir.IsEmpty())
        {
            const FString Report = FString::Printf(TEXT("{\"schema\":\"missile-acceptance/1\",\"status\":\"%s\",\"fixture\":\"real game world; normal input and projectile commands; no AutoPlay\",\"visual_evidence\":\"numeric viewport contrast only; no human image review\",\"media_files_written\":0,\"viewport\":[%d,%d],\"checks\":[%s],\"observations\":[%s]}\n"),
                Test->HasAnyErrors() ? TEXT("failed") : TEXT("passed"), ViewSize.X, ViewSize.Y, *FString::Join(Checks, TEXT(",")), *FString::Join(Observations, TEXT(",")));
            Test->TestTrue(TEXT("JSON acceptance report saved"), FFileHelper::SaveStringToFile(Report, *(OutputDir / TEXT("report.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
            Test->AddInfo(TEXT("Missile acceptance JSON: ") + OutputDir);
        }
        return true;
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDSMissileAcceptanceTest, "DuneSentinel.Rendering.MissileAcceptance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FDSMissileAcceptanceTest::RunTest(const FString& Parameters)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("DSMissileValidation")) ||
        FParse::Param(FCommandLine::Get(), TEXT("AutoPlay")) || FParse::Param(FCommandLine::Get(), TEXT("CaptureFrames")))
    {
        AddError(TEXT("Use dedicated -game -DSMissileValidation without AutoPlay or CaptureFrames on DesertDefense."));
        return false;
    }
    ADD_LATENT_AUTOMATION_COMMAND(FDSMissileAcceptanceCommand(this));
    return true;
}
#endif
