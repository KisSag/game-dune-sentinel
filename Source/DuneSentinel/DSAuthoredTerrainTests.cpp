#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "DSDesertScatter.h"
#include "DSEnemyAircraft.h"
#include "DSFlightPawn.h"
#include "DSGameMode.h"
#include "DSProjectile.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#include "LandscapeHeightfieldCollisionComponent.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RHI.h"
#include "Serialization/JsonSerializer.h"
#include "UnrealClient.h"

namespace
{
using FReviewObject = TSharedPtr<FJsonObject>;

FReviewObject ReviewObject(const FString& Name)
{
    FReviewObject Value = MakeShared<FJsonObject>();
    Value->SetStringField(TEXT("name"), Name);
    return Value;
}

TArray<TSharedPtr<FJsonValue>> ReviewVector(const FVector& Value)
{
    return {MakeShared<FJsonValueNumber>(Value.X), MakeShared<FJsonValueNumber>(Value.Y), MakeShared<FJsonValueNumber>(Value.Z)};
}

// The fixture uses the real map, renderer and Chaos heightfield. Its images
// remain in memory; the report contains measurements, not an art certification.
class FDSAuthoredTerrainReview final : public IAutomationLatentCommand
{
public:
    explicit FDSAuthoredTerrainReview(FAutomationTestBase* InTest) : Test(InTest) {}
    virtual ~FDSAuthoredTerrainReview() override { Restore(); }

    virtual bool Update() override
    {
        if (bFinished) return true;
        if (GetCurrentRunTime() > 150.) return Finish(TEXT("Authored terrain review exceeded 150 seconds."));
        if (!bInitialized) return Initialize();
        if (!World.IsValid() || !PC.IsValid() || !Camera.IsValid()) return Finish(TEXT("The review world or camera became unavailable."));
        if (Stage == EStage::Hold && FPlatformTime::Seconds() >= HoldUntil)
        {
            if (++ViewIndex == 4) return Finish();
            if (!BeginView()) return Finish(TEXT("A flight view could not find terrain under the camera and target."));
        }
        return false;
    }

private:
    enum class EStage { Warmup, Measure, Hold };
    struct FActorState
    {
        TWeakObjectPtr<AActor> Actor;
        bool bTick = false, bHidden = false;
        float LifeSpan = 0.f;
    };
    FAutomationTestBase* Test;
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<APlayerController> PC;
    TWeakObjectPtr<ACameraActor> Camera;
    TWeakObjectPtr<AActor> OriginalView;
    TWeakObjectPtr<ALandscape> Terrain;
    TArray<FActorState> Frozen;
    TArray<TSharedPtr<FJsonValue>> Checks, Observations;
    TArray<uint8> SourceHeight;
    TArray<double> FrameMilliseconds;
    FDelegateHandle RenderedHandle;
    FReviewObject CurrentView;
    FString OutputPath;
    EStage Stage = EStage::Warmup;
    int32 ViewIndex = 0;
    double StageStart = 0., LastRenderedTime = 0., HoldUntil = 0.;
    bool bInitialized = false, bFinished = false, bRestored = false, bControllerCaptured = false;
    bool bOriginalAutoCamera = false, bOriginalHUD = false;
    bool bVisualReview = FParse::Param(FCommandLine::Get(), TEXT("DSVisualReview"));
    static constexpr int32 HeightWidth = 2017;
    static constexpr double VertexSpacing = 2000000. / 2016.;

    void Check(const FString& Name, bool bPassed)
    {
        Test->TestTrue(Name, bPassed);
        FReviewObject Result = ReviewObject(Name);
        Result->SetBoolField(TEXT("passed"), bPassed);
        Checks.Add(MakeShared<FJsonValueObject>(Result));
    }

