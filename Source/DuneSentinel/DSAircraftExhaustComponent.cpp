#include "DSAircraftExhaustComponent.h"

#include "Components/MaterialBillboardComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

UDSAircraftExhaustComponent::UDSAircraftExhaustComponent()
{
    // Aircraft update thrust after movement. Attached visuals inherit full bank and
    // translation without another tick, independent movement or a trailing frame.
    PrimaryComponentTick.bCanEverTick = false;
}

bool UDSAircraftExhaustComponent::EnsureVisuals()
{
    const int32 NozzleCount = NozzleLocations.Num();
    if (NozzleCount < 1 || NozzleCount > MaximumNozzles) return false;
    if (Cores.Num() == NozzleCount && FlameCards.Num() == NozzleCount * CardsPerNozzle && HasReadyMaterials()) return true;
    if (!GetOwner() || !GetWorld() || !GetWorld()->IsGameWorld() || IsRunningCommandlet()) return false;

    FlameMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Art/Effects/M_AircraftExhaustFlame.M_AircraftExhaustFlame"));
    CoreMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Art/Effects/M_AircraftExhaustCore.M_AircraftExhaustCore"));
    UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
    if (!FlameMaterial || !CoreMaterial || !PlaneMesh) return false;

    // Selection can change the engine count. Rebuild only at configuration or
    // first initialization, never while updating engine power each frame.
    DestroyVisuals();
    FlameCards.Reserve(NozzleCount * CardsPerNozzle);
    Cores.Reserve(NozzleCount);
    FlameMIDs.Reserve(NozzleCount);
    CoreMIDs.Reserve(NozzleCount);
    for (int32 Nozzle = 0; Nozzle < NozzleCount; ++Nozzle)
    {
        UMaterialInstanceDynamic* FlameMID = UMaterialInstanceDynamic::Create(FlameMaterial, this);
        UMaterialInstanceDynamic* CoreMID = UMaterialInstanceDynamic::Create(CoreMaterial, this);
        FlameMID->SetVectorParameterValue(TEXT("Color"), FLinearColor::White);
        CoreMID->SetVectorParameterValue(TEXT("Color"), FLinearColor::White);
        FlameMID->SetScalarParameterValue(TEXT("Phase"), Nozzle * 1.73f);
        CoreMID->SetScalarParameterValue(TEXT("Phase"), Nozzle * 1.73f);
        FlameMIDs.Add(FlameMID);
        CoreMIDs.Add(CoreMID);

        for (int32 Card = 0; Card < CardsPerNozzle; ++Card)
        {
            UStaticMeshComponent* Plane = NewObject<UStaticMeshComponent>(GetOwner(),
                MakeUniqueObjectName(GetOwner(), UStaticMeshComponent::StaticClass(),
                    *FString::Printf(TEXT("EngineFlame%dCard%d"), Nozzle, Card)));
            GetOwner()->AddInstanceComponent(Plane);
            Plane->SetupAttachment(this);
            Plane->SetMobility(EComponentMobility::Movable);
            Plane->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            Plane->SetGenerateOverlapEvents(false);
            Plane->SetCastShadow(false);
            Plane->SetReceivesDecals(false);
            Plane->SetStaticMesh(PlaneMesh);
            Plane->SetMaterial(0, FlameMID);
            Plane->SetRelativeRotation(FRotator(0.f, 0.f, Card * 90.f));
            Plane->SetHiddenInGame(true);
            Plane->RegisterComponent();
            FlameCards.Add(Plane);
        }

        UMaterialBillboardComponent* Core = NewObject<UMaterialBillboardComponent>(GetOwner(),
            MakeUniqueObjectName(GetOwner(), UMaterialBillboardComponent::StaticClass(),
                *FString::Printf(TEXT("EngineHotCore%d"), Nozzle)));
        GetOwner()->AddInstanceComponent(Core);
        Core->SetupAttachment(this);
        Core->SetMobility(EComponentMobility::Movable);
        Core->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Core->SetGenerateOverlapEvents(false);
        Core->SetCastShadow(false);
        Core->SetReceivesDecals(false);
        Core->SetHiddenInGame(true);
        Core->AddElement(CoreMID, nullptr, false, NozzleRadiiCm[Nozzle], NozzleRadiiCm[Nozzle], nullptr);
        Core->RegisterComponent();
        Cores.Add(Core);
    }
    ApplyVisuals();
    return true;
}

