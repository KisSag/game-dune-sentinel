#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "DSCannonFXComponent.h"
#include "DSEnemyAircraft.h"
#include "DSFlightPawn.h"
#include "DSGameMode.h"
#include "DSProjectile.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
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
// This fixture uses the shipped firing paths in the actual map. It is never an
// AutoPlay route and is available only in development automation with opt-in.
class FDSCannonFXRenderCommand final : public IAutomationLatentCommand
{
public:
    explicit FDSCannonFXRenderCommand(FAutomationTestBase* InTest) : Test(InTest) {}
    virtual ~FDSCannonFXRenderCommand() override { RemoveWorldDelegates(); }

    virtual bool Update() override
    {
        if (bFinished) return true;
        if (GetCurrentRunTime() > 90.0)
            return Finish(TEXT("Cannon visual acceptance exceeded its 90-second test timeout."));
        if (!bInitialized)
        {
            UWorld* GameWorld = nullptr;
            for (const FWorldContext& Context : GEngine->GetWorldContexts())
                if (Context.WorldType == EWorldType::Game && Context.World() && Context.World()->HasBegunPlay())
                    GameWorld = Context.World();
            if (!GameWorld || !GEngine->GameViewport || !GEngine->GameViewport->Viewport)
                return GetCurrentRunTime() > 20.0 ? Finish(TEXT("No rendered game world became ready.")) : false;
            World = GameWorld;
            Mode = GameWorld->GetAuthGameMode<ADSGameMode>();
            PC = UGameplayStatics::GetPlayerController(GameWorld, 0);
            if (!Mode.IsValid() || !PC.IsValid() || !Mode->Player || Mode->Aircraft.Num() != 3)
                return Finish(TEXT("The DesertDefense map must provide its real mode, player and three airframes."));
            if (!PC->GetLocalPlayer()) return Finish(TEXT("A local player is required for real Enhanced Input."));
            Input = PC->GetLocalPlayer()->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>();
            if (!Input.IsValid()) return Finish(TEXT("Enhanced Input subsystem is unavailable."));
            OriginalSelection = Mode->SelectedAircraft;
            bOriginalModeTick = Mode->IsActorTickEnabled();
            bOriginalAutoCamera = PC->bAutoManageActiveCameraTarget;
            OriginalViewTarget = PC->GetViewTarget();
            OutputDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("CannonFX") /
                (FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")) + TEXT("-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8)));
            IFileManager::Get().MakeDirectory(*OutputDir, true);
            ReviewCamera = GameWorld->SpawnActor<ACameraActor>();
            if (!ReviewCamera.IsValid()) return Finish(TEXT("Could not create the review camera."));
            ReviewCamera->GetCameraComponent()->SetFieldOfView(65.f);
            PC->bAutoManageActiveCameraTarget = false;
            // No waves, score accumulation or bot decisions during the isolated
            // fixture. Flight, enemy AI, projectiles and effects tick normally.
            Mode->SetActorTickEnabled(false);
            bInitialized = true;
            if (!BeginCase()) return Finish(TEXT("Could not prepare the first cannon case."));
            PreActorTickHandle = FWorldDelegates::OnWorldPreActorTick.AddRaw(this, &FDSCannonFXRenderCommand::BeforeActorTick);
            PostActorTickHandle = FWorldDelegates::OnWorldPostActorTick.AddRaw(this, &FDSCannonFXRenderCommand::AfterActorTick);
        }
        return false;
    }

