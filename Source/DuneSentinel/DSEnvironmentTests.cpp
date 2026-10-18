#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "DSEnemyAircraft.h"
#include "DSFlightPawn.h"
#include "DSGameMode.h"
#include "DSProjectile.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

namespace
{
// This dedicated fixture observes the shipped map and normal enemy steering.
// Framebuffer bytes stay in memory; the evidence on disk is a numeric report.
class FDSEnvironmentAcceptanceCommand final : public IAutomationLatentCommand
{
public:
    explicit FDSEnvironmentAcceptanceCommand(FAutomationTestBase* InTest) : Test(InTest) {}
    virtual ~FDSEnvironmentAcceptanceCommand() override { RemoveDelegate(); }

    virtual bool Update() override
    {
        if (bFinished) return true;
        if (GetCurrentRunTime() > 90.) return Finish(TEXT("Environment acceptance exceeded 90 seconds."));
        if (!bInitialized) return Initialize();
        if (!World.IsValid() || !Mode.IsValid() || !PC.IsValid() || !Camera.IsValid())
            return Finish(TEXT("The real gameplay observation world became unavailable."));
        if (World->GetTimeSeconds() - StageTime < .9f || GFrameCounter < StageFrame + 8) return false;
        if (!CategoryVisibilityMatches(Stage == EStage::Visible))
            return Finish(TEXT("Environment category visibility did not match the isolated comparison."));
        TArray<FColor>& Destination = Stage == EStage::Baseline ? BaselinePixels : VisiblePixels;
        const EReadback Result = ReadViewport(Destination);
        if (Result == EReadback::Pending) return false;
        if (Result == EReadback::Failed) return Finish(*ReadbackError);
        if (Stage == EStage::Baseline)
        {
            if (!BuildObservationRegion()) return Finish(TEXT("The environment observation bounds were outside the view."));
            SetCategoryVisible(VisualCase, true);
            Stage = EStage::Visible;
            ResetStageClock();
        }
        else
        {
            if (BaselinePixels.Num() != VisiblePixels.Num())
                return Finish(TEXT("Viewport dimensions changed during environment comparison."));
            CompareRegion();
            BaselinePixels.Reset();
            VisiblePixels.Reset();
            if (++VisualCase == 3) return Finish();
            BeginVisualCase();
        }
        return false;
    }

private:
    enum class EStage { Baseline, Visible };
    enum class EReadback { Pending, Ready, Failed };
    struct FHiddenState { TWeakObjectPtr<AActor> Actor; bool bHidden = false; };
    FAutomationTestBase* Test;
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<ADSGameMode> Mode;
    TWeakObjectPtr<APlayerController> PC;
    TWeakObjectPtr<ACameraActor> Camera;
    TWeakObjectPtr<AActor> OriginalViewTarget;
    TWeakObjectPtr<UStaticMeshComponent> Terrain;
    TArray<TWeakObjectPtr<AStaticMeshActor>> Town, Water, Vegetation, Banks;
    TArray<FHiddenState> HiddenStates;
    TArray<FString> Checks, Observations;
    TArray<FColor> BaselinePixels, VisiblePixels, PendingPixels;
    FDelegateHandle ViewportRenderedHandle;
    FIntPoint ViewSize = FIntPoint::ZeroValue;
    FIntRect ObservationRegion;
    FBox VegetationViewBounds = FBox(ForceInit);
    FString OutputDir, ReadbackError;
    EStage Stage = EStage::Baseline;
    float StageTime = 0.f;
    uint64 StageFrame = 0;
    double ReadbackRequestedAt = 0.;
    int32 VisualCase = 0, OriginalSelection = 0;
    bool bInitialized = false, bStateCaptured = false, bFinished = false;
    bool bReadbackRequested = false, bReadbackReady = false;
    bool bOriginalModeTick = true, bOriginalPlayerTick = true, bOriginalAutoCamera = true;
    bool bOriginalPlayerHidden = false, bOriginalHUD = true;
    const FVector LakeCenters[3] = { FVector(-270000, -220000, 6000), FVector(420000, 180000, 9200), FVector(100000, 530000, 11800) };

    void Check(const TCHAR* Name, bool bPassed)
    {
        Test->TestTrue(Name, bPassed);
        Checks.Add(FString::Printf(TEXT("{\"name\":\"%s\",\"passed\":%s}"), Name, bPassed ? TEXT("true") : TEXT("false")));
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
        if (!Mode.IsValid() || !Mode->Player || !PC.IsValid()) return Finish(TEXT("DesertDefense must provide the real mode, player and controller."));
        OutputDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Validation/Environment") /
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
        Mode->StartSortie();
        ClearCombatActors();
        Mode->SetActorTickEnabled(false);
        Mode->Player->SetActorTickEnabled(false);
        Mode->Player->SetActorHiddenInGame(true);
        Mode->Player->SetActorLocation(FVector(0, 0, 300000));
        Mode->RadioLine.Empty();
        if (PC->GetHUD()) PC->GetHUD()->bShowHUD = false;
        if (PC->PlayerCameraManager) PC->PlayerCameraManager->StopCameraFade();
        int32 TerrainCount = 0;
        for (TActorIterator<AStaticMeshActor> It(GameWorld); It; ++It)
        {
            AStaticMeshActor* Actor = *It;
            if (Actor->ActorHasTag(TEXT("DSEnvironmentTerrain"))) { Terrain = Actor->GetStaticMeshComponent(); ++TerrainCount; }
            if (Actor->ActorHasTag(TEXT("DSEnvironmentTown"))) Town.Add(Actor);
            if (Actor->ActorHasTag(TEXT("DSEnvironmentWater"))) Water.Add(Actor);
            if (Actor->ActorHasTag(TEXT("DSEnvironmentVegetation"))) Vegetation.Add(Actor);
            if (Actor->ActorHasTag(TEXT("DSEnvironmentBank"))) Banks.Add(Actor);
        }
        Check(TEXT("Exactly one terrain component is present in the real map"), TerrainCount == 1 && Terrain.IsValid());
        Check(TEXT("Airport town contains at least forty assembled buildings"), Town.Num() >= 40);
        Check(TEXT("Three lake surfaces and three green banks are present"), Water.Num() == 3 && Banks.Num() == 3);
        Check(TEXT("Oasis vegetation contains at least one hundred placed meshes"), Vegetation.Num() >= 100);
        if (!Terrain.IsValid() || Town.IsEmpty() || Water.IsEmpty() || Vegetation.IsEmpty())
            return Finish(TEXT("The authored environment is incomplete; runtime and rendering checks cannot proceed."));
        for (int32 Category = 0; Category < 3; ++Category)
            for (const TWeakObjectPtr<AStaticMeshActor>& Actor : CategoryActors(Category))
                if (Actor.IsValid()) HiddenStates.Add({Actor.Get(), Actor->IsHidden()});
        RunGeometryChecks();
        RunEnemyClearanceChecks();
        ClearCombatActors();
        Mode->Player->SetActorLocation(FVector(0, 0, 300000));
        Camera = GameWorld->SpawnActor<ACameraActor>();
        if (!Camera.IsValid()) return Finish(TEXT("Could not create the environment observation camera."));
        Camera->GetCameraComponent()->SetFieldOfView(60.f);
        PC->bAutoManageActiveCameraTarget = false;
        ViewportRenderedHandle = UGameViewportClient::OnViewportRendered().AddRaw(this, &FDSEnvironmentAcceptanceCommand::AfterViewportRendered);
        bInitialized = true;
        BeginVisualCase();
        return false;
    }