void UDSAircraftExhaustComponent::ConfigureAircraft(const FString& MeshPath, bool bBomber)
{
    NozzleLocations.Reset();
    NozzleRotations.Reset();
    NozzleRadiiCm.Reset();
    const UStaticMeshComponent* Airframe = Cast<UStaticMeshComponent>(GetAttachParent());
    if (Airframe && Airframe->GetStaticMesh())
    {
        for (int32 Index = 1; Index <= MaximumNozzles; ++Index)
        {
            const UStaticMeshSocket* Socket = Airframe->GetStaticMesh()->FindSocket(
                *FString::Printf(TEXT("Exhaust_%02d"), Index));
            if (!Socket) continue;
            // +X points into the engine; the flame flows along socket -X.
            // Scale X is measured radius metadata, not an attachment scale.
            const float Radius = Socket->RelativeScale.X;
            if (!FMath::IsFinite(Radius) || Radius <= 0.f || Socket->RelativeLocation.ContainsNaN() ||
                Socket->RelativeRotation.ContainsNaN()) continue;
            const FQuat Orientation = Socket->RelativeRotation.Quaternion();
            NozzleLocations.Add(Socket->RelativeLocation - Orientation.GetForwardVector() * 2.f);
            NozzleRotations.Add(Orientation);
            NozzleRadiiCm.Add(Radius);
        }
    }

    if (NozzleLocations.IsEmpty())
    {
        // Rear engine-material rim centres measured in the original FBX files.
        // Original aircraft have two engines; retain their socket-free fallback.
        FVector Nozzle(-653.075f, 65.202f, -125.3795f);
        float NozzleRadiusCm = 39.139f;
        if (bBomber || MeshPath.Contains(TEXT("SM_bomber")))
        {
            Nozzle = FVector(-1025.289f, 70.812f, -114.9375f);
            NozzleRadiusCm = 49.8f;
        }
        else if (MeshPath.Contains(TEXT("SM_hostile")))
        {
            Nozzle = FVector(-941.817f, 76.244f, -76.495f);
            NozzleRadiusCm = 40.552f;
        }
        else if (MeshPath.Contains(TEXT("SM_lancer")))
        {
            Nozzle = FVector(-685.194f, 49.201f, -139.3305f);
            NozzleRadiusCm = 37.2f;
        }
        else if (MeshPath.Contains(TEXT("SM_falcon")))
        {
            Nozzle = FVector(-807.892f, 72.934f, -134.91f);
            NozzleRadiusCm = 50.9f;
        }
        NozzleLocations = {Nozzle, FVector(Nozzle.X, -Nozzle.Y, Nozzle.Z)};
        NozzleRotations = {FQuat::Identity, FQuat::Identity};
        NozzleRadiiCm = {NozzleRadiusCm, NozzleRadiusCm};
    }
    bConfigured = true;
    Thrust = .45f;
    if (EnsureVisuals())
    {
        ApplyVisuals();
        SetExhaustVisible(bExhaustVisible);
    }
}

void UDSAircraftExhaustComponent::ResetToCruise()
{
    Thrust = .45f;
    ApplyVisuals();
}

void UDSAircraftExhaustComponent::UpdateExhaust(float DeltaSeconds, float Throttle, float Brake)
{
    // Match flight's throttle-first priority, including high-G W+S turns. Engine
    // power is distinct from airspeed: pitching up must not extinguish a burner.
    const float Target = Throttle > .1f ? 1.f : (Brake > .1f ? 0.f : .45f);
    const float Alpha = 1.f - FMath::Exp(-7.f * FMath::Max(DeltaSeconds, 0.f));
    Thrust = FMath::Lerp(Thrust, Target, Alpha);
    if (FMath::Abs(Thrust - Target) < .0001f) Thrust = Target;
    ApplyVisuals();
}

void UDSAircraftExhaustComponent::SetCoreRadius(UMaterialBillboardComponent* Core, float Radius)
{
    if (!Core || Core->Elements.IsEmpty() || FMath::IsNearlyEqual(Core->Elements[0].BaseSizeX, Radius, .05f)) return;
    Core->Elements[0].BaseSizeX = Radius;
    Core->Elements[0].BaseSizeY = Radius;
    Core->UpdateBounds();
    Core->MarkRenderStateDirty();
}

void UDSAircraftExhaustComponent::ApplyVisuals()
{
    const int32 NozzleCount = NozzleLocations.Num();
    if (!bConfigured || NozzleCount == 0 || Cores.Num() != NozzleCount ||
        FlameCards.Num() != NozzleCount * CardsPerNozzle || !HasReadyMaterials()) return;
    const float Strength = GetGlowStrength();
    for (int32 Nozzle = 0; Nozzle < NozzleCount; ++Nozzle)
    {
        const float Length = NozzleRadiiCm[Nozzle] * FMath::Max(6.f, FMath::Lerp(3.5f, 23.f, Thrust));
        const float Radius = NozzleRadiiCm[Nozzle] * FMath::Lerp(.72f, 1.6f, Thrust);
        const float Width = NozzleRadiiCm[Nozzle] * FMath::Lerp(1.1f, 2.8f, Thrust);
        // A hot facing core preserves the visible nozzle from the normal rear
        // chase camera; crossed soft cards retain flame length in side views.
        Cores[Nozzle]->SetRelativeLocation(NozzleLocations[Nozzle]);
        SetCoreRadius(Cores[Nozzle], Radius);
        CoreMIDs[Nozzle]->SetScalarParameterValue(TEXT("Strength"), Strength);
        CoreMIDs[Nozzle]->SetScalarParameterValue(TEXT("Opacity"), FMath::Lerp(.65f, 1.f, Thrust));
        FlameMIDs[Nozzle]->SetScalarParameterValue(TEXT("Strength"), Strength * .7f);
        FlameMIDs[Nozzle]->SetScalarParameterValue(TEXT("Opacity"), FMath::Lerp(.55f, .95f, Thrust));
        for (int32 Card = 0; Card < CardsPerNozzle; ++Card)
        {
            UStaticMeshComponent* Plane = FlameCards[Nozzle * CardsPerNozzle + Card];
            Plane->SetRelativeLocation(NozzleLocations[Nozzle] - NozzleRotations[Nozzle].GetForwardVector() * (Length * .5f));
            Plane->SetRelativeRotation(NozzleRotations[Nozzle] * FRotator(0.f, 0.f, Card * 90.f).Quaternion());
            Plane->SetRelativeScale3D(FVector(Length / 100.f, Width / 100.f, 1.f));
        }
    }
}

