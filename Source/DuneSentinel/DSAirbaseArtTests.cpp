#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "DSAircraftExhaustComponent.h"
#include "DSCannonFXComponent.h"
#include "DSEnemyAircraft.h"
#include "DSFlightPawn.h"
#include "DSGameMode.h"
#include "DSImpactFX.h"
#include "DSProjectile.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMeshSocket.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "MaterialShared.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "SceneInterface.h"
#include "ShaderCompiler.h"
#include "StaticMeshResources.h"
#include "UnrealClient.h"

namespace
{
FString AirbaseJsonString(const FString& Value)
{
    return TEXT("\"") + Value.ReplaceCharWithEscapedChar() + TEXT("\"");
}

// Observe the real imported map and ordinary menu, flight and weapon paths.
// Framebuffer pixels remain in memory; only numeric findings are written.
class FDSAirbaseArtAcceptanceCommand final : public IAutomationLatentCommand
{
public:
    explicit FDSAirbaseArtAcceptanceCommand(FAutomationTestBase* InTest) : Test(InTest) {}
    virtual ~FDSAirbaseArtAcceptanceCommand() override { RemoveDelegates(); }

    virtual bool Update() override
    {
        if (bFinished) return true;
        if (GetCurrentRunTime() > 180.) return Finish(TEXT("Airbase art acceptance exceeded 180 seconds."));
        if (!bInitialized) return Initialize();
        if (!World.IsValid() || !Mode.IsValid() || !Mode->Player || !PC.IsValid() || !Camera.IsValid())
            return Finish(TEXT("The real map or observed player became unavailable."));
        if (Stage == EStage::GunInput)
        {
            if (!bGunObserved && FPlatformTime::Seconds() - StageStarted < 5.) return false;
            Check(CaseName() + TEXT(" normal cannon input fires from the imported muzzle"), bGunObserved && GunMountErrorCm < 2.);
            Observations.Add(FString::Printf(TEXT("{\"case\":%s,\"cannon_fired\":%s,\"cannon_mount_error_cm\":%.6f}"),
                *AirbaseJsonString(CaseName()), bGunObserved ? TEXT("true") : TEXT("false"), GunMountErrorCm));
            Mode->Player->SetActorTickEnabled(false);
            Mode->Player->ClearFlightInput();
            if (Input.IsValid()) Input->InjectInputForAction(Mode->Player->GunAction, FInputActionValue(false), {}, {});
            if (UDSCannonFXComponent* FX = Mode->Player->FindComponentByClass<UDSCannonFXComponent>()) FX->ResetEffects();
            ClearProjectiles();
            Mode->Player->SetActorLocationAndRotation(TestOrigin, FRotator::ZeroRotator);
            PrepareObservation();
            return false;
        }
        if (!ResourcesReady())
        {
            if (FPlatformTime::Seconds() - StageStarted > 35.)
                return Finish(TEXT("Actual imported material/render resources did not become ready within 35 seconds."));
            return false;
        }
        if (ReadyFrames < 3 || GFrameCounter < StageFrame + 8 || FPlatformTime::Seconds() - StageStarted < .8) return false;
        if (Stage == EStage::Warmup)
        {
            for (const TWeakObjectPtr<UStaticMeshComponent>& Mesh : CaseMeshes)
                if (Mesh.IsValid()) CheckMaterialCoverage(Mesh.Get(), CaseName());
            SetCaseVisible(false);
            SetStage(EStage::Baseline);
            return false;
        }
        if (!bReadbackReady)
        {
            if (!bReadbackRequested)
            {
                bReadbackRequested = true;
                ReadbackRequestedAt = FPlatformTime::Seconds();
            }
            else if (FPlatformTime::Seconds() - ReadbackRequestedAt > 5.)
                return Finish(TEXT("The actual viewport did not complete its pending readback."));
            return false;
        }
        bReadbackReady = false;
        if (!ReadbackError.IsEmpty()) return Finish(*ReadbackError);
        if (Stage == EStage::Baseline)
        {
            BaselinePixels = MoveTemp(PendingPixels);
            if (!BuildRegion()) return Finish(*(CaseName() + TEXT(" observation bounds are outside the view.")));
            SetCaseVisible(true);
            SetStage(EStage::Visible);
        }
        else
        {
            VisiblePixels = MoveTemp(PendingPixels);
            if (BaselinePixels.Num() != VisiblePixels.Num()) return Finish(TEXT("Viewport size changed between comparisons."));
            CompareFrames();
            BaselinePixels.Reset();
            VisiblePixels.Reset();
            SetCaseVisible(true);
            if (++VisualCase == 7) return Finish();
            BeginCase();
        }
        return bFinished;
    }

private:
    enum class EStage { GunInput, Warmup, Baseline, Visible };
    FAutomationTestBase* Test;
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<ADSGameMode> Mode;
    TWeakObjectPtr<APlayerController> PC;
    TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> Input;
    TWeakObjectPtr<ACameraActor> Camera;
    TWeakObjectPtr<AActor> OriginalViewTarget;
    TWeakObjectPtr<ADSEnemyAircraft> ObservedEnemy;
    TArray<TWeakObjectPtr<AStaticMeshActor>> Airport, Hangars, Towers, Services, Surfaces;
    TArray<TWeakObjectPtr<UStaticMeshComponent>> CaseMeshes;
    TArray<FString> Checks, Observations;
    TArray<FColor> BaselinePixels, VisiblePixels, PendingPixels;
    FDelegateHandle PreTickHandle, PostTickHandle, ViewportHandle;
    FBox CaseBounds = FBox(ForceInit);
    FIntPoint ViewSize = FIntPoint::ZeroValue;
    FIntRect Region;
    FString OutputDir, ReadbackError;
    EStage Stage = EStage::Warmup;
    int32 VisualCase = 0, OriginalSelection = 0, ReadyFrames = 0, RemainingShaderJobs = 0;
    uint64 StageFrame = 0, LastReadyFrame = MAX_uint64, GunStartCount = 0;
    double StageStarted = 0., ReadbackRequestedAt = 0., GunMountErrorCm = -1.;
    bool bInitialized = false, bStateCaptured = false, bFinished = false, bGunObserved = false;
    bool bReadbackRequested = false, bReadbackReady = false;
    bool bOriginalModeTick = true, bOriginalPlayerTick = true, bOriginalPlayerHidden = false;
    bool bOriginalHUD = true, bOriginalAutoCamera = true;
    const FVector TestOrigin = FVector(-400000., -400000., 500000.);

    FString CaseName() const
    {
        static const TCHAR* Names[] = {TEXT("F16"), TEXT("Rafale"), TEXT("F22"), TEXT("hostile_F22"), TEXT("hangars"), TEXT("control_towers"), TEXT("Tu22")};
        return Names[FMath::Clamp(VisualCase, 0, 6)];
    }

    void Check(const FString& Name, bool bPassed)
    {
        Test->TestTrue(Name, bPassed);
        Checks.Add(FString::Printf(TEXT("{\"name\":%s,\"passed\":%s}"), *AirbaseJsonString(Name), bPassed ? TEXT("true") : TEXT("false")));
    }