    void ClearCombatActors()
    {
        for (TActorIterator<ADSEnemyAircraft> It(World.Get()); It; ++It) It->Destroy();
        for (TActorIterator<ADSProjectile> It(World.Get()); It; ++It) It->Destroy();
        for (TActorIterator<ADSBomb> It(World.Get()); It; ++It) It->Destroy();
        Mode->Enemies.Reset();
    }

    bool TerrainHeight(const FVector& Position, double& Height) const
    {
        FHitResult Hit;
        FCollisionQueryParams Params(SCENE_QUERY_STAT(DSEnvironmentAcceptance), true);
        const bool bHit = Terrain.IsValid() && Terrain->LineTraceComponent(Hit,
            FVector(Position.X, Position.Y, 1500000.), FVector(Position.X, Position.Y, -50000.), Params);
        if (bHit) Height = Hit.ImpactPoint.Z;
        return bHit;
    }

    static bool HasTexture(UStaticMeshComponent* Component)
    {
        TArray<UTexture*> Textures;
        if (Component) Component->GetUsedTextures(Textures, EMaterialQualityLevel::High);
        return Textures.Num() > 0;
    }

    void RunGeometryChecks()
    {
        double Lowest = DBL_MAX, Highest = -DBL_MAX, LargestNeighborRise = 0.;
        int32 Hits = 0, Samples = 0;
        for (int32 X = -850000; X <= 850000; X += 50000)
        {
            double Previous = 0.;
            bool bPrevious = false;
            for (int32 Y = -850000; Y <= 850000; Y += 50000)
            {
                double Z = 0.; ++Samples;
                if (!TerrainHeight(FVector(X, Y, 0), Z)) { bPrevious = false; continue; }
                ++Hits;
                Lowest = FMath::Min(Lowest, Z); Highest = FMath::Max(Highest, Z);
                if (bPrevious) LargestNeighborRise = FMath::Max(LargestNeighborRise, FMath::Abs(Z - Previous));
                Previous = Z; bPrevious = true;
            }
        }
        Check(TEXT("Collision terrain covers every sampled point in the combat arena"), Hits == Samples);
        Check(TEXT("Actual terrain collision has at least 1800 metres of mountain relief"), Hits > 0 && Highest - Lowest >= 180000.);
        Check(TEXT("Ridges have at least 350 metres of local rise over a 500 metre sample interval"), LargestNeighborRise >= 35000.);
        Check(TEXT("Terrain uses sampled texture materials"), HasTexture(Terrain.Get()));
        bool bFlat = true, bRunwayClear = true;
        double MaxFlatError = 0., HighestRunway = -DBL_MAX;
        for (int32 X = -160000; X <= 160000; X += 20000)
            for (int32 Y : {-10000, 0, 10000})
            {
                double Height = 0.;
                if (!TerrainHeight(FVector(X, Y, 0), Height)) { bFlat = false; continue; }
                MaxFlatError = FMath::Max(MaxFlatError, FMath::Abs(Height - 2180.));
                bFlat &= FMath::Abs(Height - 2180.) < 5.;
            }
        for (int32 X = -80000; X <= 80000; X += 10000)
        {
            FHitResult Hit;
            FCollisionQueryParams Params(SCENE_QUERY_STAT(DSEnvironmentRunway), true);
            Params.AddIgnoredActor(Mode->Player.Get());
            const bool bHit = World->LineTraceSingleByObjectType(Hit, FVector(X, 0, 100000), FVector(X, 0, -5000),
                FCollisionObjectQueryParams(ECC_WorldStatic), Params);
            bRunwayClear &= bHit && Hit.ImpactPoint.Z >= 2150. && Hit.ImpactPoint.Z <= 2400.;
            if (bHit) HighestRunway = FMath::Max(HighestRunway, Hit.ImpactPoint.Z);
        }
        Check(TEXT("Protected runway and launch corridor retain their flat terrain elevation"), bFlat);
        Check(TEXT("Actual runway collision has no added buildings or hills above its surface"), bRunwayClear);
        Observations.Add(FString::Printf(TEXT("{\"case\":\"terrain\",\"samples\":%d,\"collision_hits\":%d,\"minimum_cm\":%.3f,\"maximum_cm\":%.3f,\"relief_cm\":%.3f,\"largest_500m_rise_cm\":%.3f,\"flat_corridor_error_cm\":%.3f,\"highest_runway_collision_cm\":%.3f}"),
            Samples, Hits, Lowest, Highest, Highest-Lowest, LargestNeighborRise, MaxFlatError, HighestRunway));

        int32 TownGrounded = 0, TownTextured = 0, TownDetailed = 0, TownCollidable = 0, DownloadedBuildings = 0;
        bool bTownOutsideRunway = true;
        double MaximumTownBaseError = 0.;
        TSet<UStaticMesh*> BuildingVariants;
        for (const TWeakObjectPtr<AStaticMeshActor>& Actor : Town)
        {
            UStaticMeshComponent* Mesh = Actor->GetStaticMeshComponent();
            const FVector Position = Actor->GetActorLocation();
            double Ground = 0.;
            if (TerrainHeight(Position, Ground))
            {
                MaximumTownBaseError = FMath::Max(MaximumTownBaseError, FMath::Abs(Position.Z-Ground));
                if (FMath::Abs(Position.Z-Ground) <= 100.) ++TownGrounded;
            }
            if (HasTexture(Mesh)) ++TownTextured;
            if (Mesh->GetNumMaterials() >= 3) ++TownDetailed;
            if (Mesh->IsQueryCollisionEnabled()) ++TownCollidable;
            BuildingVariants.Add(Mesh->GetStaticMesh());
            if (Actor->ActorHasTag(TEXT("DSDownloadedArt")) && Mesh->GetStaticMesh() &&
                Mesh->GetStaticMesh()->GetPathName().Contains(TEXT("/Downloaded/"))) ++DownloadedBuildings;
            const FBox Bounds = Mesh->Bounds.GetBox();
            bTownOutsideRunway &= Bounds.Min.Y > 150000. && FVector::Dist2D(Position, FVector(0,260000,0)) < 90000.;
        }
        Check(TEXT("Every town building is grounded on the actual terrain within one metre"), TownGrounded == Town.Num());
        Check(TEXT("Town contains at least four textured building variants with facade material sections"),
            BuildingVariants.Num() >= 4 && TownTextured == Town.Num() && TownDetailed >= 40);
        Check(TEXT("Town buildings retain query collision and stay beyond the runway approach area"), TownCollidable == Town.Num() && bTownOutsideRunway);
        Check(TEXT("Every town building uses the downloaded architecture kit"), DownloadedBuildings == Town.Num());
        Observations.Add(FString::Printf(TEXT("{\"case\":\"town\",\"buildings\":%d,\"variants\":%d,\"grounded\":%d,\"textured\":%d,\"multi_material\":%d,\"downloaded_models\":%d,\"maximum_base_error_cm\":%.3f}"),
            Town.Num(), BuildingVariants.Num(), TownGrounded, TownTextured, TownDetailed, DownloadedBuildings, MaximumTownBaseError));

        for (int32 I = 0; I < 3; ++I)
        {
            double Floor = 0.;
            const bool bFloor = TerrainHeight(LakeCenters[I], Floor);
            AStaticMeshActor* Nearest = nullptr;
            double Distance = DBL_MAX;
            for (const TWeakObjectPtr<AStaticMeshActor>& Actor : Water)
            {
                const double Candidate = FVector::Dist2D(Actor->GetStaticMeshComponent()->Bounds.Origin, LakeCenters[I]);
                if (Candidate < Distance) { Distance = Candidate; Nearest = Actor.Get(); }
            }
            FHitResult LakeHit;
            FCollisionQueryParams Params(SCENE_QUERY_STAT(DSEnvironmentLake), true);
            const bool bSurface = Nearest && Nearest->GetStaticMeshComponent()->LineTraceComponent(LakeHit,
                LakeCenters[I]+FVector(0,0,10000), LakeCenters[I]-FVector(0,0,10000), Params);
            const bool bLakeValid = bFloor && bSurface && Distance < 10000. &&
                FMath::Abs(LakeHit.ImpactPoint.Z-LakeCenters[I].Z) < 20. && LakeHit.ImpactPoint.Z-Floor > 300. && HasTexture(Nearest->GetStaticMeshComponent());
            Check(*FString::Printf(TEXT("Oasis %d has a textured lake surface above a genuine terrain basin"), I+1), bLakeValid);
            Observations.Add(FString::Printf(TEXT("{\"case\":\"lake_%d\",\"terrain_hit\":%s,\"water_hit\":%s,\"terrain_floor_cm\":%.3f,\"water_surface_cm\":%.3f,\"depth_cm\":%.3f,\"bounds_center_offset_cm\":%.3f}"),
                I+1, bFloor?TEXT("true"):TEXT("false"), bSurface?TEXT("true"):TEXT("false"), Floor, bSurface?LakeHit.ImpactPoint.Z:0., bSurface?LakeHit.ImpactPoint.Z-Floor:0., Distance));
        }
        for (int32 I = 0; I < Banks.Num(); ++I)
        {
            UStaticMeshComponent* Bank = Banks[I]->GetStaticMeshComponent();
            const FName OriginalProfile = Bank->GetCollisionProfileName();
            const ECollisionEnabled::Type OriginalCollision = Bank->GetCollisionEnabled();
            const FCollisionResponseContainer OriginalResponses = Bank->GetCollisionResponseToChannels();
            // Decorative banks do not collide during play. Temporarily expose
            // the same component's complex triangles to queries, then restore
            // its settings before any gameplay or rendered observation.
            Bank->SetCollisionResponseToAllChannels(ECR_Block);
            Bank->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
            const FBox Bounds = Bank->Bounds.GetBox();
            int32 BankHits = 0, GroundMisses = 0;
            double MaximumGap = 0., MaximumBurial = 0., AbsoluteErrorSum = 0.;
            FCollisionQueryParams Params(SCENE_QUERY_STAT(DSEnvironmentBank), true);
            for (double X = Bounds.Min.X+1250.; X < Bounds.Max.X; X += 2500.)
                for (double Y = Bounds.Min.Y+1250.; Y < Bounds.Max.Y; Y += 2500.)
                {
                    FHitResult BankHit;
                    if (!Bank->LineTraceComponent(BankHit,FVector(X,Y,1500000.),FVector(X,Y,-50000.),Params)) continue;
                    ++BankHits;
                    double Ground = 0.;
                    if (!TerrainHeight(FVector(X,Y,0),Ground)) { ++GroundMisses; continue; }
                    const double Difference = BankHit.ImpactPoint.Z-Ground;
                    MaximumGap = FMath::Max(MaximumGap,Difference);
                    MaximumBurial = FMath::Max(MaximumBurial,-Difference);
                    AbsoluteErrorSum += FMath::Abs(Difference);
                }
            Bank->SetCollisionEnabled(OriginalCollision);
            Bank->SetCollisionResponseToChannels(OriginalResponses);
            Bank->SetCollisionProfileName(OriginalProfile);
            Check(*FString::Printf(TEXT("Green bank %d conforms to actual terrain triangles within half a metre"),I+1),
                BankHits > 20 && GroundMisses == 0 && MaximumGap <= 50. && MaximumBurial <= 50.);
            Check(*FString::Printf(TEXT("Green bank %d collision settings are restored after geometry queries"),I+1),
                Bank->GetCollisionEnabled() == OriginalCollision && Bank->GetCollisionResponseToChannels() == OriginalResponses && Bank->GetCollisionProfileName() == OriginalProfile);
            Observations.Add(FString::Printf(TEXT("{\"case\":\"bank_%d\",\"sample_spacing_cm\":2500,\"mesh_hits\":%d,\"terrain_misses\":%d,\"maximum_gap_cm\":%.3f,\"maximum_burial_cm\":%.3f,\"mean_absolute_error_cm\":%.3f}"),
                I+1,BankHits,GroundMisses,MaximumGap,MaximumBurial,AbsoluteErrorSum/FMath::Max(BankHits-GroundMisses,1)));
        }
        int32 GroundedFlora = 0, MaskedFlora = 0, TexturedFlora = 0, BankedFlora = 0, DownloadedFlora = 0;
        double MaximumFloraBaseError = 0.;
        TSet<UStaticMesh*> FloraVariants;
        for (const TWeakObjectPtr<AStaticMeshActor>& Actor : Vegetation)
        {
            UStaticMeshComponent* Mesh = Actor->GetStaticMeshComponent();
            const FVector Position = Actor->GetActorLocation();
            double Ground = 0.;
            if (TerrainHeight(Position, Ground))
            {
                MaximumFloraBaseError = FMath::Max(MaximumFloraBaseError, FMath::Abs(Position.Z-Ground));
                if (FMath::Abs(Position.Z-Ground) <= 100.) ++GroundedFlora;
            }
            bool bMasked = false;
            for (int32 Slot = 0; Slot < Mesh->GetNumMaterials(); ++Slot)
                if (UMaterialInterface* Material = Mesh->GetMaterial(Slot)) bMasked |= Material->GetBlendMode() == BLEND_Masked;
            if (bMasked) ++MaskedFlora;
            if (HasTexture(Mesh)) ++TexturedFlora;
            if (Actor->ActorHasTag(TEXT("DSDownloadedArt")) && Mesh->GetStaticMesh() &&
                Mesh->GetStaticMesh()->GetPathName().Contains(TEXT("/Downloaded/"))) ++DownloadedFlora;
            FloraVariants.Add(Mesh->GetStaticMesh());
            double NearestDistance = DBL_MAX, NearestWater = 0.;
            for (const FVector& Center : LakeCenters)
                if (FVector::Dist2D(Position, Center) < NearestDistance) { NearestDistance = FVector::Dist2D(Position, Center); NearestWater = Center.Z; }
            if (NearestDistance < 100000. && Ground >= NearestWater-150.) ++BankedFlora;
        }
        Check(TEXT("Every vegetation origin is grounded within one metre of the terrain mesh"), GroundedFlora == Vegetation.Num());
        Check(TEXT("At least three textured vegetation variants use cutout leaf materials"),
            FloraVariants.Num() >= 3 && TexturedFlora == Vegetation.Num() && MaskedFlora == Vegetation.Num());
        Check(TEXT("Oasis vegetation sits on nearby banks instead of below lake surfaces"), BankedFlora == Vegetation.Num());
        Check(TEXT("Every oasis vegetation mesh uses downloaded plant geometry"), DownloadedFlora == Vegetation.Num());
        Observations.Add(FString::Printf(TEXT("{\"case\":\"vegetation\",\"instances\":%d,\"variants\":%d,\"grounded\":%d,\"masked\":%d,\"near_lake_banks\":%d,\"downloaded_models\":%d,\"maximum_base_error_cm\":%.3f}"),
            Vegetation.Num(), FloraVariants.Num(), GroundedFlora, MaskedFlora, BankedFlora, DownloadedFlora, MaximumFloraBaseError));
    }