    bool Initialize()
    {
        if (!GEngine || !GEngine->GameViewport || !GEngine->GameViewport->Viewport) return false;
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
            if (Context.WorldType == EWorldType::Game && Context.World() && Context.World()->HasBegunPlay()) World = Context.World();
        if (!World.IsValid()) return false;
        if (!World->GetMapName().Contains(TEXT("DesertDefense"))) return Finish(TEXT("Run the review on the DesertDefense game map."));
        PC = UGameplayStatics::GetPlayerController(World.Get(), 0);
        if (!PC.IsValid() || !World->GetAuthGameMode<ADSGameMode>()) return Finish(TEXT("The real game controller and mode are required."));
        const FString Directory = FPaths::ProjectSavedDir() / TEXT("Validation/AuthoredDesert");
        IFileManager::Get().MakeDirectory(*Directory, true);
        OutputPath = Directory / (TEXT("runtime-") + FDateTime::UtcNow().ToString(TEXT("%Y%m%dT%H%M%SZ")) +
            TEXT("-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8) + TEXT(".json"));

        // Preserve the menu/sortie and every live combat actor. No reset, kill,
        // score change or save operation is involved in this observation.
        for (TActorIterator<AActor> It(World.Get()); It; ++It)
        {
            AActor* Actor = *It;
            const bool bCombat = Actor->IsA<ADSFlightPawn>() || Actor->IsA<ADSEnemyAircraft>() ||
                Actor->IsA<ADSProjectile>() || Actor->IsA<ADSBomb>();
            if (!bCombat && !Actor->IsA<ADSGameMode>()) continue;
            Frozen.Add({Actor, Actor->IsActorTickEnabled(), Actor->IsHidden(), Actor->GetLifeSpan()});
            Actor->SetActorTickEnabled(false);
            Actor->SetLifeSpan(0.f);
            if (bCombat) Actor->SetActorHiddenInGame(true);
        }
        OriginalView = PC->GetViewTarget();
        bOriginalAutoCamera = PC->bAutoManageActiveCameraTarget;
        if (PC->GetHUD()) { bOriginalHUD = PC->GetHUD()->bShowHUD; PC->GetHUD()->bShowHUD = false; }
        bControllerCaptured = true;
        PC->bAutoManageActiveCameraTarget = false;
        InspectTerrain();
        if (!Terrain.IsValid()) return Finish(TEXT("The native authored Landscape was not found."));
        InspectCollision();
        InspectScatter();
        Camera = World->SpawnActor<ACameraActor>();
        if (!Camera.IsValid()) return Finish(TEXT("The review camera could not be created."));
        Camera->GetCameraComponent()->SetFieldOfView(75.f);
        RenderedHandle = UGameViewportClient::OnViewportRendered().AddRaw(this, &FDSAuthoredTerrainReview::AfterRendered);
        bInitialized = true;
        if (!BeginView()) return Finish(TEXT("The first flight view could not find terrain."));
        return false;
    }

    bool HasSourceTextures(UPrimitiveComponent* Component, const FString& Prefix) const
    {
        if (!Component) return false;
        for (int32 Slot = 0; Slot < Component->GetNumMaterials(); ++Slot)
        {
            UMaterialInterface* Material = Component->GetMaterial(Slot);
            if (!Material) continue;
            TArray<UTexture*> Textures;
            Material->GetUsedTextures(Textures, EMaterialQualityLevel::High,
                GetFeatureLevelShaderPlatform_Checked(World->GetFeatureLevel()));
            for (UTexture* Texture : Textures) if (Texture && Texture->GetPathName().StartsWith(Prefix)) return true;
        }
        return false;
    }

    void InspectTerrain()
    {
        int32 TaggedRoots = 0, NativeLandscapes = 0;
        for (TActorIterator<AActor> It(World.Get()); It; ++It)
        {
            if (It->IsA<ALandscape>()) ++NativeLandscapes;
            if (!It->ActorHasTag(TEXT("DSEnvironmentTerrain")) || It->ActorHasTag(TEXT("DSEnvironmentTerrainProxy"))) continue;
            ++TaggedRoots;
            if (ALandscape* Native = Cast<ALandscape>(*It)) Terrain = Native;
        }
        Check(TEXT("One native Landscape is the single tagged terrain root"), TaggedRoots == 1 && NativeLandscapes == 1 && Terrain.IsValid());
        if (!Terrain.IsValid()) return;
        TSet<FIntPoint> RenderSections, CollisionSections;
        int32 ValidRender = 0, ValidCollision = 0, Textured = 0;
        for (ULandscapeComponent* Component : Terrain->LandscapeComponents)
        {
            if (!Component) continue;
            RenderSections.Add(Component->GetSectionBase());
            if (Component->IsRegistered() && Component->ComponentSizeQuads == 126 && Component->SubsectionSizeQuads == 63 &&
                Component->NumSubsections == 2 && Component->CollisionMipLevel == 0) ++ValidRender;
            if (HasSourceTextures(Component, TEXT("/Game/LandscapePackTwo/"))) ++Textured;
        }
        for (ULandscapeHeightfieldCollisionComponent* Component : Terrain->CollisionComponents)
        {
            if (!Component) continue;
            CollisionSections.Add(Component->GetSectionBase());
            if (Component->IsRegistered() && Component->IsPhysicsStateCreated() && Component->IsQueryCollisionEnabled() &&
                Component->GetCollisionObjectType() == ECC_WorldStatic && Component->CollisionSizeQuads == 126) ++ValidCollision;
        }
        bool bCompleteGrid = RenderSections.Num() == 256 && CollisionSections.Num() == 256;
        for (int32 Y = 0; Y < 16; ++Y) for (int32 X = 0; X < 16; ++X)
            bCompleteGrid &= RenderSections.Contains(FIntPoint(X * 126, Y * 126)) && CollisionSections.Contains(FIntPoint(X * 126, Y * 126));
        Check(TEXT("All 16 by 16 native render and full resolution collision sections are resident"),
            bCompleteGrid && ValidRender == 256 && ValidCollision == 256 && Terrain->CollisionMipLevel == 0);
        Check(TEXT("Landscape transform covers the full unrotated 20 km arena"),
            Terrain->GetActorLocation().Equals(FVector(-1000000., -1000000., 120000.), .1) &&
            Terrain->GetActorScale3D().Equals(FVector(VertexSpacing, VertexSpacing, 640.), .001) && Terrain->GetActorRotation().IsNearlyZero(.001));
        Check(TEXT("Every terrain section uses acquired source textures in the authored material"), Textured == 256 &&
            Terrain->LandscapeMaterial && Terrain->LandscapeMaterial->GetPathName().Contains(TEXT("/AuthoredDesert/M_AuthoredDesert")));
        FReviewObject Result = ReviewObject(TEXT("native_landscape"));
        Result->SetNumberField(TEXT("render_components"), Terrain->LandscapeComponents.Num());
        Result->SetNumberField(TEXT("resident_full_resolution_collision_components"), ValidCollision);
        Result->SetNumberField(TEXT("textured_components"), Textured);
        Result->SetArrayField(TEXT("location_cm"), ReviewVector(Terrain->GetActorLocation()));
        Result->SetArrayField(TEXT("scale_cm"), ReviewVector(Terrain->GetActorScale3D()));
        Observations.Add(MakeShared<FJsonValueObject>(Result));
    }

    bool GroundAt(const FVector& Position, double& Height) const
    {
        TArray<FHitResult> Hits;
        FCollisionQueryParams Query(SCENE_QUERY_STAT(DSAuthoredTerrainReview), true);
        Query.bIgnoreTouches = false;
        World->LineTraceMultiByObjectType(Hits, FVector(Position.X, Position.Y, 1500000.), FVector(Position.X, Position.Y, -50000.),
            FCollisionObjectQueryParams(ECC_WorldStatic), Query);
        bool bFound = false;
        Height = -DBL_MAX;
        for (const FHitResult& Hit : Hits)
            if (Hit.GetActor() == Terrain.Get() && !Hit.bStartPenetrating && FMath::IsFinite(Hit.ImpactPoint.Z))
            { Height = FMath::Max(Height, Hit.ImpactPoint.Z); bFound = true; }
        return bFound;
    }

    double SourceAt(double X, double Y) const
    {
        const double GridX = (X + 1000000.) / VertexSpacing, GridY = (Y + 1000000.) / VertexSpacing;
        const int32 I = FMath::Clamp(FMath::FloorToInt(GridX), 0, HeightWidth - 2);
        const int32 J = FMath::Clamp(FMath::FloorToInt(GridY), 0, HeightWidth - 2);
        const double U = GridX - I, V = GridY - J;
        const auto Height = [this](int32 Col, int32 Row)
        {
            const int32 Byte = (Row * HeightWidth + Col) * 2;
            return 120000. + (int32(SourceHeight[Byte]) + int32(SourceHeight[Byte + 1]) * 256 - 32768) * 5.;
        };
        const double H00 = Height(I,J), H11 = Height(I+1,J+1);
        // Native heightfield topology uses the diagonal from 00 to 11.
        return U >= V ? H00 * (1.-U) + Height(I+1,J) * (U-V) + H11 * V :
            H00 * (1.-V) + Height(I,J+1) * (V-U) + H11 * U;
    }

    void InspectCollision()
    {
        const bool bSource = FFileHelper::LoadFileToArray(SourceHeight,
            *(FPaths::ProjectDir() / TEXT("RawAssets/Environment/AuthoredDesert/height.r16"))) && SourceHeight.Num() == HeightWidth * HeightWidth * 2;
        Check(TEXT("Retained authored heightfield is available for independent runtime comparison"), bSource);
        int32 Misses = 0, Samples = 0, BoundarySamples = 0;
        double Lowest = DBL_MAX, Highest = -DBL_MAX, MaxSourceError = 0.;
        for (int32 Y = 0; Y <= 64; ++Y) for (int32 X = 0; X <= 64; ++X)
        {
            const FVector Position(-999000. + X * 1998000. / 64., -999000. + Y * 1998000. / 64., 0.);
            ++Samples;
            if (X == 0 || Y == 0 || X == 64 || Y == 64) ++BoundarySamples;
            double Height;
            if (!GroundAt(Position, Height)) { ++Misses; continue; }
            Lowest = FMath::Min(Lowest, Height); Highest = FMath::Max(Highest, Height);
            if (bSource) MaxSourceError = FMath::Max(MaxSourceError, FMath::Abs(Height - SourceAt(Position.X, Position.Y)));
        }
        Check(TEXT("Runtime terrain traces cover a 65 by 65 grid including every arena edge at 9.99 km"), Misses == 0 && BoundarySamples == 256);
        Check(TEXT("Resident terrain collision matches authored triangle heights within five centimetres"), bSource && Misses == 0 && MaxSourceError <= 5.);
        Check(TEXT("Runtime terrain retains more than 1.8 km of authored relief"), Misses == 0 && Highest - Lowest > 180000.);
        FReviewObject Result = ReviewObject(TEXT("full_arena_collision"));
        Result->SetNumberField(TEXT("samples"), Samples); Result->SetNumberField(TEXT("boundary_samples"), BoundarySamples);
        Result->SetNumberField(TEXT("trace_misses"), Misses); Result->SetNumberField(TEXT("maximum_source_error_cm"), MaxSourceError);
        if (Samples > Misses) { Result->SetNumberField(TEXT("minimum_height_cm"), Lowest); Result->SetNumberField(TEXT("maximum_height_cm"), Highest); }
        Observations.Add(MakeShared<FJsonValueObject>(Result));
        const FVector Anchors[] = {FVector(0.,0.,2180.), FVector(0.,260000.,4800.),
            FVector(-270000.,-220000.,5200.), FVector(420000.,180000.,8400.), FVector(100000.,530000.,11000.)};
        const TCHAR* Names[] = {TEXT("airport"), TEXT("town"), TEXT("west_lake_bed"), TEXT("east_lake_bed"), TEXT("north_lake_bed")};
        for (int32 I = 0; I < UE_ARRAY_COUNT(Anchors); ++I)
        {
            double Height;
            const bool bHit = GroundAt(Anchors[I], Height);
            Check(FString::Printf(TEXT("%s retains its designed ground height"), Names[I]), bHit && FMath::Abs(Height - Anchors[I].Z) < 5.);
            FReviewObject Anchor = ReviewObject(Names[I]);
            Anchor->SetBoolField(TEXT("terrain_hit"), bHit); Anchor->SetNumberField(TEXT("expected_cm"), Anchors[I].Z);
            if (bHit) Anchor->SetNumberField(TEXT("observed_cm"), Height);
            Observations.Add(MakeShared<FJsonValueObject>(Anchor));
        }
    }

    void InspectScatter()
    {
        int32 Batches = 0, Instances = 0, ValidBatches = 0;
        TArray<TSharedPtr<FJsonValue>> Meshes;
        for (TActorIterator<ADSDesertScatter> It(World.Get()); It; ++It)
        {
            ++Batches;
            UHierarchicalInstancedStaticMeshComponent* Component = It->Instances;
            if (!Component) continue;
            const int32 Count = Component->GetInstanceCount();
            Instances += Count;
            UStaticMesh* Mesh = Component->GetStaticMesh();
            const bool bSource = Mesh && Mesh->GetPathName().StartsWith(TEXT("/Game/RealisticDesertPack/"));
            const bool bTextures = HasSourceTextures(Component, TEXT("/Game/RealisticDesertPack/"));
            if (bSource && bTextures && Count > 0 && Component->IsRegistered() && Component->IsVisible() && !It->IsHidden()) ++ValidBatches;
            FReviewObject Batch = ReviewObject(Mesh ? Mesh->GetPathName() : TEXT("missing_mesh"));
            Batch->SetNumberField(TEXT("instances"), Count); Batch->SetBoolField(TEXT("source_textures"), bTextures);
            Meshes.Add(MakeShared<FJsonValueObject>(Batch));
        }
        Check(TEXT("Acquired desert dressing uses resident textured native HISM batches"), Batches > 0 && Instances > 0 && ValidBatches == Batches);
        FReviewObject Result = ReviewObject(TEXT("native_desert_scatter"));
        Result->SetNumberField(TEXT("batches"), Batches); Result->SetNumberField(TEXT("instances"), Instances);
        Result->SetNumberField(TEXT("valid_source_batches"), ValidBatches); Result->SetArrayField(TEXT("meshes"), Meshes);
        Observations.Add(MakeShared<FJsonValueObject>(Result));
    }

    bool BeginView()
    {
        const TCHAR* Names[] = {TEXT("airport_departure"), TEXT("western_valley"), TEXT("southeastern_dunes"), TEXT("eastern_oasis")};
        const FVector Eyes[] = {FVector(-160000.,-12000.,40000.), FVector(-440000.,-240000.,45000.),
            FVector(650000.,-650000.,45000.), FVector(350000.,130000.,30000.)};
        const FVector Targets[] = {FVector(50000.,0.,8000.), FVector(-210000.,-170000.,5000.),
            FVector(400000.,-420000.,8000.), FVector(420000.,180000.,2500.)};
        FVector Eye = Eyes[ViewIndex], Target = Targets[ViewIndex];
        double EyeGround, TargetGround;
        if (!GroundAt(Eye, EyeGround) || !GroundAt(Target, TargetGround)) return false;
        Eye.Z += EyeGround; Target.Z += TargetGround;
        Check(FString(Names[ViewIndex]) + TEXT(" camera is above actual collision"), Eye.Z - EyeGround >= 30000.);
        Camera->SetActorLocationAndRotation(Eye, (Target-Eye).Rotation());
        PC->SetViewTarget(Camera.Get());
        if (PC->PlayerCameraManager) PC->PlayerCameraManager->UpdateCamera(0.f);
        CurrentView = ReviewObject(Names[ViewIndex]);
        CurrentView->SetArrayField(TEXT("camera_cm"), ReviewVector(Eye)); CurrentView->SetArrayField(TEXT("target_cm"), ReviewVector(Target));
        CurrentView->SetNumberField(TEXT("camera_ground_clearance_cm"), Eye.Z - EyeGround);
        CurrentView->SetNumberField(TEXT("warmup_seconds"), 3.); CurrentView->SetNumberField(TEXT("extra_observation_seconds"), bVisualReview ? 30. : 0.);
        Stage = EStage::Warmup; StageStart = FPlatformTime::Seconds(); FrameMilliseconds.Reset();
        Test->AddInfo(FString::Printf(TEXT("Authored terrain view %d/4: %s; warmup 3 s, sample 2 s, observation %d s."),
            ViewIndex+1, Names[ViewIndex], bVisualReview ? 30 : 0));
        return true;
    }

    void AfterRendered(FViewport* Viewport)
    {
        if (bFinished || !bInitialized || !GEngine || !GEngine->GameViewport || Viewport != GEngine->GameViewport->Viewport) return;
        const double Now = FPlatformTime::Seconds();
        if (Stage == EStage::Warmup && Now - StageStart >= 3.)
        {
            const FIntPoint Size = Viewport->GetSizeXY();
            TArray<FColor> Pixels;
            const bool bRead = Size.X > 0 && Size.Y > 0 && Viewport->ReadPixels(Pixels) && Pixels.Num() == Size.X * Size.Y;
            int32 NonBlack = 0, Different = 0;
            if (bRead)
            {
                const FColor First = Pixels[0];
                for (const FColor& Pixel : Pixels)
                {
                    if (FMath::Max3(Pixel.R, Pixel.G, Pixel.B) >= 8) ++NonBlack;
                    if (FMath::Max3(FMath::Abs(int32(Pixel.R)-First.R), FMath::Abs(int32(Pixel.G)-First.G), FMath::Abs(int32(Pixel.B)-First.B)) >= 8) ++Different;
                }
            }
            Check(CurrentView->GetStringField(TEXT("name")) + TEXT(" renders a nonconstant game view"), bRead && NonBlack > Pixels.Num()/4 && Different > Pixels.Num()/100);
            CurrentView->SetNumberField(TEXT("viewport_width"), Size.X); CurrentView->SetNumberField(TEXT("viewport_height"), Size.Y);
            CurrentView->SetNumberField(TEXT("nonblack_pixels"), NonBlack); CurrentView->SetNumberField(TEXT("nonconstant_pixels"), Different);
            // The single readback is outside the measured interval.
            Stage = EStage::Measure; StageStart = LastRenderedTime = FPlatformTime::Seconds();
        }
        else if (Stage == EStage::Measure)
        {
            FrameMilliseconds.Add((Now - LastRenderedTime) * 1000.); LastRenderedTime = Now;
            if (Now - StageStart < 2.) return;
            double Sum = 0.;
            for (double Value : FrameMilliseconds) Sum += Value;
            FrameMilliseconds.Sort();
            Check(CurrentView->GetStringField(TEXT("name")) + TEXT(" produced multiple rendered frames"), FrameMilliseconds.Num() >= 2);
            if (!FrameMilliseconds.IsEmpty())
            {
                CurrentView->SetNumberField(TEXT("frame_interval_mean_ms"), Sum / FrameMilliseconds.Num());
                CurrentView->SetNumberField(TEXT("frame_interval_p95_ms"), FrameMilliseconds[FMath::Clamp(FMath::CeilToInt(FrameMilliseconds.Num()*.95)-1, 0, FrameMilliseconds.Num()-1)]);
                CurrentView->SetNumberField(TEXT("frame_interval_max_ms"), FrameMilliseconds.Last());
            }
            CurrentView->SetNumberField(TEXT("sample_seconds"), Now - StageStart);
            CurrentView->SetNumberField(TEXT("sample_frame_intervals"), FrameMilliseconds.Num());
            CurrentView->SetStringField(TEXT("timing_scope"), TEXT("Wall-clock intervals between actual game viewport render notifications; includes frame pacing and CPU/GPU waits, not an isolated GPU timer. No performance threshold is asserted."));
            Observations.Add(MakeShared<FJsonValueObject>(CurrentView));
            Stage = EStage::Hold; HoldUntil = Now + (bVisualReview ? 30. : 0.);
        }
    }

    void Restore()
    {
        if (bRestored) return;
        bRestored = true;
        if (RenderedHandle.IsValid()) UGameViewportClient::OnViewportRendered().Remove(RenderedHandle);
        RenderedHandle.Reset();
        if (bControllerCaptured && PC.IsValid())
        {
            PC->bAutoManageActiveCameraTarget = bOriginalAutoCamera;
            if (OriginalView.IsValid()) PC->SetViewTarget(OriginalView.Get());
            if (PC->GetHUD()) PC->GetHUD()->bShowHUD = bOriginalHUD;
            if (PC->PlayerCameraManager) PC->PlayerCameraManager->UpdateCamera(0.f);
        }
        for (const FActorState& State : Frozen) if (State.Actor.IsValid())
        {
            State.Actor->SetActorTickEnabled(State.bTick); State.Actor->SetActorHiddenInGame(State.bHidden);
            State.Actor->SetLifeSpan(State.LifeSpan);
        }
        if (Camera.IsValid()) Camera->Destroy();
    }

    bool Finish(const TCHAR* Error = nullptr)
    {
        bFinished = true; Restore();
        if (Error) Test->AddError(Error);
        if (!OutputPath.IsEmpty())
        {
            FReviewObject Report = MakeShared<FJsonObject>();
            Report->SetStringField(TEXT("schema"), TEXT("authored-terrain-runtime-review/1"));
            Report->SetStringField(TEXT("status"), Test->HasAnyErrors() ? TEXT("failed") : TEXT("passed"));
            Report->SetStringField(TEXT("scope"), TEXT("Native Landscape collision, material/source presence, actual game camera views and numeric render/frame observations. These checks do not certify AAA artistic quality; no human visual verdict is inferred."));
            Report->SetBoolField(TEXT("visual_observation_hold_enabled"), bVisualReview);
            Report->SetNumberField(TEXT("media_files_written"), 0);
            Report->SetArrayField(TEXT("checks"), Checks); Report->SetArrayField(TEXT("observations"), Observations);
            if (Error) Report->SetStringField(TEXT("error"), Error);
            FString Json;
            FJsonSerializer::Serialize(Report.ToSharedRef(), TJsonWriterFactory<>::Create(&Json));
            Test->TestTrue(TEXT("Authored terrain runtime JSON saved"), FFileHelper::SaveStringToFile(Json, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
            Test->AddInfo(TEXT("Authored terrain review report: ") + OutputPath);
        }
        return true;
    }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDSAuthoredTerrainReviewTest, "DuneSentinel.Rendering.AuthoredTerrainReview",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FDSAuthoredTerrainReviewTest::RunTest(const FString& Parameters)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("DSEnvironmentValidation")) ||
        FParse::Param(FCommandLine::Get(), TEXT("AutoPlay")) || FParse::Param(FCommandLine::Get(), TEXT("CaptureFrames")))
    {
        AddError(TEXT("Use dedicated -game -DSEnvironmentValidation on DesertDefense without AutoPlay or CaptureFrames; optional -DSVisualReview holds each view for observation."));
        return false;
    }
    ADD_LATENT_AUTOMATION_COMMAND(FDSAuthoredTerrainReview(this));
    return true;
}
#endif
