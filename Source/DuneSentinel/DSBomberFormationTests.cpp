#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "DSEnemyAircraft.h"
#include "DSFlightPawn.h"
#include "DSGameMode.h"
#include "DSProjectile.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

namespace
{
// Explicit development acceptance only. All formations originate in SpawnWave;
// neither their positions nor their flight AI are driven by this fixture.
class FDSBomberFormationRenderCommand final : public IAutomationLatentCommand
{
public:
    explicit FDSBomberFormationRenderCommand(FAutomationTestBase* InTest) : Test(InTest),
        bNoMedia(FParse::Param(FCommandLine::Get(), TEXT("DSNoMedia"))) {}
    virtual ~FDSBomberFormationRenderCommand() override { RemoveDelegate(); }

    virtual bool Update() override
    {
        if (bFinished) return true;
        if (GetCurrentRunTime() > 120.0)
        {
            bFinished = Finish(TEXT("Bomber formation acceptance exceeded its 120-second wall-clock limit."));
            return true;
        }
        if (bInitialized) return false;
        UWorld* GameWorld = nullptr;
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
            if (Context.WorldType == EWorldType::Game && Context.World() && Context.World()->HasBegunPlay())
                GameWorld = Context.World();
        if (!GameWorld || !GEngine->GameViewport || !GEngine->GameViewport->Viewport)
            return false;
        World = GameWorld;
        Mode = World->GetAuthGameMode<ADSGameMode>();
        PC = UGameplayStatics::GetPlayerController(World.Get(), 0);
        if (!Mode.IsValid() || !PC.IsValid() || !Mode->Player)
        {
            bFinished = Finish(TEXT("DesertDefense must provide the real game mode, local controller and player."));
            return true;
        }
        OriginalSelection = Mode->SelectedAircraft;
        bOriginalModeTick = Mode->IsActorTickEnabled();
        bOriginalPlayerTick = Mode->Player->IsActorTickEnabled();
        bOriginalAutoCamera = PC->bAutoManageActiveCameraTarget;
        OriginalViewTarget = PC->GetViewTarget();
        OutputDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Validation/BomberFormation") /
            (FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")) + TEXT("-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8)));
        IFileManager::Get().MakeDirectory(*OutputDir, true);
        ReviewCamera = World->SpawnActor<ACameraActor>();
        if (!ReviewCamera.IsValid())
        {
            bFinished = Finish(TEXT("Could not spawn the observation camera."));
            return true;
        }
        ReviewCamera->GetCameraComponent()->SetFieldOfView(58.f);
        Mode->SetActorTickEnabled(false);
        Mode->Player->SetActorTickEnabled(false);
        PC->bAutoManageActiveCameraTarget = false;
        bInitialized = true;
        SimulationStarted = World->GetTimeSeconds();
        const FIntPoint Size = GEngine->GameViewport->Viewport->GetSizeXY();
        Check(TEXT("Rendered viewport is 1920 by 1080"), Size == FIntPoint(1920, 1080));
        ValidateCountAndLifecycle();
        if (!BeginFlightCase(3))
        {
            bFinished = Finish(TEXT("The real spawn path did not create three bombers."));
            return true;
        }
        PostActorTickHandle = FWorldDelegates::OnWorldPostActorTick.AddRaw(this, &FDSBomberFormationRenderCommand::AfterActorTick);
        return false;
    }