    ADSEnemyAircraft* MakeEnemy(bool bBomber, const FVector& Position, const FVector& Heading)
    {
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        ADSEnemyAircraft* Enemy = World->SpawnActor<ADSEnemyAircraft>(Position, Heading.Rotation(), Params);
        if (Enemy)
        {
            Enemy->Initialize(bBomber, Mode->AirportLocation);
            Enemy->SetActorTickEnabled(false);
            Mode->Enemies.Add(Enemy);
        }
        return Enemy;
    }

    int32 TickSpawnedBombs(float DeltaSeconds)
    {
        int32 Remaining = 0;
        for (TActorIterator<ADSBomb> It(World.Get()); It; ++It)
        {
            if (It->IsActorBeingDestroyed()) continue;
            It->SetActorTickEnabled(false);
            It->Tick(DeltaSeconds);
            if (!It->IsActorBeingDestroyed()) ++Remaining;
        }
        return Remaining;
    }

    void SettleSpawnedBombs(float DeltaSeconds)
    {
        // Use the real gravity, flight velocity, terrain contact, damage and
        // lifespan paths. The bound matches the ordinary bomb lifetime.
        for (int32 I = 0; I < FMath::CeilToInt(40.f/DeltaSeconds)+1; ++I)
            if (TickSpawnedBombs(DeltaSeconds) == 0) break;
    }