    bool Initialize()
    {
        if (!GEngine || !GEngine->GameViewport || !GEngine->GameViewport->Viewport) return false;
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
            if (Context.WorldType == EWorldType::Game && Context.World() && Context.World()->HasBegunPlay()) World = Context.World();
        if (!World.IsValid()) return false;
        Mode = World->GetAuthGameMode<ADSGameMode>();
        PC = UGameplayStatics::GetPlayerController(World.Get(), 0);
        if (!Mode.IsValid() || !Mode->Player || !PC.IsValid()) return Finish(TEXT("Run this fixture on DesertDefense with its normal player and mode."));
        if (PC->GetLocalPlayer()) Input = PC->GetLocalPlayer()->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
        if (!Input.IsValid()) return Finish(TEXT("The normal Enhanced Input subsystem is unavailable."));
        OutputDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Validation/AirbaseArt") /
            (FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")) + TEXT("-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8)));
        IFileManager::Get().MakeDirectory(*OutputDir, true);
        OriginalSelection = Mode->SelectedAircraft;
        bOriginalModeTick = Mode->IsActorTickEnabled();
        bOriginalPlayerTick = Mode->Player->IsActorTickEnabled();
        bOriginalPlayerHidden = Mode->Player->IsHidden();
        bOriginalAutoCamera = PC->bAutoManageActiveCameraTarget;
        OriginalViewTarget = PC->GetViewTarget();
        if (PC->GetHUD()) bOriginalHUD = PC->GetHUD()->bShowHUD;
        bStateCaptured = true;
        Mode->ReturnToMenu();
        Mode->SetActorTickEnabled(false);
        Mode->Player->SetActorTickEnabled(false);
        if (PC->GetHUD()) PC->GetHUD()->bShowHUD = false;
        PC->bAutoManageActiveCameraTarget = false;
        Camera = World->SpawnActor<ACameraActor>();
        if (!Camera.IsValid()) return Finish(TEXT("Could not create the observation camera."));
        Camera->GetCameraComponent()->SetFieldOfView(55.f);
        for (TActorIterator<AStaticMeshActor> It(World.Get()); It; ++It)
        {
            if (It->ActorHasTag(TEXT("DSAirbaseTask06"))) Airport.Add(*It);
            if (It->ActorHasTag(TEXT("DSAirbaseHangar"))) Hangars.Add(*It);
            if (It->ActorHasTag(TEXT("DSAirbaseTower"))) Towers.Add(*It);
            if (It->ActorHasTag(TEXT("DSAirbaseService"))) Services.Add(*It);
            if (It->ActorHasTag(TEXT("DSAirbaseSurface"))) Surfaces.Add(*It);
        }
        Check(TEXT("Detailed airport contains hangars, control tower, service equipment and runway surfaces"),
            !Hangars.IsEmpty() && !Towers.IsEmpty() && Services.Num() >= 3 && !Surfaces.IsEmpty());
        if (Hangars.IsEmpty() || Towers.IsEmpty()) return Finish(TEXT("Imported hangar and control tower actors are required for actual rendering acceptance."));
        CheckAirportGeometry();
        PreTickHandle = FWorldDelegates::OnWorldPreActorTick.AddRaw(this, &FDSAirbaseArtAcceptanceCommand::BeforeTick);
        PostTickHandle = FWorldDelegates::OnWorldPostActorTick.AddRaw(this, &FDSAirbaseArtAcceptanceCommand::AfterTick);
        ViewportHandle = UGameViewportClient::OnViewportRendered().AddRaw(this, &FDSAirbaseArtAcceptanceCommand::AfterViewportRendered);
        bInitialized = true;
        BeginCase();
        return false;
    }

    void ClearProjectiles()
    {
        for (TActorIterator<ADSProjectile> It(World.Get()); It; ++It) It->Destroy();
        for (TActorIterator<ADSBomb> It(World.Get()); It; ++It) It->Destroy();
        for (TActorIterator<ADSImpactFX> It(World.Get()); It; ++It) It->Destroy();
    }

    void ClearEnemies()
    {
        for (TActorIterator<ADSEnemyAircraft> It(World.Get()); It; ++It) It->Destroy();
        Mode->Enemies.Reset();
        ObservedEnemy.Reset();
        ClearProjectiles();
    }

    void SetStage(EStage NewStage)
    {
        Stage = NewStage;
        StageStarted = FPlatformTime::Seconds();
        StageFrame = GFrameCounter;
        ReadyFrames = 0;
        LastReadyFrame = MAX_uint64;
    }

    void BeforeTick(UWorld* TickWorld, ELevelTick, float)
    {
        if (TickWorld == World.Get() && Input.IsValid() && Mode.IsValid() && Mode->Player && Mode->Player->GunAction)
            Input->InjectInputForAction(Mode->Player->GunAction, FInputActionValue(Stage == EStage::GunInput && !bGunObserved), {}, {});
    }

    void AfterTick(UWorld* TickWorld, ELevelTick, float)
    {
        if (TickWorld != World.Get() || Stage != EStage::GunInput || bGunObserved || !Mode.IsValid() || !Mode->Player) return;
        UDSCannonFXComponent* FX = Mode->Player->FindComponentByClass<UDSCannonFXComponent>();
        UStaticMeshComponent* Mesh = Mode->Player->GetAirframeMesh();
        if (FX && Mesh && FX->GetShotCount() > GunStartCount)
        {
            bGunObserved = true;
            GunMountErrorCm = FVector::Distance(FX->GetLastMuzzleLocation(), Mesh->GetSocketLocation(TEXT("Cannon")));
        }
    }

    void BeginCase()
    {
        CaseMeshes.Reset();
        ClearEnemies();
        Mode->ReturnToMenu();
        Mode->Player->SetActorTickEnabled(false);
        Mode->Player->SetActorHiddenInGame(false);
        if (VisualCase < 3)
        {
            Mode->SelectAircraft(VisualCase);
            UStaticMeshComponent* Airframe = Mode->Player->GetAirframeMesh();
            const FString ExpectedName = TEXT("SM_AU_") + CaseName();
            Check(CaseName() + TEXT(" menu uses its complete imported parked pose"), Airframe && Airframe->GetStaticMesh() && Airframe->GetStaticMesh()->GetName() == ExpectedName + TEXT("_Parked"));
            if (!Airframe || !Airframe->GetStaticMesh()) { Finish(TEXT("A selected imported parked airframe is missing.")); return; }
            CheckParkedContact(Airframe);
            int32 VisibleLegacyGear = 0;
            TArray<UStaticMeshComponent*> PlayerMeshes;
            Mode->Player->GetComponents(PlayerMeshes);
            for (UStaticMeshComponent* Mesh : PlayerMeshes)
                if (Mesh != Airframe && Mesh->GetStaticMesh() && Mesh->GetStaticMesh()->GetName().Contains(TEXT("landinggear")) && Mesh->IsVisible() && !Mesh->bHiddenInGame) ++VisibleLegacyGear;
            Check(CaseName() + TEXT(" parked pose hides the obsolete shared landing gear"), VisibleLegacyGear == 0);
            Mode->StartSortie();
            ClearEnemies();
            Mode->Player->SetActorLocationAndRotation(TestOrigin, FRotator::ZeroRotator);
            Check(CaseName() + TEXT(" normal sortie selects its gear-retracted imported flight pose"), Airframe->GetStaticMesh() && Airframe->GetStaticMesh()->GetName() == ExpectedName && Mode->Player->IsFlying());
            CaseMeshes.Add(Airframe);
            CheckMeshGeometry(Airframe, CaseName(), 10000, VisualCase == 0 ? 2 : 1);
            CheckExhaust(Airframe, Mode->Player->GetAircraftExhaust(), VisualCase == 0 ? 1 : 2);
            CheckMissileRails();
            Mode->Player->SetActorLocationAndRotation(TestOrigin, FRotator::ZeroRotator);
            UDSCannonFXComponent* FX = Mode->Player->FindComponentByClass<UDSCannonFXComponent>();
            GunStartCount = FX ? FX->GetShotCount() : 0;
            GunMountErrorCm = -1.;
            bGunObserved = false;
            SetStage(EStage::GunInput);
            Mode->Player->SetActorTickEnabled(true);
        }
        else if (VisualCase == 3 || VisualCase == 6)
        {
            const bool bBomberCase = VisualCase == 6;
            Mode->StartSortie();
            ClearEnemies();
            Mode->Player->SetActorLocationAndRotation(TestOrigin + FVector(bBomberCase ? -50000 : 50000, 0, 0), FRotator::ZeroRotator);
            ObservedEnemy = World->SpawnActor<ADSEnemyAircraft>(TestOrigin, FRotator::ZeroRotator);
            if (!ObservedEnemy.IsValid()) { Finish(TEXT("Could not create the normal hostile aircraft.")); return; }
            ObservedEnemy->Initialize(bBomberCase, Mode->AirportLocation);
            ObservedEnemy->SetActorTickEnabled(false);
            UStaticMeshComponent* Mesh = ObservedEnemy->Mesh;
            const FString ExpectedMesh = bBomberCase ? TEXT("SM_AU_Tu22") : TEXT("SM_AU_F22");
            Check(CaseName() + TEXT(" normal hostile initialization selects its imported geometry"), Mesh && Mesh->GetStaticMesh() && Mesh->GetStaticMesh()->GetName() == ExpectedMesh);
            CaseMeshes.Add(Mesh);
            CheckMeshGeometry(Mesh, CaseName(), 10000, 1);
            CheckEnemyHitProxy();
            CheckExhaust(Mesh, ObservedEnemy->GetAircraftExhaust(), 2);
            UDSCannonFXComponent* FX = ObservedEnemy->FindComponentByClass<UDSCannonFXComponent>();
            const uint64 Before = FX ? FX->GetShotCount() : 0;
            ObservedEnemy->Tick(2.1f);
            Check(TEXT("Hostile normal attack fires from its actual imported cannon socket"), FX && FX->GetShotCount() > Before &&
                Mesh->DoesSocketExist(TEXT("Cannon")) && FVector::Distance(FX->GetLastMuzzleLocation(), Mesh->GetSocketLocation(TEXT("Cannon"))) < 2.);
            if (bBomberCase) CheckBomberGunArc();
            if (FX) FX->ResetEffects();
            ClearProjectiles();
            if (bBomberCase) CheckBomberAttachments();
            ObservedEnemy->SetActorLocationAndRotation(TestOrigin, FRotator::ZeroRotator);
            Mode->Player->SetActorHiddenInGame(true);
            PrepareObservation();
        }
        else
        {
            Mode->Player->SetActorHiddenInGame(true);
            const TArray<TWeakObjectPtr<AStaticMeshActor>>& Actors = VisualCase == 4 ? Hangars : Towers;
            for (const TWeakObjectPtr<AStaticMeshActor>& Actor : Actors)
                if (Actor.IsValid()) CaseMeshes.Add(Actor->GetStaticMeshComponent());
            PrepareObservation();
        }
    }

    void CheckEnemyHitProxy()
    {
        UStaticMeshComponent* Mesh = ObservedEnemy->Mesh;
        USphereComponent* HitProxy = ObservedEnemy->Collision;
        if (!Mesh || !Mesh->GetStaticMesh() || !HitProxy)
        { Check(CaseName() + TEXT(" has a measurable visible mesh and spherical hit proxy"), false); return; }
        const FBox Bounds = Mesh->GetStaticMesh()->GetBoundingBox();
        const double Radius = HitProxy->GetScaledSphereRadius();
        const FVector Center = HitProxy->GetComponentLocation();
        double FarthestVisibleCorner = 0.;
        for (int32 Corner = 0; Corner < 8; ++Corner)
        {
            const FVector Local((Corner & 1) ? Bounds.Max.X : Bounds.Min.X,
                (Corner & 2) ? Bounds.Max.Y : Bounds.Min.Y, (Corner & 4) ? Bounds.Max.Z : Bounds.Min.Z);
            FarthestVisibleCorner = FMath::Max(FarthestVisibleCorner,
                FVector::Distance(Mesh->GetComponentTransform().TransformPosition(Local), Center));
        }
        FHitResult Hit;
        FCollisionQueryParams Params(SCENE_QUERY_STAT(DSImportedEnemyHitProxy), false, Mode->Player.Get());
        const FVector Reach(Radius + 2000., 0, 0);
        const bool bTraced = World->LineTraceSingleByChannel(Hit, Center + Reach, Center - Reach, ECC_Visibility, Params);
        Check(CaseName() + TEXT(" spherical hit proxy encloses the complete visible airframe and responds to weapon traces"),
            Radius >= 1050. && Radius + .5 >= FarthestVisibleCorner && bTraced && Hit.GetActor() == ObservedEnemy.Get());
        Observations.Add(FString::Printf(TEXT("{\"case\":%s,\"hit_proxy_radius_cm\":%.6f,\"farthest_visible_bounds_corner_cm\":%.6f,\"weapon_trace_hit_aircraft\":%s}"),
            *AirbaseJsonString(CaseName()), Radius, FarthestVisibleCorner,
            bTraced && Hit.GetActor() == ObservedEnemy.Get() ? TEXT("true") : TEXT("false")));
    }

    void CheckBomberGunArc()
    {
        UDSCannonFXComponent* FX = ObservedEnemy->FindComponentByClass<UDSCannonFXComponent>();
        if (!FX) { Check(TEXT("Tu22 exposes its normal defensive gun effects"), false); return; }
        // Hold the same aircraft pose between ordinary ticks so steering cannot
        // move a front-side target into the rear firing arc during the check.
        const auto ObserveDirection = [this, FX](const FVector& PlayerOffset, bool bShouldFire, const TCHAR* Label)
        {
            Mode->Player->SetActorLocation(TestOrigin + PlayerOffset);
            const uint64 Before = FX->GetShotCount();
            double MaximumMuzzleError = 0., MaximumAimErrorDegrees = 0.;
            for (int32 Step = 0; Step < 150; ++Step)
            {
                ObservedEnemy->SetActorLocationAndRotation(TestOrigin, FRotator::ZeroRotator);
                const uint64 StepBefore = FX->GetShotCount();
                ObservedEnemy->Tick(1.f / 60.f);
                if (FX->GetShotCount() > StepBefore)
                {
                    const FVector Muzzle = ObservedEnemy->Mesh->GetSocketLocation(TEXT("Cannon"));
                    MaximumMuzzleError = FMath::Max(MaximumMuzzleError, FVector::Distance(FX->GetLastMuzzleLocation(), Muzzle));
                    const FVector ExpectedDirection = (Mode->Player->GetActorLocation() - Muzzle).GetSafeNormal();
                    const double Dot = FVector::DotProduct(FX->GetLastShotDirection(), ExpectedDirection);
                    MaximumAimErrorDegrees = FMath::Max(MaximumAimErrorDegrees,
                        FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Dot, -1., 1.))));
                }
            }
            const uint64 Shots = FX->GetShotCount() - Before;
            Check(Label, bShouldFire ? Shots > 0 && MaximumMuzzleError < 2. && MaximumAimErrorDegrees < 2.2 : Shots == 0);
            Observations.Add(FString::Printf(TEXT("{\"case\":\"Tu22_defensive_arc\",\"check\":%s,\"shots\":%llu,\"maximum_muzzle_error_cm\":%.6f,\"maximum_aim_error_degrees\":%.6f}"),
                *AirbaseJsonString(Label), static_cast<unsigned long long>(Shots), MaximumMuzzleError, MaximumAimErrorDegrees));
            ClearProjectiles();
        };
        ObserveDirection(FVector(10000, 8000, 0), false, TEXT("Tu22 does not fire its tail gun at a player ahead"));
        ObserveDirection(FVector(-10000, 8000, 0), true, TEXT("Tu22 fires toward a rear player from the actual tail muzzle"));
        const FVector SourceMuzzle(-1982.085609, .000095, -103.794837);
        const double OutsideAngle = FMath::DegreesToRadians(80.);
        ObserveDirection(SourceMuzzle + FVector(-FMath::Cos(OutsideAngle), FMath::Sin(OutsideAngle), 0.) * 20000.,
            false, TEXT("Tu22 rejects a nearby target beyond the 75 degree rear firing arc"));
        ObservedEnemy->SetActorLocationAndRotation(TestOrigin, FRotator::ZeroRotator);
    }

    void CheckBomberAttachments()
    {
        UStaticMeshComponent* Mesh = ObservedEnemy->Mesh;
        // Independent normalized source measurements, retained alongside the
        // downloaded bomber FBX; do not derive expected locations from sockets.
        const FVector SourceCannon(-1982.085609, .000095, -103.794837);
        const FVector SourceBombBay(-55.798912, .000095, -324.486303);
        const FVector CannonLocal = Mesh->GetSocketTransform(TEXT("Cannon"), RTS_Component).GetLocation();
        const FVector BombBayLocal = Mesh->GetSocketTransform(TEXT("BombBay"), RTS_Component).GetLocation();
        const FVector GunForward = Mesh->GetSocketTransform(TEXT("Cannon"), RTS_Component).GetRotation().GetForwardVector();
        Check(TEXT("Tu22 rear defensive gun and belly release socket match measured source mounts"),
            Mesh->DoesSocketExist(TEXT("Cannon")) && Mesh->DoesSocketExist(TEXT("BombBay")) &&
            FVector::Distance(CannonLocal, SourceCannon) < 1. && FVector::Distance(BombBayLocal, SourceBombBay) < 1. &&
            FVector::DotProduct(GunForward, -FVector::ForwardVector) > .999);
        ObservedEnemy->SetActorLocationAndRotation(Mode->AirportLocation + FVector(-100000, 0, 65000), FRotator::ZeroRotator);
        const FVector ExpectedRelease = Mesh->GetComponentTransform().TransformPosition(SourceBombBay);
        const FVector ExpectedVelocity = ObservedEnemy->GetVelocity();
        const int32 Before = ObservedEnemy->BombsDropped;
        ObservedEnemy->Tick(.02f);
        ADSBomb* Bomb = nullptr;
        for (TActorIterator<ADSBomb> It(World.Get()); It; ++It) Bomb = *It;
        const double ReleaseError = Bomb ? FVector::Distance(Bomb->GetActorLocation(), ExpectedRelease) : -1.;
        Check(TEXT("Tu22 normal bombing tick releases a real payload at the measured belly mount"),
            Bomb && ObservedEnemy->BombsDropped == Before + 1 && ReleaseError < 2. &&
            FVector::Distance(Bomb->Velocity, ExpectedVelocity) < 1.);
        Observations.Add(FString::Printf(TEXT("{\"case\":\"Tu22_weapon_mounts\",\"rear_cannon_source_error_cm\":%.6f,\"bomb_bay_source_error_cm\":%.6f,\"real_bomb_release_error_cm\":%.6f,\"bombs_before\":%d,\"bombs_after\":%d}"),
            FVector::Distance(CannonLocal, SourceCannon), FVector::Distance(BombBayLocal, SourceBombBay), ReleaseError, Before, ObservedEnemy->BombsDropped));
        ClearProjectiles();
    }

    void CheckParkedContact(UStaticMeshComponent* Airframe)
    {
        const FBox Bounds = Airframe->GetStaticMesh()->GetBoundingBox();
        const double WheelFloor = Airframe->GetComponentTransform().TransformPosition(FVector(0, 0, Bounds.Min.Z)).Z;
        const FVector Socket = Airframe->GetSocketLocation(TEXT("GearFloor"));
        FHitResult Hit;
        FCollisionQueryParams Params(SCENE_QUERY_STAT(DSAirbaseParkContact), true, Mode->Player.Get());
        const bool bHit = World->LineTraceSingleByObjectType(Hit, Socket + FVector(0, 0, 5000), Socket - FVector(0, 0, 5000), FCollisionObjectQueryParams(ECC_WorldStatic), Params);
        Check(CaseName() + TEXT(" parked wheel geometry rests on the actual apron surface"), Airframe->DoesSocketExist(TEXT("GearFloor")) && bHit &&
            FMath::Abs(WheelFloor - Hit.ImpactPoint.Z) < 12. && FMath::Abs(Socket.Z - WheelFloor) < 12.);
        Observations.Add(FString::Printf(TEXT("{\"case\":%s,\"parked_mesh\":%s,\"mesh_wheel_floor_cm\":%.5f,\"socket_floor_cm\":%.5f,\"surface_hit\":%s,\"surface_height_cm\":%.5f}"),
            *AirbaseJsonString(CaseName()), *AirbaseJsonString(Airframe->GetStaticMesh()->GetPathName()), WheelFloor, Socket.Z, bHit ? TEXT("true") : TEXT("false"), bHit ? Hit.ImpactPoint.Z : 0.));
    }

    void CheckMeshGeometry(UStaticMeshComponent* Component, const FString& Label, int32 MinimumTriangles, int32 MinimumUVs)
    {
        const UStaticMesh* Mesh = Component ? Component->GetStaticMesh() : nullptr;
        const FStaticMeshRenderData* Data = Mesh ? Mesh->GetRenderData() : nullptr;
        const bool bLOD = Data && Data->LODResources.Num() > 0;
        const int32 FallbackTriangles = bLOD ? Data->LODResources[0].GetNumTriangles() : 0;
        const int32 NaniteTriangles = Mesh ? Mesh->GetNumNaniteTriangles() : 0;
        const int32 Triangles = NaniteTriangles > 0 ? NaniteTriangles : FallbackTriangles;
        const int32 UVs = bLOD ? Data->LODResources[0].GetNumTexCoords() : 0;
        Check(Label + TEXT(" has detailed native geometry and preserved texture coordinates"), bLOD && Triangles >= MinimumTriangles && UVs >= MinimumUVs);
        const FVector Size = Mesh ? Mesh->GetBoundingBox().GetSize() * Component->GetComponentScale().GetAbs() : FVector::ZeroVector;
        if (VisualCase < 4 && Label == CaseName())
            Check(Label + TEXT(" native aircraft dimensions use physical centimetres"), Size.X >= 1200. && Size.X <= 2300. && Size.Y >= 750. && Size.Y <= 1600. && Size.Z >= 150. && Size.Z <= 700.);
        if (VisualCase == 6 && Label == CaseName())
            Check(TEXT("Tu22 native dimensions retain the measured source metre scale"),
                FVector::Distance(Size, FVector(4267.652893, 2311.864281, 833.347702)) < 5.);
        Observations.Add(FString::Printf(TEXT("{\"case\":%s,\"mesh\":%s,\"triangles\":%d,\"nanite_triangles\":%d,\"fallback_triangles\":%d,\"uv_channels\":%d,\"material_slots\":%d,\"physical_dimensions_cm\":[%.4f,%.4f,%.4f]}"),
            *AirbaseJsonString(Label), *AirbaseJsonString(Mesh ? Mesh->GetPathName() : TEXT("missing")), Triangles, NaniteTriangles, FallbackTriangles, UVs, Component ? Component->GetNumMaterials() : 0, Size.X, Size.Y, Size.Z));
    }

    void CheckExhaust(UStaticMeshComponent* Mesh, UDSAircraftExhaustComponent* Exhaust, int32 Count)
    {
        if (!Mesh || !Mesh->GetStaticMesh() || !Exhaust) { Check(CaseName() + TEXT(" has its normal exhaust component"), false); return; }
        const TArray<FVector> Roots = Exhaust->GetNozzleWorldLocations();
        const TArray<FVector> AftDirections = Exhaust->GetNozzleWorldAftDirections();
        const TArray<float>& Radii = Exhaust->GetNozzleRadiiCm();
        bool bMounted = Roots.Num() == Count && Exhaust->GetVisualPoolSize() == Count * 3 && Exhaust->IsExhaustVisible();
        double MaxError = 0.;
        const FBox Bounds = Mesh->GetStaticMesh()->GetBoundingBox();
        for (int32 Index = 0; Index < Count; ++Index)
        {
            const FName Name(*FString::Printf(TEXT("Exhaust_%02d"), Index + 1));
            const UStaticMeshSocket* Socket = Mesh->GetStaticMesh()->FindSocket(Name);
            bMounted &= Socket && Roots.IsValidIndex(Index) && AftDirections.IsValidIndex(Index) && Radii.IsValidIndex(Index);
            if (!Socket || !Roots.IsValidIndex(Index) || !AftDirections.IsValidIndex(Index) || !Radii.IsValidIndex(Index)) continue;
            const FVector Local = Mesh->GetComponentTransform().InverseTransformPosition(Roots[Index]);
            MaxError = FMath::Max(MaxError, FVector::Distance(Local, Socket->RelativeLocation));
            bMounted &= Socket->RelativeLocation.X < Bounds.GetCenter().X - Bounds.GetExtent().X * .2 &&
                Bounds.ExpandBy(100.).IsInsideOrOn(Socket->RelativeLocation) && Socket->RelativeScale.X > 0. &&
                FMath::Abs(Radii[Index] - Socket->RelativeScale.X) < .01 &&
                FVector::DotProduct(AftDirections[Index], Mesh->GetComponentTransform().TransformVectorNoScale(-Socket->RelativeRotation.Vector()).GetSafeNormal()) > .999;
        }
        Check(CaseName() + TEXT(" actual rear nozzles drive the correct bounded exhaust pool"), bMounted && MaxError < 3.);
        AActor* Owner = Mesh->GetOwner();
        const FTransform Original = Owner->GetActorTransform();
        TArray<FVector> LocalRoots;
        for (const FVector& Root : Roots) LocalRoots.Add(Original.InverseTransformPosition(Root));
        const FTransform Banked(FRotator(22, 38, 67), TestOrigin + FVector(300, -400, 500));
        Owner->SetActorTransform(Banked);
        const TArray<FVector> BankedRoots = Exhaust->GetNozzleWorldLocations();
        double AttachmentError = 0.;
        for (int32 Index = 0; Index < LocalRoots.Num() && Index < BankedRoots.Num(); ++Index)
            AttachmentError = FMath::Max(AttachmentError, FVector::Distance(Banked.TransformPosition(LocalRoots[Index]), BankedRoots[Index]));
        Owner->SetActorTransform(Original);
        const int32 ComponentsBefore = Owner->GetComponents().Num();
        bool bStable = true;
        for (int32 Index = 0; Index < 90; ++Index)
        {
            Exhaust->UpdateExhaust(.02f, Index % 2 ? 1.f : 0.f, Index % 2 ? 0.f : 1.f);
            bStable &= Owner->GetComponents().Num() == ComponentsBefore && Exhaust->GetVisualPoolSize() == Count * 3;
        }
        Exhaust->ResetToCruise();
        Check(CaseName() + TEXT(" banked nozzle attachment and throttle updates preserve component counts"), AttachmentError < .5 && bStable);
        const float CruiseLength = Exhaust->GetFlameLengthCm();
        for (int32 Index = 0; Index < 60; ++Index) Exhaust->UpdateExhaust(.02f, 1.f, 0.f);
        const float BoostLength = Exhaust->GetFlameLengthCm(), BoostRadius = Exhaust->GetCoreRadiusCm(), BoostGlow = Exhaust->GetGlowStrength();
        for (int32 Index = 0; Index < 60; ++Index) Exhaust->UpdateExhaust(.02f, 0.f, 1.f);
        const float BrakeLength = Exhaust->GetFlameLengthCm(), BrakeRadius = Exhaust->GetCoreRadiusCm(), BrakeGlow = Exhaust->GetGlowStrength();
        Check(CaseName() + TEXT(" imported nozzle flame size and brightness respond to throttle and brake"), BoostLength > CruiseLength * 1.25f && CruiseLength > BrakeLength * 1.25f && BoostRadius > BrakeRadius * 1.25f && BoostGlow > BrakeGlow * 1.5f);
        Exhaust->ResetToCruise();
        Observations.Add(FString::Printf(TEXT("{\"case\":%s,\"nozzles\":%d,\"pool_primitives\":%d,\"maximum_socket_lip_distance_cm\":%.5f,\"bank_attachment_error_cm\":%.5f,\"cruise_flame_length_cm\":%.4f,\"boost_flame_length_cm\":%.4f,\"brake_flame_length_cm\":%.4f,\"boost_core_radius_cm\":%.4f,\"brake_core_radius_cm\":%.4f,\"boost_glow\":%.4f,\"brake_glow\":%.4f}"),
            *AirbaseJsonString(CaseName()), Roots.Num(), Exhaust->GetVisualPoolSize(), MaxError, AttachmentError, CruiseLength, BoostLength, BrakeLength, BoostRadius, BrakeRadius, BoostGlow, BrakeGlow));
    }

    void CheckMissileRails()
    {
        ADSFlightPawn* Player = Mode->Player;
        UStaticMeshComponent* Mesh = Player->GetAirframeMesh();
        for (int32 Side = 0; Side < 2; ++Side)
        {
            if (Side) Player->Tick(1.3f);
            Player->MissileAmmo = Side ? 5 : 6;
            Player->LockProgress = 0.f;
            Player->Target.Reset();
            const FName Rail = Side ? TEXT("Missile_R") : TEXT("Missile_L");
            const FVector Expected = Mesh->GetSocketLocation(Rail);
            const int32 Before = Player->MissileAmmo;
            Player->FireMissile();
            ADSProjectile* Missile = nullptr;
            for (TActorIterator<ADSProjectile> It(World.Get()); It; ++It)
                if (It->IsPlayerOwned() && It->IsMissile()) Missile = *It;
            const double Error = Missile ? FVector::Distance(Missile->GetActorLocation(), Expected) : -1.;
            Check(CaseName() + TEXT(" unguided missile releases from ") + Rail.ToString(), Mesh->DoesSocketExist(Rail) && Missile &&
                Error < 2. && !Missile->HasGuidanceTarget() && Player->MissileAmmo == Before - 1 &&
                FVector::DotProduct(Missile->GetTravelDirection(), Player->GetActorForwardVector()) > .999);
            Observations.Add(FString::Printf(TEXT("{\"case\":%s,\"rail\":%s,\"release_error_cm\":%.5f,\"ammo_before\":%d,\"ammo_after\":%d}"),
                *AirbaseJsonString(CaseName()), *AirbaseJsonString(Rail.ToString()), Error, Before, Player->MissileAmmo));
            ClearProjectiles();
        }
    }

    void CheckAirportGeometry()
    {
        bool bRunwayClear = true;
        double MaximumHeight = -DBL_MAX;
        for (int32 X = -80000; X <= 80000; X += 10000)
        {
            FHitResult Hit;
            FCollisionQueryParams Params(SCENE_QUERY_STAT(DSAirbaseRunway), true, Mode->Player.Get());
            const bool bHit = World->LineTraceSingleByObjectType(Hit, FVector(X, 0, 100000), FVector(X, 0, -5000), FCollisionObjectQueryParams(ECC_WorldStatic), Params);
            bRunwayClear &= bHit && Hit.ImpactPoint.Z >= 2150. && Hit.ImpactPoint.Z <= 2400.;
            if (bHit) MaximumHeight = FMath::Max(MaximumHeight, Hit.ImpactPoint.Z);
        }
        Check(TEXT("Detailed airport leaves the actual runway centreline clear"), bRunwayClear);
        int32 VisibleLegacyAirbases = 0;
        for (TActorIterator<AStaticMeshActor> It(World.Get()); It; ++It)
            if (!It->IsHidden() && It->GetStaticMeshComponent()->GetStaticMesh() && It->GetStaticMeshComponent()->GetStaticMesh()->GetName() == TEXT("SM_Airbase")) ++VisibleLegacyAirbases;
        Check(TEXT("The obsolete combined airbase is removed from the visible level"), VisibleLegacyAirbases == 0);
        bool bAirportBounds = !Airport.IsEmpty();
        for (const TWeakObjectPtr<AStaticMeshActor>& Actor : Airport)
            if (Actor.IsValid())
                bAirportBounds &= Actor->GetStaticMeshComponent()->GetStaticMesh() && !Actor->IsHidden() &&
                    FVector::Dist2D(Actor->GetActorLocation(), Mode->AirportLocation) < 250000.;
        Check(TEXT("New airport dressing exists in the objective area and is visible"), bAirportBounds);
        for (const TWeakObjectPtr<AStaticMeshActor>& Actor : Hangars)
            if (Actor.IsValid()) CheckMeshGeometry(Actor->GetStaticMeshComponent(), TEXT("hangar native asset"), 20000, 1);
        for (const TWeakObjectPtr<AStaticMeshActor>& Actor : Towers)
            if (Actor.IsValid()) CheckMeshGeometry(Actor->GetStaticMeshComponent(), TEXT("control tower native asset"), 20000, 1);
        Observations.Add(FString::Printf(TEXT("{\"case\":\"airport_layout\",\"hangars\":%d,\"towers\":%d,\"service_props\":%d,\"surface_actors\":%d,\"highest_runway_collision_cm\":%.4f}"),
            Hangars.Num(), Towers.Num(), Services.Num(), Surfaces.Num(), MaximumHeight == -DBL_MAX ? 0. : MaximumHeight));
    }

    void PrepareObservation()
    {
        CaseBounds = FBox(ForceInit);
        if (VisualCase < 3) Mode->Player->GetAircraftExhaust()->SetExhaustVisible(false);
        if (ObservedEnemy.IsValid()) ObservedEnemy->GetAircraftExhaust()->SetExhaustVisible(false);
        for (const TWeakObjectPtr<UStaticMeshComponent>& Mesh : CaseMeshes)
            if (Mesh.IsValid())
            {
                Mesh->UpdateBounds();
                CaseBounds += Mesh->Bounds.GetBox();
            }
        if (!CaseBounds.IsValid) { Finish(TEXT("Observed art has no valid world bounds.")); return; }
        const FVector Center = CaseBounds.GetCenter();
        const double Radius = FMath::Max(CaseBounds.GetExtent().Size(), 500.);
        // Horizontal FOV alone cannot frame the tall control tower in a wide
        // viewport. Fit the bounding sphere to the smaller camera half-angle.
        const FIntPoint Pixels = GEngine->GameViewport->Viewport->GetSizeXY();
        const double Aspect = double(Pixels.X) / FMath::Max(Pixels.Y, 1);
        const double HorizontalHalf = FMath::DegreesToRadians(Camera->GetCameraComponent()->FieldOfView * .5);
        const double VerticalHalf = FMath::Atan(FMath::Tan(HorizontalHalf) / Aspect);
        const double Distance = Radius * 1.15 / FMath::Sin(FMath::Min(HorizontalHalf, VerticalHalf));
        const FVector Position = Center + FVector(-1.6, -2.1, .9).GetSafeNormal() * Distance;
        Camera->SetActorLocationAndRotation(Position, (Center - Position).Rotation());
        PC->SetViewTarget(Camera.Get());
        if (PC->PlayerCameraManager) PC->PlayerCameraManager->StopCameraFade();
        SetCaseVisible(true);
        SetStage(EStage::Warmup);
    }

    void SetCaseVisible(bool bVisible)
    {
        for (const TWeakObjectPtr<UStaticMeshComponent>& Mesh : CaseMeshes)
            if (Mesh.IsValid()) Mesh->SetVisibility(bVisible);
    }

    bool ResourcesReady()
    {
        if (!World->Scene) return false;
        bool bReady = !CaseMeshes.IsEmpty();
        TSet<UMaterialInterface*> Materials;
        for (const TWeakObjectPtr<UStaticMeshComponent>& Mesh : CaseMeshes)
        {
            if (!Mesh.IsValid()) { bReady = false; continue; }
            bReady &= Mesh->GetStaticMesh() && !Mesh->GetStaticMesh()->IsCompiling() && Mesh->IsRegistered() &&
                !Mesh->CheckPSOPrecachingAndBoostPriority() && (Stage == EStage::Baseline || Mesh->GetSceneProxy());
            for (int32 Index = 0; Index < Mesh->GetNumMaterials(); ++Index)
                if (UMaterialInterface* Material = Mesh->GetMaterial(Index)) Materials.Add(Material);
                else bReady = false;
        }
        TArray<const FMaterialRenderProxy*> Proxies;
        TArray<UMaterial*> Expected;
        for (UMaterialInterface* Interface : Materials)
        {
#if WITH_EDITOR
            if (const FMaterialResource* Resource = Interface->GetMaterialResource(World->Scene->GetShaderPlatform())) Resource->IsCompilationFinished();
#endif
            Proxies.Add(Interface->GetRenderProxy());
            Expected.Add(Interface->GetMaterial());
        }
        TArray<bool> Valid;
        Valid.Init(false, Proxies.Num());
        const ERHIFeatureLevel::Type Feature = World->Scene->GetFeatureLevel();
        ENQUEUE_RENDER_COMMAND(DSObserveImportedAirbaseMaterials)([Proxies, Expected, Feature, &Valid](FRHICommandListImmediate&)
        {
            for (int32 Index = 0; Index < Proxies.Num(); ++Index)
            {
                const FMaterial* Material = Proxies[Index] ? Proxies[Index]->GetMaterialNoFallback(Feature) : nullptr;
                const FMaterialShaderMap* Map = Material ? Material->GetRenderingThreadShaderMap() : nullptr;
                Valid[Index] = Material && Map && Map->IsValidForRendering() && Material->GetMaterialInterface() == Expected[Index];
            }
        });
        FlushRenderingCommands();
        bReady &= Valid.Num() > 0;
        for (bool bValid : Valid) bReady &= bValid;
        RemainingShaderJobs = 0;
#if WITH_EDITOR
        if (GShaderCompilingManager)
        {
            RemainingShaderJobs = GShaderCompilingManager->GetNumRemainingJobs();
            bReady &= !GShaderCompilingManager->IsCompiling();
        }
#endif
        if (LastReadyFrame != GFrameCounter)
        {
            LastReadyFrame = GFrameCounter;
            ReadyFrames = bReady ? ReadyFrames + 1 : 0;
        }
        return bReady;
    }

    void CheckMaterialCoverage(UStaticMeshComponent* Component, const FString& Label)
    {
        UStaticMesh* Mesh = Component->GetStaticMesh();
        const FStaticMeshRenderData* Data = Mesh ? Mesh->GetRenderData() : nullptr;
        if (!Data || Data->LODResources.Num() == 0) { Check(Label + TEXT(" material inspection has render geometry"), false); return; }
        const FStaticMeshLODResources& LOD = Data->LODResources[0];
        int32 TotalTriangles = 0, TexturedTriangles = 0, HighResolutionTextures = 0, SourceResolutionTextures = 0, MaximumTextureDimension = 0;
        const bool bLimitedBomberSource = Mesh->GetName() == TEXT("SM_AU_Tu22");
        const int32 RequiredSourceDimension = bLimitedBomberSource ? 1024 : 2048;
        bool bAssigned = Component->GetNumMaterials() > 0, bResources = true, bNaniteMaterials = true;
        const bool bNanite = Mesh->GetNumNaniteTriangles() > 0;
        TArray<FString> MaterialRows;
        for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
        {
            UMaterialInterface* Material = Component->GetMaterial(Slot);
            bAssigned &= Material && Material->GetPathName().StartsWith(TEXT("/Game/"));
            const UMaterial* BaseMaterial = Material ? Material->GetMaterial() : nullptr;
            bNaniteMaterials &= !bNanite || (BaseMaterial && BaseMaterial->GetUsageByFlag(MATUSAGE_Nanite));
            TArray<UTexture*> Textures;
            if (Material) Material->GetUsedTextures(Textures, TOptional<EMaterialQualityLevel::Type>(), TOptional<EShaderPlatform>(World->Scene->GetShaderPlatform()));
            int32 SlotTriangles = 0;
            for (const FStaticMeshSection& Section : LOD.Sections) if (Section.MaterialIndex == Slot) SlotTriangles += Section.NumTriangles;
            TotalTriangles += SlotTriangles;
            if (!Textures.IsEmpty()) TexturedTriangles += SlotTriangles;
            TArray<FString> TextureRows;
            for (UTexture* Texture : Textures)
            {
                UTexture2D* Image = Cast<UTexture2D>(Texture);
                if (!Image) continue;
                const bool bHighResolution = FMath::Max(Image->GetSizeX(), Image->GetSizeY()) >= 2048;
                HighResolutionTextures += bHighResolution ? 1 : 0;
                const int32 Dimension = FMath::Max(Image->GetSizeX(), Image->GetSizeY());
                SourceResolutionTextures += Dimension >= RequiredSourceDimension ? 1 : 0;
                MaximumTextureDimension = FMath::Max(MaximumTextureDimension, Dimension);
                bResources &= Image->GetResource() != nullptr;
                TextureRows.Add(FString::Printf(TEXT("{\"path\":%s,\"width\":%d,\"height\":%d,\"resource\":%s}"),
                    *AirbaseJsonString(Image->GetPathName()), Image->GetSizeX(), Image->GetSizeY(), Image->GetResource() ? TEXT("true") : TEXT("false")));
            }
            MaterialRows.Add(FString::Printf(TEXT("{\"slot\":%d,\"material\":%s,\"triangles\":%d,\"textures\":[%s]}"),
                Slot, *AirbaseJsonString(Material ? Material->GetPathName() : TEXT("missing")), SlotTriangles, *FString::Join(TextureRows, TEXT(","))));
        }
        const double Coverage = TotalTriangles ? double(TexturedTriangles) / TotalTriangles : 0.;
        const FString ResourceLabel = bLimitedBomberSource ?
            TEXT(" retains assigned materials and live original 1K texture resources") :
            TEXT(" uses assigned native materials with live high-resolution texture resources");
        Check(Label + ResourceLabel, bAssigned && bResources && bNaniteMaterials && SourceResolutionTextures >= 1 && Coverage >= .60);
        if (bLimitedBomberSource)
        {
            Check(TEXT("Tu22 source texture limit is retained without artificial resolution inflation"), MaximumTextureDimension == 1024 && HighResolutionTextures == 0);
            const TArray<FStaticMaterial>& NativeSlots = Mesh->GetStaticMaterials();
            UMaterialInterface* ExteriorMaterial = Component->GetMaterial(15);
            const bool bExteriorSlot = NativeSlots.IsValidIndex(15) &&
                NativeSlots[15].MaterialSlotName == FName(TEXT("M_AU_tu22_15_DefaultWhite_texture_png")) &&
                ExteriorMaterial && ExteriorMaterial->GetName() == TEXT("M_AU_tu22_15_DefaultWhite_texture_png");
            TArray<UTexture*> ExteriorTextures;
            if (ExteriorMaterial) ExteriorMaterial->GetUsedTextures(ExteriorTextures, TOptional<EMaterialQualityLevel::Type>(),
                TOptional<EShaderPlatform>(World->Scene->GetShaderPlatform()));
            UTexture2D* ExteriorAtlas = nullptr;
            for (UTexture* Texture : ExteriorTextures)
                if (Texture && Texture->GetPathName() == TEXT("/Game/Art/Textures/AirbaseUpgrade/T_image_015_3ae1f26bb5.T_image_015_3ae1f26bb5"))
                    ExteriorAtlas = Cast<UTexture2D>(Texture);
            int32 ExteriorTriangles = 0;
            for (const FStaticMeshSection& Section : LOD.Sections)
                if (Section.MaterialIndex == 15) ExteriorTriangles += Section.NumTriangles;
            const bool bExteriorReady = bExteriorSlot && ExteriorTriangles > 0 && ExteriorAtlas &&
                ExteriorAtlas->GetSizeX() == 1024 && ExteriorAtlas->GetSizeY() == 1024 && ExteriorAtlas->GetResource();
            Check(TEXT("Tu22 exterior slot 15 uses its original 1024 square atlas with a live render resource"), bExteriorReady);
            Observations.Add(FString::Printf(TEXT("{\"case\":\"Tu22_exterior_atlas\",\"material_slot\":15,\"native_material\":%s,\"native_texture\":%s,\"triangles\":%d,\"width\":%d,\"height\":%d,\"verified_live_original_atlas\":%s}"),
                *AirbaseJsonString(ExteriorMaterial ? ExteriorMaterial->GetPathName() : TEXT("missing")),
                *AirbaseJsonString(ExteriorAtlas ? ExteriorAtlas->GetPathName() : TEXT("missing")), ExteriorTriangles,
                ExteriorAtlas ? ExteriorAtlas->GetSizeX() : 0, ExteriorAtlas ? ExteriorAtlas->GetSizeY() : 0, bExteriorReady ? TEXT("true") : TEXT("false")));
            Observations.Add(TEXT("{\"case\":\"Tu22\",\"texture_resolution_limit\":\"Original 256-1024 pixel color textures; exterior atlas 1024 square. Source supplies no normal or ORM maps. This bomber is not certified as high-resolution PBR or AAA art.\",\"required_source_dimension\":1024}"));
        }
        Observations.Add(FString::Printf(TEXT("{\"case\":%s,\"mesh\":%s,\"fallback_section_textured_triangle_fraction\":%.6f,\"high_resolution_texture_uses\":%d,\"nanite_usage_valid\":%s,\"materials\":[%s]}"),
            *AirbaseJsonString(Label), *AirbaseJsonString(Mesh->GetPathName()), Coverage, HighResolutionTextures, bNaniteMaterials ? TEXT("true") : TEXT("false"), *FString::Join(MaterialRows, TEXT(","))));
    }

    void AfterViewportRendered(FViewport* Viewport)
    {
        if (!bReadbackRequested || bFinished || !GEngine || !GEngine->GameViewport || Viewport != GEngine->GameViewport->Viewport) return;
        bReadbackRequested = false;
        bReadbackReady = true;
        ViewSize = Viewport->GetSizeXY();
        if (ViewSize.X < 1280 || ViewSize.Y < 720 || !Viewport->ReadPixels(PendingPixels) || PendingPixels.Num() != ViewSize.X * ViewSize.Y)
        { ReadbackError = TEXT("The actual viewport readback was invalid."); return; }
        int32 NonBlack = 0, Different = 0;
        const FColor First = PendingPixels[0];
        for (const FColor& Pixel : PendingPixels)
        {
            if (FMath::Max3(Pixel.R, Pixel.G, Pixel.B) >= 8) ++NonBlack;
            if (FMath::Max3(FMath::Abs(int32(Pixel.R) - First.R), FMath::Abs(int32(Pixel.G) - First.G), FMath::Abs(int32(Pixel.B) - First.B)) >= 8) ++Different;
        }
        if (NonBlack <= PendingPixels.Num() / 4 || Different <= PendingPixels.Num() / 100)
            ReadbackError = TEXT("A blank or constant viewport cannot substantiate imported art visibility.");
    }

    bool BuildRegion()
    {
        FVector2D Minimum(DBL_MAX, DBL_MAX), Maximum(-DBL_MAX, -DBL_MAX);
        for (int32 Corner = 0; Corner < 8; ++Corner)
        {
            const FVector Point((Corner & 1) ? CaseBounds.Max.X : CaseBounds.Min.X, (Corner & 2) ? CaseBounds.Max.Y : CaseBounds.Min.Y, (Corner & 4) ? CaseBounds.Max.Z : CaseBounds.Min.Z);
            FVector2D Screen;
            if (!PC->ProjectWorldLocationToScreen(Point, Screen)) return false;
            Minimum.X = FMath::Min(Minimum.X, Screen.X); Minimum.Y = FMath::Min(Minimum.Y, Screen.Y);
            Maximum.X = FMath::Max(Maximum.X, Screen.X); Maximum.Y = FMath::Max(Maximum.Y, Screen.Y);
        }
        if (Minimum.X < 4 || Minimum.Y < 4 || Maximum.X >= ViewSize.X - 4 || Maximum.Y >= ViewSize.Y - 4) return false;
        Region = FIntRect(FMath::FloorToInt(Minimum.X) - 3, FMath::FloorToInt(Minimum.Y) - 3, FMath::CeilToInt(Maximum.X) + 3, FMath::CeilToInt(Maximum.Y) + 3);
        return Region.Area() >= 5000;
    }

    void CompareFrames()
    {
        const auto Delta = [this](int32 X, int32 Y)
        {
            const FColor& A = VisiblePixels[Y * ViewSize.X + X];
            const FColor& B = BaselinePixels[Y * ViewSize.X + X];
            return FMath::Max3(FMath::Abs(int32(A.R) - B.R), FMath::Abs(int32(A.G) - B.G), FMath::Abs(int32(A.B) - B.B));
        };
        int32 Changed = 0, Peak = 0;
        double Sum = 0., ControlSum = 0.;
        for (int32 Y = Region.Min.Y; Y < Region.Max.Y; ++Y)
            for (int32 X = Region.Min.X; X < Region.Max.X; ++X)
            {
                const int32 Difference = Delta(X, Y);
                Sum += Difference; Peak = FMath::Max(Peak, Difference);
                if (Difference >= 12) ++Changed;
            }
        const FIntRect Control(8, 8, 72, 72);
        const bool bSeparate = Control.Max.X <= Region.Min.X || Control.Max.Y <= Region.Min.Y;
        for (int32 Y = Control.Min.Y; Y < Control.Max.Y; ++Y)
            for (int32 X = Control.Min.X; X < Control.Max.X; ++X) ControlSum += Delta(X, Y);
        const double Mean = Sum / Region.Area(), ControlMean = ControlSum / Control.Area();
        Check(CaseName() + TEXT(" imported geometry visibly changes the actual viewport beyond background variation"), bSeparate && Changed >= 1000 && Peak >= 30 && Mean > ControlMean + .5);
        Observations.Add(FString::Printf(TEXT("{\"case\":%s,\"roi\":[%d,%d,%d,%d],\"changed_pixels_at_12\":%d,\"peak_difference\":%d,\"mean_difference\":%.6f,\"control_mean_difference\":%.6f,\"remaining_shader_jobs\":%d}"),
            *AirbaseJsonString(CaseName()), Region.Min.X, Region.Min.Y, Region.Max.X, Region.Max.Y, Changed, Peak, Mean, ControlMean, RemainingShaderJobs));
    }

    void RemoveDelegates()
    {
        if (PreTickHandle.IsValid()) FWorldDelegates::OnWorldPreActorTick.Remove(PreTickHandle);
        if (PostTickHandle.IsValid()) FWorldDelegates::OnWorldPostActorTick.Remove(PostTickHandle);
        if (ViewportHandle.IsValid()) UGameViewportClient::OnViewportRendered().Remove(ViewportHandle);
        PreTickHandle.Reset(); PostTickHandle.Reset(); ViewportHandle.Reset();
        bReadbackRequested = false;
    }

    bool Finish(const TCHAR* Error = nullptr)
    {
        bFinished = true;
        RemoveDelegates();
        if (Error) Test->AddError(Error);
        SetCaseVisible(true);
        if (bStateCaptured && Mode.IsValid())
        {
            if (Input.IsValid() && Mode->Player) Input->InjectInputForAction(Mode->Player->GunAction, FInputActionValue(false), {}, {});
            ClearEnemies();
            Mode->ReturnToMenu();
            Mode->SelectAircraft(OriginalSelection);
            Mode->SetActorTickEnabled(bOriginalModeTick);
            if (Mode->Player)
            {
                Mode->Player->ClearFlightInput();
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
            const FString Report = FString::Printf(TEXT("{\"schema\":\"airbase-art-acceptance/1\",\"status\":\"%s\",\"fixture\":\"real DesertDefense map and normal menu, flight, input and weapon entry points\",\"visual_evidence\":\"numeric in-memory visibility comparisons and actual rendered material resources; no human image review or AAA certification\",\"media_files_written\":0,\"viewport\":[%d,%d],\"checks\":[%s],\"observations\":[%s]}\n"),
                Test->HasAnyErrors() ? TEXT("failed") : TEXT("passed"), ViewSize.X, ViewSize.Y, *FString::Join(Checks, TEXT(",")), *FString::Join(Observations, TEXT(",")));
            Test->TestTrue(TEXT("Numeric imported airbase acceptance report saved"), FFileHelper::SaveStringToFile(Report, *(OutputDir / TEXT("report.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
            Test->AddInfo(TEXT("Airbase art acceptance JSON: ") + OutputDir);
        }
        return true;
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDSAirbaseArtAcceptanceTest, "DuneSentinel.Rendering.AirbaseArtAcceptance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FDSAirbaseArtAcceptanceTest::RunTest(const FString& Parameters)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("DSAirbaseArtValidation")) ||
        FParse::Param(FCommandLine::Get(), TEXT("AutoPlay")) || FParse::Param(FCommandLine::Get(), TEXT("CaptureFrames")))
    {
        AddError(TEXT("Use dedicated -game -DSAirbaseArtValidation without AutoPlay or CaptureFrames on DesertDefense."));
        return false;
    }
    ADD_LATENT_AUTOMATION_COMMAND(FDSAirbaseArtAcceptanceCommand(this));
    return true;
}
#endif