void UDSAircraftExhaustComponent::SetExhaustVisible(bool bShow)
{
    if (bShow && bConfigured && !EnsureVisuals()) return;
    bExhaustVisible = bShow && bConfigured && HasReadyMaterials();
    for (UStaticMeshComponent* Plane : FlameCards)
    {
        if (!IsValid(Plane)) continue;
        Plane->SetVisibility(bExhaustVisible);
        Plane->SetHiddenInGame(!bExhaustVisible);
    }
    for (UMaterialBillboardComponent* Core : Cores)
    {
        if (!IsValid(Core)) continue;
        Core->SetVisibility(bExhaustVisible);
        Core->SetHiddenInGame(!bExhaustVisible);
    }
}

TArray<FVector> UDSAircraftExhaustComponent::GetNozzleWorldLocations() const
{
    TArray<FVector> Positions;
    Positions.Reserve(NozzleLocations.Num());
    for (const FVector& Local : NozzleLocations) Positions.Add(GetComponentTransform().TransformPosition(Local));
    return Positions;
}

TArray<FVector> UDSAircraftExhaustComponent::GetNozzleWorldAftDirections() const
{
    TArray<FVector> Directions;
    Directions.Reserve(NozzleRotations.Num());
    for (const FQuat& Rotation : NozzleRotations)
        Directions.Add(GetComponentTransform().TransformVectorNoScale(-Rotation.GetForwardVector()).GetSafeNormal());
    return Directions;
}

float UDSAircraftExhaustComponent::GetFlameLengthCm() const
{
    return (NozzleRadiiCm.IsEmpty() ? 40.f : NozzleRadiiCm[0]) * FMath::Max(6.f, FMath::Lerp(3.5f, 23.f, Thrust));
}

float UDSAircraftExhaustComponent::GetCoreRadiusCm() const
{
    return (NozzleRadiiCm.IsEmpty() ? 40.f : NozzleRadiiCm[0]) * FMath::Lerp(.72f, 1.6f, Thrust);
}

float UDSAircraftExhaustComponent::GetGlowStrength() const
{
    return FMath::Max(36000.f, FMath::Lerp(24000.f, 165000.f, Thrust));
}

int32 UDSAircraftExhaustComponent::GetVisiblePrimitiveCount() const
{
    int32 Count = 0;
    for (const UStaticMeshComponent* Plane : FlameCards)
        if (IsValid(Plane) && Plane->IsVisible() && !Plane->bHiddenInGame) ++Count;
    for (const UMaterialBillboardComponent* Core : Cores)
        if (IsValid(Core) && Core->IsVisible() && !Core->bHiddenInGame) ++Count;
    return Count;
}

bool UDSAircraftExhaustComponent::IsExhaustVisible() const
{
    return bExhaustVisible && !NozzleLocations.IsEmpty() &&
        GetVisiblePrimitiveCount() == NozzleLocations.Num() * (CardsPerNozzle + 1);
}

bool UDSAircraftExhaustComponent::HasReadyMaterials() const
{
    return !NozzleLocations.IsEmpty() && FlameMIDs.Num() == NozzleLocations.Num() && CoreMIDs.Num() == NozzleLocations.Num()
        && !FlameMIDs.ContainsByPredicate([](const UMaterialInstanceDynamic* MID) { return !IsValid(MID); })
        && !CoreMIDs.ContainsByPredicate([](const UMaterialInstanceDynamic* MID) { return !IsValid(MID); });
}

void UDSAircraftExhaustComponent::DestroyVisuals()
{
    for (UStaticMeshComponent* Plane : FlameCards)
        if (IsValid(Plane))
        {
            if (GetOwner()) GetOwner()->RemoveInstanceComponent(Plane);
            Plane->DestroyComponent();
        }
    for (UMaterialBillboardComponent* Core : Cores)
        if (IsValid(Core))
        {
            if (GetOwner()) GetOwner()->RemoveInstanceComponent(Core);
            Core->DestroyComponent();
        }
    FlameCards.Empty();
    Cores.Empty();
    FlameMIDs.Empty();
    CoreMIDs.Empty();
}

void UDSAircraftExhaustComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{
    SetExhaustVisible(false);
    DestroyVisuals();
    Super::OnComponentDestroyed(bDestroyingHierarchy);
}