    void RunEnemyClearanceChecks()
    {
        // Independently inspect the widest supported formation over a full
        // compass of arrivals. Staggered 75 m stations and 25 m wing margins
        // also cover positions between the spawn planner's own probes.
        double IngressMinimumClearance = DBL_MAX, LowestIngress = DBL_MAX, HighestIngress = -DBL_MAX;
        int32 IngressSamples = 0, IngressTraceMisses = 0;
        for (int32 Direction = 0; Direction < 24; ++Direction)
        {
            const double Angle = Direction*UE_TWO_PI/24.;
            const FVector OutwardRay(FMath::Cos(Angle), FMath::Sin(Angle), 0);
            FVector Origin = Mode->AirportLocation+OutwardRay*850000.;
            Origin.Z = Mode->GetBomberIngressAltitude(Origin,5);
            LowestIngress = FMath::Min(LowestIngress, Origin.Z);
            HighestIngress = FMath::Max(HighestIngress, Origin.Z);
            const FVector Heading = -OutwardRay;
            const FVector Right = FVector::CrossProduct(FVector::UpVector, Heading);
            const double ApproachLength = 850000.-ADSEnemyAircraft::FormationBreakDistance;
            const int32 Stations = FMath::CeilToInt(ApproachLength/7500.);
            for (int32 Slot = 0; Slot < 5; ++Slot)
            {
                const FVector Offset = ADSEnemyAircraft::GetFormationSlotOffset(Slot);
                for (int32 Station = 0; Station <= Stations; ++Station)
                    for (const double WingOffset : {-2500.,0.,2500.})
                    {
                        const FVector Position = Origin+Heading*(Offset.X+ApproachLength*Station/Stations)+Right*(Offset.Y+WingOffset);
                        double Ground = 0.;
                        ++IngressSamples;
                        if (!TerrainHeight(Position,Ground)) { ++IngressTraceMisses; continue; }
                        IngressMinimumClearance = FMath::Min(IngressMinimumClearance,Position.Z-Ground);
                    }
            }
        }
        Check(TEXT("Five-bomber ingress clears real terrain in all twenty-four sampled approach directions"),
            IngressSamples > 20000 && IngressTraceMisses == 0 && IngressMinimumClearance >= 15000.);
        Check(TEXT("Shared bomber ingress altitude adapts to the actual mountain approaches"),
            HighestIngress >= 200000. && HighestIngress-LowestIngress >= 30000.);
        Observations.Add(FString::Printf(TEXT("{\"case\":\"five_bomber_approach_sweep\",\"directions\":24,\"slots_per_formation\":5,\"terrain_samples\":%d,\"trace_misses\":%d,\"minimum_clearance_cm\":%.3f,\"lowest_ingress_altitude_cm\":%.3f,\"highest_ingress_altitude_cm\":%.3f}"),
            IngressSamples,IngressTraceMisses,IngressMinimumClearance,LowestIngress,HighestIngress));

        // Choose a genuinely difficult approach from collision measurements,
        // not a test-only mound or a path that happens to cross flat ground.
        FVector Outward = FVector(1, 0, 0);
        double RidgeHeight = -DBL_MAX, RidgeRadius = 0.;
        for (int32 Direction = 0; Direction < 24; ++Direction)
        {
            const double Angle = Direction * UE_TWO_PI / 24.;
            const FVector Ray(FMath::Cos(Angle), FMath::Sin(Angle), 0);
            for (int32 Radius = 350000; Radius <= 750000; Radius += 25000)
            {
                double Height = 0.;
                if (TerrainHeight(Ray*Radius, Height) && Height > RidgeHeight) { RidgeHeight = Height; RidgeRadius = Radius; Outward = Ray; }
            }
        }
        Check(TEXT("Enemy clearance fixture traverses an actual ridge above 1800 metres"), RidgeHeight > 180000.);
        Mode->Player->SetActorLocation(FVector(0,0,400000));
        Mode->AirportHealth = Mode->AirportMaxHealth;
        const float SoloInitialHealth = Mode->AirportHealth;
        const FVector SoloOriginXY = Outward*850000.;
        const float SoloAltitude = Mode->GetBomberIngressAltitude(SoloOriginXY, 1);
        ADSEnemyAircraft* Bomber = MakeEnemy(true, SoloOriginXY+FVector(0,0,SoloAltitude), -Outward);
        if (!Bomber) { Check(TEXT("Single bomber clearance fixture spawned"), false); return; }
        double MinimumClearance = DBL_MAX, MaxStepError = 0.;
        int32 Samples = 0, TraceMisses = 0;
        constexpr float Step = .05f;
        for (int32 I = 0; I < 1500 && !Bomber->bEgress && !Bomber->IsActorBeingDestroyed(); ++I)
        {
            const FVector Before = Bomber->GetActorLocation();
            Bomber->Tick(Step);
            TickSpawnedBombs(Step);
            const FVector After = Bomber->GetActorLocation();
            MaxStepError = FMath::Max(MaxStepError, FMath::Abs(FVector::Distance(Before,After)-Bomber->Speed*100.*Step));
            double Ground = 0.;
            if (TerrainHeight(After, Ground)) { MinimumClearance = FMath::Min(MinimumClearance, After.Z-Ground); ++Samples; }
            else ++TraceMisses;
        }
        Check(TEXT("Single bomber crosses the ridge without intersecting terrain or teleporting"), Samples > 300 && TraceMisses == 0 && MinimumClearance > 2500. && MaxStepError < 2.);
        Check(TEXT("Terrain-aware single bomber still reaches the airport and releases three bombs"), Bomber->BombsDropped == 3 && Bomber->bEgress);
        SettleSpawnedBombs(Step);
        Check(TEXT("Single bomber payload falls onto the airport and causes actual damage"), Mode->AirportHealth < SoloInitialHealth && SoloInitialHealth-Mode->AirportHealth >= 300.f);
        Observations.Add(FString::Printf(TEXT("{\"case\":\"single_bomber_ridge\",\"ridge_height_cm\":%.3f,\"ridge_radius_cm\":%.3f,\"approach_xy\":[%.6f,%.6f],\"samples\":%d,\"trace_misses\":%d,\"minimum_clearance_cm\":%.3f,\"maximum_movement_error_cm\":%.6f,\"bombs\":%d,\"egress\":%s,\"airport_health_before\":%.3f,\"airport_health_after_impacts\":%.3f}"),
            RidgeHeight, RidgeRadius, Outward.X, Outward.Y, Samples, TraceMisses, MinimumClearance, MaxStepError, Bomber->BombsDropped, Bomber->bEgress?TEXT("true"):TEXT("false"),SoloInitialHealth,Mode->AirportHealth));
        ClearCombatActors();

        const FVector PlayerPoint = Outward*FMath::Max(200000., RidgeRadius-180000.);
        double PlayerGround = 0.;
        TerrainHeight(PlayerPoint, PlayerGround);
        Mode->Player->SetActorLocation(PlayerPoint+FVector(0,0,PlayerGround+3000.));
        Mode->Player->SetActorRotation((-Outward).Rotation());
        Mode->Player->Speed = 0.f;
        ADSEnemyAircraft* Fighter = MakeEnemy(false, SoloOriginXY+FVector(0,0,SoloAltitude), -Outward);
        if (!Fighter) { Check(TEXT("Fighter clearance fixture spawned"), false); return; }
        MinimumClearance = DBL_MAX; MaxStepError = 0.; Samples = 0; TraceMisses = 0;
        double ClosestTarget = DBL_MAX, HighestTraversedGround = -DBL_MAX;
        for (int32 I = 0; I < 800 && !Fighter->IsActorBeingDestroyed(); ++I)
        {
            const FVector Before = Fighter->GetActorLocation();
            Fighter->Tick(Step);
            const FVector After = Fighter->GetActorLocation();
            MaxStepError = FMath::Max(MaxStepError, FMath::Abs(FVector::Distance(Before,After)-Fighter->Speed*100.*Step));
            double Ground = 0.;
            if (TerrainHeight(After, Ground))
            {
                MinimumClearance = FMath::Min(MinimumClearance, After.Z-Ground);
                HighestTraversedGround = FMath::Max(HighestTraversedGround, Ground); ++Samples;
            }
            else ++TraceMisses;
            ClosestTarget = FMath::Min(ClosestTarget, FVector::Dist2D(After, PlayerPoint));
        }
        Check(TEXT("Pursuing fighter crosses elevated terrain with physical clearance and ordinary movement"),
            Samples >= 700 && TraceMisses == 0 && HighestTraversedGround > 180000. && MinimumClearance > 2500. && MaxStepError < 2. && ClosestTarget < 100000.);
        Observations.Add(FString::Printf(TEXT("{\"case\":\"fighter_ridge\",\"samples\":%d,\"trace_misses\":%d,\"highest_traversed_ground_cm\":%.3f,\"minimum_clearance_cm\":%.3f,\"maximum_movement_error_cm\":%.6f,\"closest_target_horizontal_cm\":%.3f}"),
            Samples, TraceMisses, HighestTraversedGround, MinimumClearance, MaxStepError, ClosestTarget));
        ClearCombatActors();
        Mode->Player->SetActorLocation(FVector(0,0,400000));
        Mode->AirportHealth = Mode->AirportMaxHealth;
        const float GroupInitialHealth = Mode->AirportHealth;

        const FVector FormationOrigin = SoloOriginXY+FVector(0,0,Mode->GetBomberIngressAltitude(SoloOriginXY,3)), Heading = -Outward;
        const FVector Right = FVector::CrossProduct(FVector::UpVector, Heading);
        TArray<ADSEnemyAircraft*> Group;
        for (int32 Slot = 0; Slot < 3; ++Slot)
        {
            const FVector Offset = ADSEnemyAircraft::GetFormationSlotOffset(Slot);
            ADSEnemyAircraft* Member = MakeEnemy(true, FormationOrigin+Heading*Offset.X+Right*Offset.Y, Heading);
            if (Member) { Member->InitializeFormation(904,Slot,3,FormationOrigin,Heading); Group.Add(Member); }
        }
        bool bAllReleased = false, bBombsOnlyAfterRelease = true;
        double MaximumSlotError = 0., GroupMinimumClearance = DBL_MAX;
        int32 ActiveSamples = 0, GroupTraceMisses = 0;
        for (int32 I = 0; I < 1100 && Group.Num() == 3; ++I)
        {
            int32 Released = 0, Egress = 0;
            for (ADSEnemyAircraft* Member : Group)
            {
                Member->Tick(Step);
                const FVector Position = Member->GetActorLocation();
                double Ground = 0.;
                if (TerrainHeight(Position,Ground)) GroupMinimumClearance = FMath::Min(GroupMinimumClearance, Position.Z-Ground);
                else ++GroupTraceMisses;
                if (Member->bFormationActive)
                {
                    const FVector Offset = ADSEnemyAircraft::GetFormationSlotOffset(Member->FormationSlot);
                    const FVector Expected = Member->GetFormationAnchor()+Heading*Offset.X+Right*Offset.Y;
                    MaximumSlotError = FMath::Max(MaximumSlotError,FVector::Distance(Position,Expected)); ++ActiveSamples;
                }
                else ++Released;
                bBombsOnlyAfterRelease &= !(Member->bFormationActive && Member->BombsDropped > 0);
                if (Member->bEgress && Member->BombsDropped == 3) ++Egress;
            }
            TickSpawnedBombs(Step);
            bAllReleased |= Released == 3;
            if (Egress == 3) break;
        }
        int32 FinalEgress = 0;
        for (const ADSEnemyAircraft* Member : Group) if (Member->bEgress && Member->BombsDropped == 3) ++FinalEgress;
        Check(TEXT("Three-bomber triangular slots remain intact and terrain-clear during ridge ingress"),
            Group.Num()==3 && ActiveSamples>900 && GroupTraceMisses==0 && MaximumSlotError<3000. && GroupMinimumClearance>2500.);
        Check(TEXT("Formation still releases near the airport before all members bomb and depart"), bAllReleased && bBombsOnlyAfterRelease && FinalEgress==3);
        SettleSpawnedBombs(Step);
        Check(TEXT("Released formation payloads fall onto the airport and cause actual damage"), Mode->AirportHealth < GroupInitialHealth && GroupInitialHealth-Mode->AirportHealth >= 300.f);
        Observations.Add(FString::Printf(TEXT("{\"case\":\"formation_ridge\",\"members\":%d,\"active_slot_samples\":%d,\"maximum_slot_error_cm\":%.3f,\"minimum_clearance_cm\":%.3f,\"trace_misses\":%d,\"members_bombed_and_departing\":%d,\"airport_health_before\":%.3f,\"airport_health_after_impacts\":%.3f}"),
            Group.Num(), ActiveSamples, MaximumSlotError, GroupMinimumClearance, GroupTraceMisses, FinalEgress,GroupInitialHealth,Mode->AirportHealth));
        ClearCombatActors();
    }