private:
    bool AdvanceScenario()
    {
        if (!World.IsValid() || !Mode.IsValid() || !PC.IsValid() || !Shooter.IsValid() || !FX.IsValid())
            return Finish(TEXT("A required gameplay actor disappeared during cannon validation."));

        FollowReviewCamera();
        // The review camera follows actors after their actual movement. Refresh
        // its view cache before this frame is rendered, rather than filming the
        // previous frame's camera while a screenshot readback stalls simulation.
        if (bSideView && PC->PlayerCameraManager) PC->PlayerCameraManager->UpdateCamera(0.f);
        const float Now = World->GetTimeSeconds();
        SampleProjectiles(Now);
        MaxFlash = FMath::Max(MaxFlash, FX->GetActiveFlashCount());
        MaxSmoke = FMath::Max(MaxSmoke, FX->GetActiveSmokeCount());
        if (FirstShotComponents >= 0)
            PeakComponents = FMath::Max(PeakComponents, Shooter->GetComponents().Num());

        if (Phase == EPhase::Warmup)
        {
            if (Now - PhaseStarted < 1.5f) return false; // normal sortie fade and render settling
            if (CaseIndex >= 3 && !CreateEnemy())
                return Finish(TEXT("Could not prepare the real enemy cannon."));
            Phase = EPhase::Fire;
            PhaseStarted = Now;
            ShotBaseline = FX->GetShotCount();
            AmmoBaseline = Mode->Player->CannonAmmo;
            return false;
        }

        if (Phase == EPhase::Fire)
        {
            if (Now - PhaseStarted > 9.f)
                return Finish(TEXT("Normal fire did not yield two rendered evidence frames in time."));
            const uint64 Shots = FX->GetShotCount() - ShotBaseline;
            if (Shots > 0 && FirstShotComponents < 0)
                FirstShotComponents = PeakComponents = Shooter->GetComponents().Num();
            if (!PendingScreenshot.IsEmpty())
            {
                if (!FScreenshotRequest::IsScreenshotRequested() && IFileManager::Get().FileSize(*PendingScreenshot) > 1024)
                {
                    ScreenshotNames.Add(FPaths::GetCleanFilename(PendingScreenshot));
                    PendingScreenshot.Empty();
                    if (CaptureStep == 1)
                    {
                        bSideView = true;
                        SecondCaptureAfter = Now + .35f;
                        SideViewReadyFrame = GFrameCounter + 3;
                        CaptureStep = 2;
                    }
                    else
                    {
                        if (Enemy.IsValid()) Enemy->SetActorTickEnabled(false);
                        Phase = EPhase::Drain;
                        PhaseStarted = Now;
                    }
                }
                else if (FPlatformTime::Seconds() - ScreenshotRequestedAt > 8.0)
                    return Finish(TEXT("The viewport did not write the requested screenshot."));
                return false;
            }
            const uint64 WarmShots = CaseIndex < 3 ? 12 : 3;
            if (CaptureStep == 0 && Now - PhaseStarted >= 1.2f && Shots >= WarmShots &&
                FX->GetActiveFlashCount() > 0 && FX->GetActiveSmokeCount() >= 2 && CurrentVisibleTrails > 0)
            {
                FirstCaptureShots = Shots;
                RequestFrame(TEXT("flash"));
                CaptureStep = 1;
            }
            else if (CaptureStep == 2 && Now >= SecondCaptureAfter && GFrameCounter >= SideViewReadyFrame &&
                     Shots >= FirstCaptureShots + (CaseIndex < 3 ? 4 : 2) &&
                     FX->GetActiveFlashCount() > 0 && FX->GetActiveSmokeCount() >= 2 && CurrentVisibleTrails > 0)
            {
                RequestFrame(TEXT("smoke_trail"));
                CaptureStep = 3;
            }
            return false;
        }

        // Stop the real input/AI and allow the longest cannon lifetime (1.6 s),
        // smoke (0.65 s), and pending input frame to expire without manual resets.
        if (Now - PhaseStarted < 2.2f) return false;
        CompleteCase();
        ++CaseIndex;
        if (CaseIndex == 5) return Finish();
        if (!BeginCase()) return Finish(TEXT("Could not prepare the next cannon case."));
        return false;
    }

    enum class EPhase { Warmup, Fire, Drain };
    struct FProjectileSample
    {
        FVector Position;
        float Distance;
        float Time;
    };
    FAutomationTestBase* Test;
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<ADSGameMode> Mode;
    TWeakObjectPtr<APlayerController> PC;
    TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> Input;
    TWeakObjectPtr<AActor> Shooter;
    TWeakObjectPtr<UDSCannonFXComponent> FX;
    TWeakObjectPtr<ADSEnemyAircraft> Enemy;
    TWeakObjectPtr<ACameraActor> ReviewCamera;
    TWeakObjectPtr<AActor> OriginalViewTarget;
    TMap<TWeakObjectPtr<ADSProjectile>, FProjectileSample> ProjectileSamples;
    TArray<FString> CaseReports;
    TArray<FString> ScreenshotNames;
    TArray<FString> CaptureObservations;
    FDelegateHandle PreActorTickHandle, PostActorTickHandle;
    FString OutputDir, CaseName, PendingScreenshot;
    EPhase Phase = EPhase::Warmup;
    int32 CaseIndex = 0, OriginalSelection = 0, CaptureStep = 0, AmmoBaseline = 0;
    int32 MaxFlash = 0, MaxSmoke = 0, FirstShotComponents = -1, PeakComponents = 0;
    int32 MotionSamples = 0, TrailSamples = 0, CurrentVisibleTrails = 0;
    uint64 ShotBaseline = 0, FirstCaptureShots = 0, SideViewReadyFrame = 0;
    float PhaseStarted = 0.f, SecondCaptureAfter = 0.f;
    double ScreenshotRequestedAt = 0.0;
    float MaxTrailHeadError = 0.f, MaxTravelError = 0.f, MaxSpeedRelativeError = 0.f;
    bool bInitialized = false, bFinished = false, bOriginalModeTick = true, bOriginalAutoCamera = true, bSideView = false;
    bool bTrailGeometryValid = true;

    void BeforeActorTick(UWorld* TickWorld, ELevelTick, float)
    {
        if (TickWorld == World.Get() && !bFinished)
            InjectGun(Phase == EPhase::Fire && CaseIndex < 3);
    }

    void AfterActorTick(UWorld* TickWorld, ELevelTick, float)
    {
        if (TickWorld == World.Get() && !bFinished) bFinished = AdvanceScenario();
    }

    void RemoveWorldDelegates()
    {
        if (PreActorTickHandle.IsValid()) FWorldDelegates::OnWorldPreActorTick.Remove(PreActorTickHandle);
        if (PostActorTickHandle.IsValid()) FWorldDelegates::OnWorldPostActorTick.Remove(PostActorTickHandle);
        PreActorTickHandle.Reset();
        PostActorTickHandle.Reset();
    }

    void InjectGun(bool bHeld)
    {
        if (Input.IsValid() && Mode.IsValid() && Mode->Player && Mode->Player->GunAction)
            Input->InjectInputForAction(Mode->Player->GunAction, FInputActionValue(bHeld), {}, {});
    }

    bool BeginCase()
    {
        Mode->ReturnToMenu();
        Mode->SelectAircraft(CaseIndex < 3 ? CaseIndex : 1);
        Mode->StartSortie();
        // Replace randomly spawned opponents only in this explicit development
        // fixture. No spawn, targeting, ammo, damage or input rules are changed.
        for (TActorIterator<ADSEnemyAircraft> It(World.Get()); It; ++It) It->Destroy();
        Mode->Enemies.Reset();
        Mode->RadioLine.Empty();
        Mode->Player->SetActorLocation(FVector(-200000.f, 0.f, 120000.f));
        Mode->Player->SetActorRotation(FRotator::ZeroRotator);
        Mode->Player->ClearFlightInput();
        Enemy.Reset();
        Shooter = Mode->Player;
        FX = Shooter->FindComponentByClass<UDSCannonFXComponent>();
        if (!FX.IsValid() || !Mode->Player->GunAction) return false;
        CaseName = CaseIndex < 3 ? FString::Printf(TEXT("player_%d"), CaseIndex) :
            (CaseIndex == 3 ? TEXT("enemy_fighter") : TEXT("bomber_defensive_gun"));
        CaptureStep = MaxFlash = MaxSmoke = MotionSamples = TrailSamples = CurrentVisibleTrails = 0;
        FirstCaptureShots = SideViewReadyFrame = 0;
        FirstShotComponents = -1;
        PeakComponents = 0;
        MaxTrailHeadError = MaxTravelError = MaxSpeedRelativeError = 0.f;
        bTrailGeometryValid = true;
        ProjectileSamples.Reset();
        PendingScreenshot.Empty();
        Phase = EPhase::Warmup;
        PhaseStarted = World->GetTimeSeconds();
        bSideView = CaseIndex >= 3;
        PC->SetViewTarget(CaseIndex < 3 ? static_cast<AActor*>(Mode->Player.Get()) : ReviewCamera.Get());
        return true;
    }

    bool CreateEnemy()
    {
        const FVector PlayerLocation = Mode->Player->GetActorLocation();
        const FVector Position = PlayerLocation - FVector(25000.f, 3500.f, 0.f);
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        Enemy = World->SpawnActor<ADSEnemyAircraft>(Position, (PlayerLocation - Position).Rotation(), Params);
        if (!Enemy.IsValid()) return false;
        // A distant objective avoids the bombing zone. Initial gun cooldown,
        // tracking, spread, projectile speed and damage all retain normal rules.
        Enemy->Initialize(CaseIndex == 4, Position + FVector(2000000.f, 0.f, -65000.f));
        Mode->Enemies.Add(Enemy.Get());
        Shooter = Enemy.Get();
        FX = Shooter->FindComponentByClass<UDSCannonFXComponent>();
        PC->SetViewTarget(ReviewCamera.Get());
        return FX.IsValid();
    }

    void FollowReviewCamera()
    {
        if (!bSideView || !ReviewCamera.IsValid() || !Shooter.IsValid()) return;
        PC->SetViewTarget(ReviewCamera.Get());
        const FTransform Pose = Shooter->GetActorTransform();
        const FVector Offset = CaptureStep < 2 ? FVector(-1600.f, -3500.f, 900.f) : FVector(-3500.f, -7500.f, 2400.f);
        const FVector LookAt = Pose.TransformPosition(FVector(CaptureStep < 2 ? 1000.f : 2000.f, 0.f, 0.f));
        const FVector CameraPosition = Pose.TransformPosition(Offset);
        ReviewCamera->SetActorLocationAndRotation(CameraPosition, (LookAt - CameraPosition).Rotation());
    }

    void SampleProjectiles(float Now)
    {
        CurrentVisibleTrails = 0;
        for (TActorIterator<ADSProjectile> It(World.Get()); It; ++It)
        {
            ADSProjectile* Shot = *It;
            if (Shot->GetOwner() != Shooter.Get() || Shot->IsGuided()) continue;
            const TWeakObjectPtr<ADSProjectile> Key(Shot);
            const FVector Position = Shot->GetActorLocation();
            const float Distance = Shot->GetDistanceTravelledCm();
            if (const FProjectileSample* Previous = ProjectileSamples.Find(Key))
            {
                const float Dt = Now - Previous->Time;
                const float Travel = FVector::Distance(Position, Previous->Position);
                if (Dt > SMALL_NUMBER && Previous->Distance > 1.f && Travel > 1.f)
                {
                    ++MotionSamples;
                    MaxTravelError = FMath::Max(MaxTravelError, FMath::Abs(Travel - (Distance - Previous->Distance)));
                    const float Expected = Shot->GetSpeedMps() * 100.f * Dt;
                    MaxSpeedRelativeError = FMath::Max(MaxSpeedRelativeError, FMath::Abs(Travel - Expected) / FMath::Max(Expected, 1.f));
                }
            }
            ProjectileSamples.Add(Key, { Position, Distance, Now });
            if (Distance <= 1.f) continue;
            ++TrailSamples;
            const int32 Planes = Shot->GetVisibleCannonTrailPlaneCount();
            if (Planes > 0) ++CurrentVisibleTrails;
            const FVector Head = Shot->GetCannonTrailHeadLocation();
            const FVector Tail = Shot->GetCannonTrailTailLocation();
            const float Length = Shot->GetCannonTrailLengthCm();
            MaxTrailHeadError = FMath::Max(MaxTrailHeadError, FVector::Distance(Head, Position));
            bTrailGeometryValid &= Planes == 3 && Length > 0.f && Length <= Distance + 2.f && Length <= 4202.f &&
                FVector::DotProduct((Head - Tail).GetSafeNormal(), Shot->GetTravelDirection()) > .999f &&
                FMath::Abs(FVector::Distance(Head, Tail) - Length) < 2.f;
        }
    }

    void RequestFrame(const TCHAR* Moment)
    {
        PendingScreenshot = OutputDir / (CaseName + TEXT("_") + Moment + TEXT(".png"));
        ScreenshotRequestedAt = FPlatformTime::Seconds();
        CaptureObservations.Add(FString::Printf(TEXT("{\"file\":\"%s\",\"world_time_s\":%.6f,\"render_frame\":%llu,\"shots\":%llu,\"flash\":%d,\"smoke\":%d,\"visible_trails\":%d,\"view\":\"%s\",\"phase\":\"post_actor_tick\"}"),
            *FPaths::GetCleanFilename(PendingScreenshot), World->GetTimeSeconds(), static_cast<unsigned long long>(GFrameCounter),
            static_cast<unsigned long long>(FX->GetShotCount() - ShotBaseline), FX->GetActiveFlashCount(),
            FX->GetActiveSmokeCount(), CurrentVisibleTrails, bSideView ? TEXT("side") : TEXT("normal_chase")));
        FScreenshotRequest::RequestScreenshot(PendingScreenshot, false, false);
    }

    void CompleteCase()
    {
        int32 LiveCannonProjectiles = 0;
        for (TActorIterator<ADSProjectile> It(World.Get()); It; ++It)
            if (It->GetOwner() == Shooter.Get() && !It->IsGuided()) ++LiveCannonProjectiles;
        const uint64 Shots = FX->GetShotCount() - ShotBaseline;
        const int32 FlashAfter = FX->GetActiveFlashCount(), SmokeAfter = FX->GetActiveSmokeCount();
        const int32 ComponentsAfter = Shooter->GetComponents().Num();
        Test->TestTrue(CaseName + TEXT(": real firing sustained both evidence frames"), Shots >= static_cast<uint64>(CaseIndex < 3 ? 16 : 5));
        if (CaseIndex < 3) Test->TestTrue(CaseName + TEXT(": real firing consumed ammunition"), Mode->Player->CannonAmmo < AmmoBaseline);
        Test->TestTrue(CaseName + TEXT(": flash and smoke activated"), MaxFlash > 0 && MaxSmoke > 0 && MaxSmoke <= 16);
        Test->TestTrue(CaseName + TEXT(": trail planes follow real projectile geometry"), TrailSamples > 0 && bTrailGeometryValid && MaxTrailHeadError < 2.f);
        Test->TestTrue(CaseName + TEXT(": real projectile travel matches velocity"), MotionSamples > 0 && MaxTravelError < 2.f && MaxSpeedRelativeError < .03f);
        Test->TestTrue(CaseName + TEXT(": flash and smoke naturally expire"), FlashAfter == 0 && SmokeAfter == 0);
        Test->TestTrue(CaseName + TEXT(": cannon projectiles naturally expire"), LiveCannonProjectiles == 0);
        Test->TestTrue(CaseName + TEXT(": firing reuses a stable component pool"), FirstShotComponents >= 0 &&
            PeakComponents == FirstShotComponents && ComponentsAfter == FirstShotComponents);
        CaseReports.Add(FString::Printf(TEXT("{\"case\":\"%s\",\"shots\":%llu,\"max_flash\":%d,\"max_smoke\":%d,\"motion_samples\":%d,\"trail_samples\":%d,\"max_trail_head_error_cm\":%.6f,\"max_travel_error_cm\":%.6f,\"max_speed_relative_error\":%.6f,\"trail_geometry_valid\":%s,\"flash_after\":%d,\"smoke_after\":%d,\"projectiles_after\":%d,\"components_first_shot\":%d,\"components_peak\":%d,\"components_after\":%d}"),
            *CaseName, static_cast<unsigned long long>(Shots), MaxFlash, MaxSmoke, MotionSamples, TrailSamples,
            MaxTrailHeadError, MaxTravelError, MaxSpeedRelativeError, bTrailGeometryValid ? TEXT("true") : TEXT("false"),
            FlashAfter, SmokeAfter, LiveCannonProjectiles, FirstShotComponents, PeakComponents, ComponentsAfter));
    }

    bool Finish(const TCHAR* Error = nullptr)
    {
        RemoveWorldDelegates();
        if (Error) Test->AddError(Error);
        InjectGun(false);
        if (bInitialized && Mode.IsValid())
        {
            Mode->ReturnToMenu();
            Mode->SelectAircraft(OriginalSelection);
            Mode->SetActorTickEnabled(bOriginalModeTick);
        }
        if (PC.IsValid())
        {
            PC->bAutoManageActiveCameraTarget = bOriginalAutoCamera;
            if (OriginalViewTarget.IsValid()) PC->SetViewTarget(OriginalViewTarget.Get());
        }
        if (ReviewCamera.IsValid()) ReviewCamera->Destroy();
        Test->TestEqual(TEXT("All five real firing cases completed"), CaseReports.Num(), 5);
        Test->TestEqual(TEXT("Ten actual viewport screenshots were saved"), ScreenshotNames.Num(), 10);
        if (!OutputDir.IsEmpty())
        {
            TArray<FString> Images;
            for (const FString& Name : ScreenshotNames) Images.Add(TEXT("\"") + Name + TEXT("\""));
            const FString Report = FString::Printf(TEXT("{\"schema\":\"cannon-fx-acceptance/1\",\"status\":\"%s\",\"checks_scope\":\"mechanical_only\",\"visual_review\":\"required\",\"fixture\":\"development automation using normal firing paths; no AutoPlay\",\"cases\":[%s],\"screenshots\":[%s],\"capture_observations\":[%s]}\n"),
                Test->HasAnyErrors() ? TEXT("failed") : TEXT("passed"), *FString::Join(CaseReports, TEXT(",")),
                *FString::Join(Images, TEXT(",")), *FString::Join(CaptureObservations, TEXT(",")));
            Test->TestTrue(TEXT("Acceptance report saved"), FFileHelper::SaveStringToFile(Report, *(OutputDir / TEXT("report.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
            Test->AddInfo(TEXT("Cannon visual evidence: ") + OutputDir);
        }
        return true;
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDSCannonFXRenderTest, "DuneSentinel.Rendering.CannonFX",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FDSCannonFXRenderTest::RunTest(const FString& Parameters)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("DSCannonFXValidation")) ||
        FParse::Param(FCommandLine::Get(), TEXT("AutoPlay")))
    {
        AddError(TEXT("Use a dedicated -game -DSCannonFXValidation launch without -AutoPlay on DesertDefense."));
        return false;
    }
    ADD_LATENT_AUTOMATION_COMMAND(FDSCannonFXRenderCommand(this));
    return true;
}
#endif