private:
    enum class EPhase { ThreeFlight, LeaderLoss, FiveFlight, Released, Bombing, Damage };
    FAutomationTestBase* Test;
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<ADSGameMode> Mode;
    TWeakObjectPtr<APlayerController> PC;
    TWeakObjectPtr<ACameraActor> ReviewCamera;
    TWeakObjectPtr<AActor> OriginalViewTarget;
    TArray<TWeakObjectPtr<ADSEnemyAircraft>> Group;
    TMap<TWeakObjectPtr<ADSEnemyAircraft>, FVector> PreviousLocations;
    TMap<TWeakObjectPtr<ADSEnemyAircraft>, FVector> SurvivorStarts;
    TSet<TWeakObjectPtr<ADSBomb>> ObservedBombs;
    TWeakObjectPtr<ADSBomb> FirstBomb;
    TArray<FString> Checks, Images, Captures, CapacityReports;
    FDelegateHandle PostActorTickHandle;
    FString OutputDir, PendingScreenshot;
    EPhase Phase = EPhase::ThreeFlight;
    int32 OriginalSelection = 0, ExpectedCount = 0, FormationSamples = 0, MotionSamples = 0;
    int32 ThreeSamples = 0, FiveSamples = 0, FiveSampleBaseline = 0, CaptureStep = 0, ReleasedCount = 0, MaxBombsDropped = 0;
    float SimulationStarted = 0.f, PhaseStarted = 0.f, PreviousSampleTime = 0.f, ReleaseTime = 0.f;
    float InitialAirportHealth = 0.f, MaxSlotError = 0.f, MaxSymmetryError = 0.f, MaxMotionRelativeError = 0.f;
    float ReleaseDistance = 0.f, FirstBombTime = 0.f, MaxAttackLateralChange = 0.f;
    double ScreenshotRequestedAt = 0.0;
    uint64 CameraSettleFrame = 0;
    bool bInitialized = false, bFinished = false, bOriginalModeTick = true, bOriginalPlayerTick = true, bOriginalAutoCamera = true;
    bool bEverReformed = false, bEarlyBomb = false, bPartialRelease = false;
    const bool bNoMedia;

    void Check(const FString& Name, bool bPassed)
    {
        Test->TestTrue(Name, bPassed);
        Checks.Add(FString::Printf(TEXT("{\"name\":\"%s\",\"passed\":%s}"), *Name, bPassed ? TEXT("true") : TEXT("false")));
    }

    void RemoveDelegate()
    {
        if (PostActorTickHandle.IsValid()) FWorldDelegates::OnWorldPostActorTick.Remove(PostActorTickHandle);
        PostActorTickHandle.Reset();
    }

    void ClearHostiles()
    {
        for (TActorIterator<ADSEnemyAircraft> It(World.Get()); It; ++It) It->Destroy();
        for (TActorIterator<ADSProjectile> It(World.Get()); It; ++It) It->Destroy();
        for (TActorIterator<ADSBomb> It(World.Get()); It; ++It) It->Destroy();
        Mode->Enemies.Reset();
    }

    TArray<ADSEnemyAircraft*> Bombers() const
    {
        TArray<ADSEnemyAircraft*> Result;
        for (ADSEnemyAircraft* Enemy : Mode->Enemies)
            if (IsValid(Enemy) && !Enemy->bDying && Enemy->bBomber) Result.Add(Enemy);
        Result.Sort([](const ADSEnemyAircraft& A, const ADSEnemyAircraft& B) { return A.FormationSlot < B.FormationSlot; });
        return Result;
    }

    void SpawnAtCapacity(int32 Capacity)
    {
        Mode->ReturnToMenu();
        Mode->StartSortie();
        ClearHostiles();
        // Stationary, distant occupancy actors exercise the real 24-enemy cap.
        // They are not bombers, not formation members and never appear on camera.
        for (int32 I = Capacity; I < 24; ++I)
        {
            ADSEnemyAircraft* Filler = World->SpawnActor<ADSEnemyAircraft>(FVector(3000000.f, I * 5000.f, 200000.f), FRotator::ZeroRotator);
            if (Filler)
            {
                Filler->SetActorTickEnabled(false);
                Mode->Enemies.Add(Filler);
            }
        }
        Mode->Player->SetActorLocation(FVector(2500000.f, 2500000.f, 200000.f));
        Mode->Player->ClearFlightInput();
        Mode->Wave = 6;
        Mode->Tick(300.1f); // Wave seven guarantees the maximum requested bomber group.
    }

    void ValidateCountAndLifecycle()
    {
        bool bCountPolicyValid = true;
        for (int32 Requested = 0; Requested <= 9; ++Requested)
            for (int32 Capacity = 0; Capacity <= 25; ++Capacity)
            {
                const int32 Actual = ADSGameMode::GetBomberCountForCapacity(Requested, Capacity);
                int32 Expected = FMath::Min3(Requested, Capacity, 5);
                if (Expected > 0 && Expected % 2 == 0) --Expected;
                bCountPolicyValid &= Actual == Expected;
            }
        Check(TEXT("Odd count selection preserves zero and rounds constrained requests down to one three or five"), bCountPolicyValid);
        Check(TEXT("Invalid negative request or capacity yields no bombers"),
            ADSGameMode::GetBomberCountForCapacity(-1, 24) == 0 && ADSGameMode::GetBomberCountForCapacity(5, -1) == 0);
        bool bRealCountsValid = true, bSlotsValid = true;
        const int32 Capacities[] = { 0, 1, 2, 3, 4, 5, 6, 23, 24 };
        for (int32 Capacity : Capacities)
        {
            SpawnAtCapacity(Capacity);
            const TArray<ADSEnemyAircraft*> Spawned = Bombers();
            const int32 Expected = ADSGameMode::GetBomberCountForCapacity(5, Capacity);
            bRealCountsValid &= Spawned.Num() == Expected && Mode->Enemies.Num() <= 24;
            TSet<int32> Slots;
            for (ADSEnemyAircraft* Bomber : Spawned)
            {
                Slots.Add(Bomber->FormationSlot);
                bSlotsValid &= Bomber->FormationCount == Expected && Bomber->bFormationActive == (Expected > 1);
            }
            if (Expected > 1) bSlotsValid &= Slots.Num() == Expected;
            CapacityReports.Add(FString::Printf(TEXT("{\"available\":%d,\"expected_bombers\":%d,\"actual_bombers\":%d,\"total_enemies\":%d}"),
                Capacity, Expected, Spawned.Num(), Mode->Enemies.Num()));
        }
        Check(TEXT("Actual waves respect odd bomber counts and the total enemy capacity at all boundaries"), bRealCountsValid);
        Check(TEXT("Actual spawning assigns unique slots and only multi-bomber groups hold formation"), bSlotsValid);

        SpawnAtCapacity(24);
        const TArray<ADSEnemyAircraft*> FirstGroup = Bombers();
        const int32 FirstGroupId = FirstGroup.IsEmpty() ? INDEX_NONE : FirstGroup[0]->FormationGroup;
        TArray<TWeakObjectPtr<ADSEnemyAircraft>> FirstReferences;
        for (ADSEnemyAircraft* Bomber : FirstGroup) FirstReferences.Add(Bomber);
        Mode->Tick(300.1f);
        int32 OldGroupCount = 0, NewGroupCount = 0;
        int32 SecondGroupId = INDEX_NONE;
        for (ADSEnemyAircraft* Bomber : Bombers())
        {
            if (Bomber->FormationGroup == FirstGroupId) ++OldGroupCount;
            else { ++NewGroupCount; SecondGroupId = Bomber->FormationGroup; }
        }
        Check(TEXT("Concurrent waves retain independent group identities and five members each"),
            OldGroupCount == 5 && NewGroupCount == 5 && SecondGroupId != INDEX_NONE && SecondGroupId != FirstGroupId);
        Mode->StartSortie();
        bool bOldDestroyed = true;
        for (const TWeakObjectPtr<ADSEnemyAircraft>& Bomber : FirstReferences)
            bOldDestroyed &= !Bomber.IsValid() || Bomber->IsActorBeingDestroyed();
        Check(TEXT("Restart removes previous formation actors and starts a fresh first wave"), bOldDestroyed && Mode->Wave == 1);
        Mode->ReturnToMenu();
        int32 LiveEnemies = 0, LiveBombs = 0;
        for (TActorIterator<ADSEnemyAircraft> It(World.Get()); It; ++It) if (!It->IsActorBeingDestroyed()) ++LiveEnemies;
        for (TActorIterator<ADSBomb> It(World.Get()); It; ++It) if (!It->IsActorBeingDestroyed()) ++LiveBombs;
        Check(TEXT("Returning to menu clears enemies bombs and formation actors"),
            Mode->Phase == EDSPhase::Menu && Mode->Enemies.IsEmpty() && LiveEnemies == 0 && LiveBombs == 0);
    }

    bool BeginFlightCase(int32 Count)
    {
        SpawnAtCapacity(Count == 3 ? 3 : 24);
        const TArray<ADSEnemyAircraft*> Spawned = Bombers();
        if (Spawned.Num() != Count) return false;
        // Retain exactly the group that SpawnWave produced. Remove unrelated
        // fighters so this controlled camera can observe its uninterrupted AI.
        for (ADSEnemyAircraft* Enemy : Mode->Enemies)
            if (IsValid(Enemy) && !Enemy->bBomber) Enemy->Destroy();
        Mode->Enemies.RemoveAll([](ADSEnemyAircraft* Enemy) { return !IsValid(Enemy) || !Enemy->bBomber; });
        Group.Reset();
        for (ADSEnemyAircraft* Bomber : Spawned) Group.Add(Bomber);
        ExpectedCount = Count;
        if (Count == 5) FiveSampleBaseline = FormationSamples;
        Phase = Count == 3 ? EPhase::ThreeFlight : EPhase::FiveFlight;
        PhaseStarted = PreviousSampleTime = World->GetTimeSeconds();
        PreviousLocations.Reset();
        CaptureStep = 0;
        Mode->RadioLine.Empty();
        InitialAirportHealth = Mode->AirportHealth;
        PC->SetViewTarget(ReviewCamera.Get());
        return true;
    }

    void AfterActorTick(UWorld* TickWorld, ELevelTick, float)
    {
        if (TickWorld == World.Get() && !bFinished) bFinished = Advance();
    }

    bool Advance()
    {
        if (!World.IsValid() || !Mode.IsValid() || !PC.IsValid()) return Finish(TEXT("A required actor disappeared."));
        const float Now = World->GetTimeSeconds();
        if (Now - SimulationStarted > 90.f) return Finish(TEXT("Bomber formation acceptance exceeded 90 simulated seconds."));
        ObserveFormation(Now);
        FollowCamera();
        if (PC->PlayerCameraManager) PC->PlayerCameraManager->UpdateCamera(0.f);
        if (!PendingScreenshot.IsEmpty())
        {
            if (!FScreenshotRequest::IsScreenshotRequested() && IFileManager::Get().FileSize(*PendingScreenshot) > 1024)
            {
                Images.Add(FPaths::GetCleanFilename(PendingScreenshot));
                PendingScreenshot.Empty();
            }
            else if (FPlatformTime::Seconds() - ScreenshotRequestedAt > 8.0)
                return Finish(TEXT("The rendered viewport did not save a requested evidence frame."));
            return false;
        }
        if (Phase == EPhase::ThreeFlight)
        {
            if (CaptureStep == 0 && Now - PhaseStarted >= 2.f) { Capture(TEXT("01_three_bombers_formation")); ++CaptureStep; }
            else if (CaptureStep == 1 && Now - PhaseStarted >= 3.5f) { Capture(TEXT("02_three_bombers_in_flight")); ++CaptureStep; }
            else if (CaptureStep == 2)
            {
                ThreeSamples = FormationSamples;
                Check(TEXT("Three aircraft sustained a symmetric moving formation over many frames"), ThreeSamples > 30 && MaxSlotError < 20.f && MaxSymmetryError < 20.f);
                for (const TWeakObjectPtr<ADSEnemyAircraft>& Bomber : Group)
                    if (Bomber.IsValid() && Bomber->FormationSlot != 0) SurvivorStarts.Add(Bomber, Bomber->GetActorLocation());
                if (!Group[0].IsValid()) return Finish(TEXT("The three-bomber front slot was not available for the loss case."));
                Group[0]->ApplyCombatDamage(Group[0]->Health + 1.f, true);
                Phase = EPhase::LeaderLoss;
                PhaseStarted = Now;
            }
        }
        else if (Phase == EPhase::LeaderLoss && Now - PhaseStarted >= 2.f)
        {
            bool bSurvivorsAdvanced = SurvivorStarts.Num() == 2;
            for (const TPair<TWeakObjectPtr<ADSEnemyAircraft>, FVector>& Pair : SurvivorStarts)
                bSurvivorsAdvanced &= Pair.Key.IsValid() && Pair.Key->bFormationActive &&
                    Pair.Key->FormationSlot != 0 && FVector::Dist(Pair.Key->GetActorLocation(), Pair.Value) > 25000.f;
            Check(TEXT("Destroying the front aircraft leaves surviving slots flying rather than hovering"), bSurvivorsAdvanced);
            if (!BeginFlightCase(5)) return Finish(TEXT("The real spawn path did not create five bombers."));
        }
        else if (Phase == EPhase::FiveFlight)
        {
            const ADSEnemyAircraft* Front = Group[0].Get();
            if (!Front) return Finish(TEXT("The five-bomber front slot disappeared before approach."));
            const float Range = FVector::Dist2D(Front->GetFormationAnchor(), Mode->AirportLocation);
            if (CaptureStep == 0 && Now - PhaseStarted >= 2.f) { Capture(TEXT("03_five_bombers_formation")); ++CaptureStep; }
            else if (CaptureStep == 1 && Now - PhaseStarted >= 4.f) { Capture(TEXT("04_five_bombers_in_flight")); ++CaptureStep; }
            else if (CaptureStep == 2 && Range < 380000.f && Front->bFormationActive)
            {
                Capture(TEXT("05_formation_approaching_airfield"));
                ++CaptureStep;
            }
            if (ReleasedCount > 0)
            {
                FiveSamples = FormationSamples - FiveSampleBaseline;
                ReleaseTime = Now;
                ReleaseDistance = Range;
                Check(TEXT("All five bombers release together at the three-kilometre approach threshold"),
                    ReleasedCount == 5 && Range <= 300100.f && Range >= 290000.f);
                Check(TEXT("Five aircraft retained symmetric slots throughout ingress"), FiveSamples > 100 && MaxSlotError < 20.f && MaxSymmetryError < 20.f);
                Check(TEXT("Bombers reached the approach without dropping ordnance while in formation"), !bEarlyBomb);
                Phase = EPhase::Released;
                PhaseStarted = Now;
                CameraSettleFrame = GFrameCounter + 3;
            }
        }
        else if (Phase == EPhase::Released)
        {
            if (Now - PhaseStarted >= 4.f && GFrameCounter >= CameraSettleFrame && CaptureStep == 3)
            {
                Capture(TEXT("06_formation_released_attack_runs"));
                ++CaptureStep;
            }
            if (FirstBomb.IsValid())
            {
                Phase = EPhase::Bombing;
                PhaseStarted = Now;
                CameraSettleFrame = GFrameCounter + 4;
            }
        }
        else if (Phase == EPhase::Bombing)
        {
            if (CaptureStep == 4 && GFrameCounter >= CameraSettleFrame && Now - PhaseStarted > .25f)
            {
                Capture(TEXT("07_bombs_released_from_aircraft"));
                ++CaptureStep;
            }
            if (Mode->AirportHealth < InitialAirportHealth)
            {
                Phase = EPhase::Damage;
                PhaseStarted = Now;
                CameraSettleFrame = GFrameCounter + 4;
            }
        }
        else if (Phase == EPhase::Damage && GFrameCounter >= CameraSettleFrame)
        {
            if (CaptureStep == 5)
            {
                Capture(TEXT("08_airfield_bomb_damage"));
                ++CaptureStep;
            }
            else
            {
                Check(TEXT("Real bomber AI created physical falling bombs after formation release"), ObservedBombs.Num() >= 3 && MaxBombsDropped >= 3 && FirstBombTime > ReleaseTime);
                Check(TEXT("Those real bombs reached terrain and damaged the airfield"), Mode->AirportHealth <= InitialAirportHealth - 300.f);
                Check(TEXT("Released bombers never returned to formation while conducting their attacks"), !bEverReformed && !bPartialRelease);
                Check(TEXT("Released aircraft visibly diverged from fixed wing slots into individual attack paths"), MaxAttackLateralChange > 2000.f);
                Check(TEXT("Aircraft advance at their real cruise speed while holding slots"), MotionSamples > 100 && MaxMotionRelativeError < .03f);
                return Finish();
            }
        }
        return false;
    }

    void ObserveFormation(float Now)
    {
        ReleasedCount = 0;
        const float Dt = Now - PreviousSampleTime;
        bool bAnyActive = false;
        int32 Dropped = 0;
        for (const TWeakObjectPtr<ADSEnemyAircraft>& Bomber : Group)
        {
            if (!Bomber.IsValid() || Bomber->bDying) continue;
            Dropped += Bomber->BombsDropped;
            if (!Bomber->bFormationActive) { ++ReleasedCount; continue; }
            bAnyActive = true;
            bEarlyBomb |= Bomber->BombsDropped > 0;
            bEverReformed |= Phase == EPhase::Released || Phase == EPhase::Bombing || Phase == EPhase::Damage;
            const FVector Heading = Bomber->GetFormationHeading();
            const FVector Right = FVector::CrossProduct(FVector::UpVector, Heading).GetSafeNormal();
            const FVector Relative = Bomber->GetActorLocation() - Bomber->GetFormationAnchor();
            const int32 Slot = Bomber->FormationSlot;
            const int32 Row = (Slot + 1) / 2;
            const float Side = Slot == 0 ? 0.f : (Slot % 2 == 1 ? -1.f : 1.f);
            const FVector Expected = Heading * (-16000.f * Row) + Right * (14000.f * Row * Side);
            MaxSlotError = FMath::Max(MaxSlotError, static_cast<float>(FVector::Dist(Relative, Expected)));
            if (const FVector* Previous = PreviousLocations.Find(Bomber); Previous && Dt > SMALL_NUMBER)
            {
                const float ExpectedTravel = Bomber->Speed * 100.f * Dt;
                const float ActualTravel = FVector::Dist(Bomber->GetActorLocation(), *Previous);
                MaxMotionRelativeError = FMath::Max(MaxMotionRelativeError, FMath::Abs(ActualTravel - ExpectedTravel) / FMath::Max(ExpectedTravel, 1.f));
                ++MotionSamples;
            }
            PreviousLocations.Add(Bomber, Bomber->GetActorLocation());
        }
        if (bAnyActive)
        {
            ++FormationSamples;
            if (ReleasedCount > 0) bPartialRelease = true;
            for (int32 I = 1; I + 1 < Group.Num(); I += 2)
                if (Group[I].IsValid() && Group[I + 1].IsValid() && !Group[I]->bDying && !Group[I + 1]->bDying)
                {
                    const FVector A = Group[I]->GetActorLocation() - Group[I]->GetFormationAnchor();
                    const FVector B = Group[I + 1]->GetActorLocation() - Group[I + 1]->GetFormationAnchor();
                    const FVector Heading = Group[I]->GetFormationHeading();
                    const FVector Symmetry = A + B - Heading * FVector::DotProduct(A + B, Heading);
                    MaxSymmetryError = FMath::Max(MaxSymmetryError, static_cast<float>(Symmetry.Size()));
                }
        }
        MaxBombsDropped = FMath::Max(MaxBombsDropped, Dropped);
        if (Phase == EPhase::Released || Phase == EPhase::Bombing || Phase == EPhase::Damage)
        {
            const ADSEnemyAircraft* Front = Group[0].Get();
            if (Front)
            {
                const FVector Right = FVector::CrossProduct(FVector::UpVector, Front->GetFormationHeading()).GetSafeNormal();
                for (int32 I = 1; I < Group.Num(); ++I)
                    if (Group[I].IsValid())
                    {
                        const float ExpectedSide = ((I + 1) / 2) * (I % 2 == 1 ? -14000.f : 14000.f);
                        const float ActualSide = FVector::DotProduct(Group[I]->GetActorLocation() - Front->GetActorLocation(), Right);
                        MaxAttackLateralChange = FMath::Max(MaxAttackLateralChange, FMath::Abs(ActualSide - ExpectedSide));
                    }
            }
        }
        for (TActorIterator<ADSBomb> It(World.Get()); It; ++It)
        {
            if (It->IsActorBeingDestroyed()) continue;
            ObservedBombs.Add(*It);
            if (FirstBombTime == 0.f) { FirstBomb = *It; FirstBombTime = Now; }
        }
        PreviousSampleTime = Now;
    }

    void FollowCamera()
    {
        if (!ReviewCamera.IsValid() || Group.IsEmpty() || !Group.Last().IsValid()) return;
        FVector Center = FVector::ZeroVector;
        int32 Live = 0;
        for (const TWeakObjectPtr<ADSEnemyAircraft>& Bomber : Group)
            if (Bomber.IsValid() && !Bomber->bDying) { Center += Bomber->GetActorLocation(); ++Live; }
        if (!Live) return;
        Center /= Live;
        const FVector Heading = Group.Last()->GetFormationHeading();
        const FVector Right = FVector::CrossProduct(FVector::UpVector, Heading).GetSafeNormal();
        FVector CameraPosition = Center - Heading * (ExpectedCount == 3 ? 34000.f : 52000.f) + FVector(0, 0, ExpectedCount == 3 ? 47000.f : 68000.f);
        // The wider five-aircraft view keeps both outer wings inside the space
        // between the radar and telemetry panels, while retaining the real HUD.
        FVector LookAt = ExpectedCount == 3 ? Center + Heading * 6000.f : Center;
        float Fov = ExpectedCount == 3 ? 58.f : 68.f;
        if (Phase == EPhase::Bombing && Group[0].IsValid())
        {
            const FVector AircraftPosition = Group[0]->GetActorLocation();
            LookAt = FirstBomb.IsValid() ? (AircraftPosition + FirstBomb->GetActorLocation()) * .5f : AircraftPosition - FVector(0, 0, 3000.f);
            CameraPosition = LookAt - Heading * 4500.f - Right * 13500.f + FVector(0, 0, 3500.f);
            Fov = 60.f;
        }
        else if (Phase == EPhase::Damage)
        {
            LookAt = Mode->AirportLocation + FVector(0, 0, 1500.f);
            CameraPosition = Mode->AirportLocation - Heading * 115000.f - Right * 70000.f + FVector(0, 0, 95000.f);
            Fov = 62.f;
        }
        ReviewCamera->GetCameraComponent()->SetFieldOfView(Fov);
        ReviewCamera->SetActorLocationAndRotation(CameraPosition, (LookAt - CameraPosition).Rotation());
        PC->SetViewTarget(ReviewCamera.Get());
    }

    void Capture(const TCHAR* Name)
    {
        if (ExpectedCount == 5 && Phase == EPhase::FiveFlight)
        {
            bool bClearOfHUD = true;
            for (const TWeakObjectPtr<ADSEnemyAircraft>& Bomber : Group)
            {
                FVector2D Screen;
                bClearOfHUD &= Bomber.IsValid() && PC->ProjectWorldLocationToScreen(Bomber->GetActorLocation(), Screen) &&
                    Screen.X >= 400.f && Screen.X <= 1500.f && Screen.Y >= 220.f && Screen.Y <= 850.f;
            }
            Check(FString::Printf(TEXT("%s keeps all five aircraft centers within the HUD-free observation area"), Name), bClearOfHUD);
        }
        if (bNoMedia)
        {
            Captures.Add(FString::Printf(TEXT("{\"file\":null,\"kind\":\"numeric_only\",\"moment\":\"%s\",\"world_time_s\":%.6f,\"render_frame\":%llu,\"group_size\":%d,\"released\":%d,\"bombs_created\":%d,\"airport_health\":%.1f,\"max_slot_error_cm\":%.6f}"),
                Name, World->GetTimeSeconds(), static_cast<unsigned long long>(GFrameCounter), ExpectedCount, ReleasedCount,
                ObservedBombs.Num(), Mode->AirportHealth, MaxSlotError));
            return;
        }
        PendingScreenshot = OutputDir / (FString(Name) + TEXT(".png"));
        ScreenshotRequestedAt = FPlatformTime::Seconds();
        Captures.Add(FString::Printf(TEXT("{\"file\":\"%s.png\",\"world_time_s\":%.6f,\"render_frame\":%llu,\"group_size\":%d,\"released\":%d,\"bombs_created\":%d,\"airport_health\":%.1f,\"max_slot_error_cm\":%.6f}"),
            Name, World->GetTimeSeconds(), static_cast<unsigned long long>(GFrameCounter), ExpectedCount, ReleasedCount,
            ObservedBombs.Num(), Mode->AirportHealth, MaxSlotError));
        FScreenshotRequest::RequestScreenshot(PendingScreenshot, false, false);
    }

    bool Finish(const TCHAR* Error = nullptr)
    {
        RemoveDelegate();
        if (Error) Check(Error, false);
        const float FinalAirportHealth = Mode.IsValid() ? Mode->AirportHealth : 0.f;
        if (bInitialized && Mode.IsValid())
        {
            Mode->ReturnToMenu();
            int32 RemainingBombs = 0;
            for (TActorIterator<ADSBomb> It(World.Get()); It; ++It) if (!It->IsActorBeingDestroyed()) ++RemainingBombs;
            Check(TEXT("Final menu transition clears the completed attack and all live ordnance"), Mode->Enemies.IsEmpty() && Bombers().IsEmpty() && RemainingBombs == 0);
            Mode->SelectAircraft(OriginalSelection);
            Mode->SetActorTickEnabled(bOriginalModeTick);
            if (Mode->Player) Mode->Player->SetActorTickEnabled(bOriginalPlayerTick);
        }
        if (PC.IsValid())
        {
            PC->bAutoManageActiveCameraTarget = bOriginalAutoCamera;
            if (OriginalViewTarget.IsValid()) PC->SetViewTarget(OriginalViewTarget.Get());
        }
        if (ReviewCamera.IsValid()) ReviewCamera->Destroy();
        if (bNoMedia)
        {
            Check(TEXT("Eight numeric formation observations were recorded"), Captures.Num() == 8);
            Test->AddInfo(TEXT("DSNoMedia: screenshot evidence was not requested or evaluated; visual review was not observed."));
        }
        else Check(TEXT("Eight actual viewport evidence frames were saved"), Images.Num() == 8);
        if (!OutputDir.IsEmpty())
        {
            TArray<FString> ImageJson;
            for (const FString& Name : Images) ImageJson.Add(TEXT("\"") + Name + TEXT("\""));
            const FString Report = FString::Printf(TEXT("{\"schema\":\"bomber-formation-acceptance/1\",\"status\":\"%s\",\"checks_scope\":\"mechanical_only\",\"visual_review\":\"%s\",\"media_capture\":\"%s\",\"media_files_written\":%d,\"screenshot_check\":\"%s\",\"fixture\":\"development observation of real wave spawning and enemy AI; no AutoPlay\",\"simulated_seconds\":%.3f,\"checks\":[%s],\"capacity_cases\":[%s],\"three_frame_samples\":%d,\"five_frame_samples\":%d,\"motion_samples\":%d,\"max_slot_error_cm\":%.6f,\"max_symmetry_error_cm\":%.6f,\"max_motion_relative_error\":%.6f,\"release_distance_cm\":%.3f,\"attack_lateral_change_cm\":%.3f,\"bombs_observed\":%d,\"initial_airport_health\":%.1f,\"final_airport_health\":%.1f,\"screenshots\":[%s],\"capture_observations\":[%s]}\n"),
                Test->HasAnyErrors() ? TEXT("failed") : TEXT("passed"), bNoMedia ? TEXT("not_observed") : TEXT("required"),
                bNoMedia ? TEXT("disabled") : TEXT("enabled"), Images.Num(), bNoMedia ? TEXT("not_run") : TEXT("evaluated"), World.IsValid() ? World->GetTimeSeconds() - SimulationStarted : 0.f,
                *FString::Join(Checks, TEXT(",")), *FString::Join(CapacityReports, TEXT(",")), ThreeSamples, FiveSamples, MotionSamples,
                MaxSlotError, MaxSymmetryError, MaxMotionRelativeError, ReleaseDistance, MaxAttackLateralChange, ObservedBombs.Num(), InitialAirportHealth, FinalAirportHealth,
                *FString::Join(ImageJson, TEXT(",")), *FString::Join(Captures, TEXT(",")));
            Test->TestTrue(TEXT("Bomber formation report saved"), FFileHelper::SaveStringToFile(Report, *(OutputDir / TEXT("report.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
            Test->AddInfo(TEXT("Bomber formation evidence: ") + OutputDir);
        }
        return true;
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDSBomberFormationRenderTest, "DuneSentinel.Rendering.BomberFormation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FDSBomberFormationRenderTest::RunTest(const FString& Parameters)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("DSBomberFormationValidation")) || FParse::Param(FCommandLine::Get(), TEXT("AutoPlay")) ||
        (FParse::Param(FCommandLine::Get(), TEXT("DSNoMedia")) && FParse::Param(FCommandLine::Get(), TEXT("CaptureFrames"))))
    {
        AddError(TEXT("Use a dedicated -game -DSBomberFormationValidation launch without -AutoPlay on DesertDefense; DSNoMedia also forbids CaptureFrames."));
        return false;
    }
    ADD_LATENT_AUTOMATION_COMMAND(FDSBomberFormationRenderCommand(this));
    return true;
}
#endif