    const TArray<TWeakObjectPtr<AStaticMeshActor>>& CategoryActors(int32 Index) const
    {
        return Index == 0 ? Town : Index == 1 ? Water : Vegetation;
    }

    const TCHAR* CategoryName() const { return VisualCase == 0 ? TEXT("town") : VisualCase == 1 ? TEXT("water") : TEXT("vegetation"); }

    void SetCategoryVisible(int32 Index, bool bVisible)
    {
        for (const TWeakObjectPtr<AStaticMeshActor>& Actor : CategoryActors(Index))
            if (Actor.IsValid()) Actor->SetActorHiddenInGame(!bVisible);
    }

    bool CategoryVisibilityMatches(bool bVisible) const
    {
        for (int32 I = 0; I < 3; ++I)
            for (const TWeakObjectPtr<AStaticMeshActor>& Actor : CategoryActors(I))
                if (!Actor.IsValid() || Actor->IsHidden() != !(bVisible && I == VisualCase)) return false;
        return true;
    }

    void ResetStageClock() { StageTime = World->GetTimeSeconds(); StageFrame = GFrameCounter; }

    void BeginVisualCase()
    {
        for (int32 I = 0; I < 3; ++I) SetCategoryVisible(I, false);
        FVector Eye = VisualCase == 0 ? FVector(0,180000,85000) : FVector(-290000,-260000,43000);
        FVector Target = VisualCase == 0 ? FVector(0,260000,6000) : LakeCenters[0];
        if (VisualCase == 2)
        {
            // Observe a real tree cluster at a readable flight distance. A
            // whole-lake rectangle would dilute slender foliage into its sand.
            VegetationViewBounds = FBox(ForceInit);
            TArray<AStaticMeshActor*> Selected;
            for (int32 I = 0; I < 6; ++I)
            {
                AStaticMeshActor* Nearest = nullptr;
                double NearestDistance = DBL_MAX;
                for (const TWeakObjectPtr<AStaticMeshActor>& Actor : Vegetation)
                {
                    if (!Actor.IsValid() || Selected.Contains(Actor.Get())) continue;
                    const FBoxSphereBounds& Bounds = Actor->GetStaticMeshComponent()->Bounds;
                    if (Bounds.BoxExtent.Z < 350. || FVector::Dist2D(Bounds.Origin,LakeCenters[0]) > 100000.) continue;
                    const double Distance = FVector::DistSquared2D(Bounds.Origin,Eye);
                    if (Distance < NearestDistance) { NearestDistance=Distance; Nearest=Actor.Get(); }
                }
                if (!Nearest) break;
                Selected.Add(Nearest);
                VegetationViewBounds += Nearest->GetStaticMeshComponent()->Bounds.GetBox();
            }
            Check(TEXT("The foliage observation contains at least four real oasis trees"), Selected.Num() >= 4);
            if (VegetationViewBounds.IsValid)
            {
                Target=VegetationViewBounds.GetCenter();
                const double Distance=FMath::Max(18000.,VegetationViewBounds.GetExtent().Size2D()*2.5);
                Eye=Target+FVector(0,-Distance,Distance*.8);
            }
        }
        Camera->SetActorLocationAndRotation(Eye,(Target-Eye).Rotation());
        PC->SetViewTarget(Camera.Get());
        if (PC->PlayerCameraManager) PC->PlayerCameraManager->UpdateCamera(0.f);
        Observations.Add(FString::Printf(TEXT("{\"case\":\"%s_view\",\"camera_cm\":[%.3f,%.3f,%.3f],\"target_cm\":[%.3f,%.3f,%.3f]}"),
            CategoryName(),Eye.X,Eye.Y,Eye.Z,Target.X,Target.Y,Target.Z));
        Stage = EStage::Baseline;
        ResetStageClock();
    }

    bool BuildObservationRegion()
    {
        FBox Bounds(ForceInit);
        const FVector Center = VisualCase == 0 ? FVector(0,260000,6000) : LakeCenters[0];
        if (VisualCase == 2) Bounds=VegetationViewBounds;
        else
            for (const TWeakObjectPtr<AStaticMeshActor>& Actor : CategoryActors(VisualCase))
                if (Actor.IsValid() && FVector::Dist2D(Actor->GetStaticMeshComponent()->Bounds.Origin,Center) < 100000.)
                    Bounds += Actor->GetStaticMeshComponent()->Bounds.GetBox();
        if (!Bounds.IsValid) return false;
        FVector2D Minimum(DBL_MAX,DBL_MAX), Maximum(-DBL_MAX,-DBL_MAX);
        for (int32 Corner = 0; Corner < 8; ++Corner)
        {
            const FVector Point((Corner&1)?Bounds.Max.X:Bounds.Min.X,(Corner&2)?Bounds.Max.Y:Bounds.Min.Y,(Corner&4)?Bounds.Max.Z:Bounds.Min.Z);
            FVector2D Screen;
            if (!PC->ProjectWorldLocationToScreen(Point,Screen)) continue;
            Minimum.X=FMath::Min(Minimum.X,Screen.X); Minimum.Y=FMath::Min(Minimum.Y,Screen.Y);
            Maximum.X=FMath::Max(Maximum.X,Screen.X); Maximum.Y=FMath::Max(Maximum.Y,Screen.Y);
        }
        if (Minimum.X==DBL_MAX) return false;
        ObservationRegion=FIntRect(FMath::Clamp(FMath::FloorToInt(Minimum.X)-8,0,ViewSize.X),FMath::Clamp(FMath::FloorToInt(Minimum.Y)-8,0,ViewSize.Y),
            FMath::Clamp(FMath::CeilToInt(Maximum.X)+8,0,ViewSize.X),FMath::Clamp(FMath::CeilToInt(Maximum.Y)+8,0,ViewSize.Y));
        return ObservationRegion.Width()>40 && ObservationRegion.Height()>40 && ObservationRegion.Area()>5000;
    }

    EReadback ReadViewport(TArray<FColor>& Pixels)
    {
        if (bReadbackReady)
        {
            bReadbackReady=false;
            if (!ReadbackError.IsEmpty()) return EReadback::Failed;
            Pixels=MoveTemp(PendingPixels); return EReadback::Ready;
        }
        if (!bReadbackRequested)
        {
            PendingPixels.Reset(); ReadbackError.Empty(); ReadbackRequestedAt=FPlatformTime::Seconds(); bReadbackRequested=true;
        }
        else if (FPlatformTime::Seconds()-ReadbackRequestedAt>5.)
        {
            ReadbackError=TEXT("The actual viewport did not complete the pending readback within five seconds.");
            return EReadback::Failed;
        }
        return EReadback::Pending;
    }

    void AfterViewportRendered(FViewport* Viewport)
    {
        if (!bReadbackRequested || bFinished || !GEngine || !GEngine->GameViewport || Viewport!=GEngine->GameViewport->Viewport) return;
        bReadbackRequested=false; bReadbackReady=true; ViewSize=Viewport->GetSizeXY();
        // The scene back buffer is still valid here, after the canvas flush.
        if (ViewSize.X<1280 || ViewSize.Y<720 || !Viewport->ReadPixels(PendingPixels) || PendingPixels.Num()!=ViewSize.X*ViewSize.Y)
        { ReadbackError=TEXT("Environment viewport readback failed or had unexpected dimensions."); return; }
        int32 NonBlack=0, Different=0;
        const FColor First=PendingPixels[0];
        for (const FColor& Pixel : PendingPixels)
        {
            if (FMath::Max3(Pixel.R,Pixel.G,Pixel.B)>=8) ++NonBlack;
            if (FMath::Max3(FMath::Abs(int32(Pixel.R)-First.R),FMath::Abs(int32(Pixel.G)-First.G),FMath::Abs(int32(Pixel.B)-First.B))>=8) ++Different;
        }
        const bool bScene=NonBlack>PendingPixels.Num()/4 && Different>PendingPixels.Num()/100;
        Observations.Add(FString::Printf(TEXT("{\"case\":\"viewport_readback\",\"category\":\"%s\",\"phase\":\"%s\",\"frame\":%llu,\"nonblack_pixels\":%d,\"pixels_different_from_first\":%d,\"valid_scene\":%s}"),
            CategoryName(),Stage==EStage::Baseline?TEXT("baseline"):TEXT("visible"),static_cast<unsigned long long>(GFrameCounter),NonBlack,Different,bScene?TEXT("true"):TEXT("false")));
        if (!bScene) ReadbackError=TEXT("A blank or constant framebuffer cannot substantiate environment acceptance.");
    }

    void CompareRegion()
    {
        const auto Difference=[this](int32 X,int32 Y)
        {
            const int32 Index=Y*ViewSize.X+X;
            const FColor& A=VisiblePixels[Index]; const FColor& B=BaselinePixels[Index];
            return FMath::Max3(FMath::Abs(int32(A.R)-B.R),FMath::Abs(int32(A.G)-B.G),FMath::Abs(int32(A.B)-B.B));
        };
        int32 Changed=0,Peak=0;
        double Sum=0.;
        for (int32 Y=ObservationRegion.Min.Y;Y<ObservationRegion.Max.Y;++Y)
            for (int32 X=ObservationRegion.Min.X;X<ObservationRegion.Max.X;++X)
            {
                const int32 Delta=Difference(X,Y); Sum+=Delta; Peak=FMath::Max(Peak,Delta); if (Delta>=12) ++Changed;
            }
        // A patch outside the category bounds detects global exposure changes
        // and temporal shifts that must not be mistaken for local geometry.
        FIntRect Control(8,8,72,72);
        const FIntRect Candidates[] = {FIntRect(8,8,72,72),FIntRect(ViewSize.X-72,8,ViewSize.X-8,72),
            FIntRect(8,ViewSize.Y-72,72,ViewSize.Y-8),FIntRect(ViewSize.X-72,ViewSize.Y-72,ViewSize.X-8,ViewSize.Y-8)};
        bool bControlSeparate=false;
        for (const FIntRect& Candidate : Candidates)
            if (Candidate.Max.X<=ObservationRegion.Min.X || Candidate.Min.X>=ObservationRegion.Max.X || Candidate.Max.Y<=ObservationRegion.Min.Y || Candidate.Min.Y>=ObservationRegion.Max.Y)
            { Control=Candidate; bControlSeparate=true; break; }
        double ControlSum=0.;
        for (int32 Y=Control.Min.Y;Y<Control.Max.Y;++Y)
            for (int32 X=Control.Min.X;X<Control.Max.X;++X) ControlSum+=Difference(X,Y);
        const double Mean=Sum/FMath::Max(ObservationRegion.Area(),1), ControlMean=ControlSum/Control.Area();
        const int32 MinimumChanged=VisualCase==0?300:VisualCase==1?1000:150;
        const bool bVisible=bControlSeparate && Changed>=MinimumChanged && Peak>=30 && Mean>ControlMean+.35;
        Check(*FString::Printf(TEXT("Isolated %s visibly changes the actual viewport beyond background temporal variation"),CategoryName()),bVisible);
        Observations.Add(FString::Printf(TEXT("{\"case\":\"%s_contrast\",\"comparison\":\"isolated category versus all three categories hidden\",\"roi\":[%d,%d,%d,%d],\"control_roi\":[%d,%d,%d,%d],\"pixels_changed_at_least_12\":%d,\"required_changed_pixels\":%d,\"peak_channel_difference\":%d,\"mean_channel_difference\":%.6f,\"control_mean_difference\":%.6f}"),
            CategoryName(),ObservationRegion.Min.X,ObservationRegion.Min.Y,ObservationRegion.Max.X,ObservationRegion.Max.Y,
            Control.Min.X,Control.Min.Y,Control.Max.X,Control.Max.Y,Changed,MinimumChanged,Peak,Mean,ControlMean));
    }

    void RemoveDelegate()
    {
        if (ViewportRenderedHandle.IsValid()) UGameViewportClient::OnViewportRendered().Remove(ViewportRenderedHandle);
        ViewportRenderedHandle.Reset(); bReadbackRequested=false;
    }

    bool Finish(const TCHAR* Error=nullptr)
    {
        bFinished=true; RemoveDelegate();
        if (Error) Test->AddError(Error);
        for (const FHiddenState& State : HiddenStates) if (State.Actor.IsValid()) State.Actor->SetActorHiddenInGame(State.bHidden);
        if (bStateCaptured && Mode.IsValid())
        {
            Mode->ReturnToMenu(); Mode->SelectAircraft(OriginalSelection); Mode->SetActorTickEnabled(bOriginalModeTick);
            if (Mode->Player) { Mode->Player->SetActorTickEnabled(bOriginalPlayerTick); Mode->Player->SetActorHiddenInGame(bOriginalPlayerHidden); }
        }
        if (bStateCaptured && PC.IsValid())
        {
            PC->bAutoManageActiveCameraTarget=bOriginalAutoCamera;
            if (OriginalViewTarget.IsValid()) PC->SetViewTarget(OriginalViewTarget.Get());
            if (PC->GetHUD()) PC->GetHUD()->bShowHUD=bOriginalHUD;
        }
        if (Camera.IsValid()) Camera->Destroy();
        if (!OutputDir.IsEmpty())
        {
            const FString Report=FString::Printf(TEXT("{\"schema\":\"environment-acceptance/1\",\"status\":\"%s\",\"fixture\":\"real game map, complex terrain traces and normal enemy Tick movement; no AutoPlay\",\"visual_evidence\":\"numeric post-render viewport comparisons only; no human image inspection\",\"media_files_written\":0,\"viewport\":[%d,%d],\"checks\":[%s],\"observations\":[%s]}\n"),
                Test->HasAnyErrors()?TEXT("failed"):TEXT("passed"),ViewSize.X,ViewSize.Y,*FString::Join(Checks,TEXT(",")),*FString::Join(Observations,TEXT(",")));
            Test->TestTrue(TEXT("Environment JSON report saved"),FFileHelper::SaveStringToFile(Report,*(OutputDir/TEXT("report.json")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
            Test->AddInfo(TEXT("Environment acceptance JSON: ")+OutputDir);
        }
        return true;
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDSEnvironmentAcceptanceTest,"DuneSentinel.Rendering.EnvironmentAcceptance",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter|EAutomationTestFlags::NonNullRHI)

bool FDSEnvironmentAcceptanceTest::RunTest(const FString& Parameters)
{
    if (!FParse::Param(FCommandLine::Get(),TEXT("DSEnvironmentValidation")) ||
        FParse::Param(FCommandLine::Get(),TEXT("AutoPlay")) || FParse::Param(FCommandLine::Get(),TEXT("CaptureFrames")))
    {
        AddError(TEXT("Use dedicated -game -DSEnvironmentValidation without AutoPlay or CaptureFrames on DesertDefense."));
        return false;
    }
    ADD_LATENT_AUTOMATION_COMMAND(FDSEnvironmentAcceptanceCommand(this));
    return true;
}
#endif
