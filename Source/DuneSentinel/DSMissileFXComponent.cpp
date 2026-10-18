#include "DSMissileFXComponent.h"

#include "Components/MaterialBillboardComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

UDSMissileFXComponent::UDSMissileFXComponent()
{
    // The projectile supplies its completed swept movement, including range and collision clipping.
    PrimaryComponentTick.bCanEverTick = false;
}

UMaterialBillboardComponent* UDSMissileFXComponent::MakeSprite(FName Name, UMaterialInterface* Material, float Radius)
{
    UMaterialBillboardComponent* Sprite = NewObject<UMaterialBillboardComponent>(GetOwner(), Name);
    GetOwner()->AddInstanceComponent(Sprite);
    Sprite->SetupAttachment(this);
    Sprite->SetAbsolute(true, true, true);
    Sprite->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Sprite->SetGenerateOverlapEvents(false);
    Sprite->SetCastShadow(false);
    Sprite->SetReceivesDecals(false);
    Sprite->SetHiddenInGame(true);
    Sprite->AddElement(Material, nullptr, false, Radius, Radius, nullptr);
    Sprite->RegisterComponent();
    return Sprite;
}

bool UDSMissileFXComponent::EnsureVisuals()
{
    if (ExhaustCore) return true;
    if (!GetOwner() || !GetWorld() || IsRunningCommandlet()) return false;

    // These soft materials already belong to the cooked effects directory.
    CoreMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Art/Effects/M_CannonHead.M_CannonHead"));
    FlameMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Art/Effects/M_CannonTrail.M_CannonTrail"));
    SmokeMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Art/Effects/M_CannonSmoke.M_CannonSmoke"));
    UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
    if (!CoreMaterial || !FlameMaterial || !SmokeMaterial || !PlaneMesh) return false;

    ExhaustCoreMID = UMaterialInstanceDynamic::Create(CoreMaterial, this);
    ExhaustFlameMID = UMaterialInstanceDynamic::Create(FlameMaterial, this);
    ExhaustCore = MakeSprite(TEXT("MissileExhaustCore"), ExhaustCoreMID, 120.f);
    for (int32 I = 0; I < 3; ++I)
    {
        UStaticMeshComponent* Plane = NewObject<UStaticMeshComponent>(GetOwner(), *FString::Printf(TEXT("MissileExhaustPlane%d"), I));
        GetOwner()->AddInstanceComponent(Plane);
        Plane->SetupAttachment(this);
        Plane->SetAbsolute(true, true, true);
        Plane->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Plane->SetGenerateOverlapEvents(false);
        Plane->SetCastShadow(false);
        Plane->SetReceivesDecals(false);
        Plane->SetStaticMesh(PlaneMesh);
        Plane->SetMaterial(0, ExhaustFlameMID);
        Plane->SetHiddenInGame(true);
        Plane->RegisterComponent();
        ExhaustPlanes.Add(Plane);
    }

    Contrail.SetNum(ContrailPoolSize);
    for (int32 I = 0; I < ContrailPoolSize; ++I)
    {
        UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(SmokeMaterial, this);
        // A pale, softly shaded cloud reads against both the blue sky and the dark desert.
        MID->SetVectorParameterValue(TEXT("Color"), FLinearColor(.84f, .88f, .92f));
        MID->SetScalarParameterValue(TEXT("Strength"), 1250.f);
        MID->SetScalarParameterValue(TEXT("Opacity"), 0.f);
        ContrailMIDs.Add(MID);
        ContrailSprites.Add(MakeSprite(*FString::Printf(TEXT("MissileContrail%02d"), I), MID, 1800.f));
    }
    return true;
}

void UDSMissileFXComponent::InitializeEffects(bool bHostile)
{
    ResetEffects();
    if (!EnsureVisuals()) return;
    bActive = true;
    SetExhaustVisible(true);
    SetContrailVisible(true);
    const FLinearColor FlameColor = bHostile ? FLinearColor(1.f, .33f, .075f) : FLinearColor(1.f, .55f, .16f);
    ExhaustCoreMID->SetVectorParameterValue(TEXT("Color"), FlameColor);
    ExhaustCoreMID->SetScalarParameterValue(TEXT("Strength"), 95000.f);
    ExhaustCoreMID->SetScalarParameterValue(TEXT("Opacity"), 1.f);
    ExhaustFlameMID->SetVectorParameterValue(TEXT("Color"), FlameColor);
    ExhaustFlameMID->SetScalarParameterValue(TEXT("Strength"), 80000.f);
    ExhaustFlameMID->SetScalarParameterValue(TEXT("Opacity"), .95f);
    UpdateExhaust(GetOwner()->GetActorLocation(), GetOwner()->GetActorForwardVector());
}

void UDSMissileFXComponent::SetSpriteRadius(UMaterialBillboardComponent* Sprite, float Radius)
{
    if (!Sprite || Sprite->Elements.IsEmpty() || FMath::IsNearlyEqual(Sprite->Elements[0].BaseSizeX, Radius, .5f)) return;
    Sprite->Elements[0].BaseSizeX = Sprite->Elements[0].BaseSizeY = Radius;
    Sprite->UpdateBounds();
    Sprite->MarkRenderStateDirty();
}

void UDSMissileFXComponent::UpdateExhaust(const FVector& MissilePosition, const FVector& Direction)
{
    const FVector Forward = Direction.GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);
    const FVector Tail = MissilePosition - Forward * EngineTailOffsetCm;
    ExhaustCore->SetWorldLocation(Tail - Forward * 40.f);
    ExhaustCore->SetHiddenInGame(false);
    // Three crossed tapered cards preserve the elongated flame from side views;
    // the facing hot core covers the otherwise edge-on view along the missile axis.
    const FQuat Facing = Forward.ToOrientationQuat();
    for (int32 I = 0; I < ExhaustPlanes.Num(); ++I)
    {
        UStaticMeshComponent* Plane = ExhaustPlanes[I];
        Plane->SetWorldLocation(Tail - Forward * (ExhaustLengthCm * .5f));
        Plane->SetWorldRotation(Facing * FQuat(FVector::ForwardVector, FMath::DegreesToRadians(I * 60.f)));
        Plane->SetWorldScale3D(FVector(ExhaustLengthCm / 100.f, 2.2f, 1.f));
        Plane->SetHiddenInGame(false);
    }
}

void UDSMissileFXComponent::UpdateContrailVisual(int32 Index)
{
    const FContrailSample& Puff = Contrail[Index];
    if (Puff.Age >= ContrailLifetime)
    {
        ContrailSprites[Index]->SetHiddenInGame(true);
        return;
    }
    const float T = FMath::Clamp(Puff.Age / ContrailLifetime, 0.f, 1.f);
    // Absolute world transforms retain the cloud where the missile passed, including turns.
    ContrailSprites[Index]->SetWorldLocation(Puff.Position);
    SetSpriteRadius(ContrailSprites[Index], FMath::Lerp(1800.f, 3000.f, T));
    ContrailMIDs[Index]->SetScalarParameterValue(TEXT("Opacity"), .88f * FMath::Pow(1.f - T, .75f));
    ContrailSprites[Index]->SetHiddenInGame(false);
}

void UDSMissileFXComponent::AddContrailSample(const FVector& Position, float Age)
{
    ++SampleOrdinal;
    if (Age >= ContrailLifetime) return;
    FContrailSample& Puff = Contrail[NextContrail];
    Puff.Position = Position;
    Puff.Age = FMath::Max(Age, 0.f);
    // Cosmetic variation is deterministic and never consumes the combat random stream.
    ContrailMIDs[NextContrail]->SetScalarParameterValue(TEXT("Seed"), static_cast<float>(SampleOrdinal % 97) * .731f);
    UpdateContrailVisual(NextContrail);
    NextContrail = (NextContrail + 1) % ContrailPoolSize;
}

void UDSMissileFXComponent::UpdateFlight(const FVector& Start, const FVector& End, const FVector& Direction, float DeltaSeconds)
{
    if (!bActive) return;
    const float Dt = FMath::Max(DeltaSeconds, 0.f);
    for (int32 I = 0; I < Contrail.Num(); ++I)
    {
        if (Contrail[I].Age >= ContrailLifetime) continue;
        Contrail[I].Age += Dt;
        UpdateContrailVisual(I);
    }

    const FVector Segment = End - Start;
    const double Length = Segment.Size();
    if (Length >= DistanceToNextSample)
    {
        const int32 Samples = 1 + FMath::FloorToInt((Length - DistanceToNextSample) / ContrailSampleSpacingCm);
        // A stalled frame may span many samples. Only the last pool's worth can
        // survive, keeping both allocation and per-update emission work bounded.
        const int32 First = FMath::Max(0, Samples - ContrailPoolSize);
        SampleOrdinal += First;
        for (int32 I = First; I < Samples; ++I)
        {
            const double AlongSegment = DistanceToNextSample + I * static_cast<double>(ContrailSampleSpacingCm);
            const double Alpha = FMath::Clamp(AlongSegment / Length, 0.0, 1.0);
            AddContrailSample(Start + Segment * Alpha, Dt * static_cast<float>(1.0 - Alpha));
        }
        DistanceToNextSample += Samples * static_cast<double>(ContrailSampleSpacingCm);
    }
    DistanceToNextSample -= Length;
    UpdateExhaust(End, Direction);
}

int32 UDSMissileFXComponent::GetActiveContrailCount() const
{
    int32 Count = 0;
    for (const FContrailSample& Puff : Contrail) if (Puff.Age < ContrailLifetime) ++Count;
    return Count;
}

TArray<FVector> UDSMissileFXComponent::GetContrailWorldLocations() const
{
    TArray<FVector> Locations;
    for (int32 I = 0; I < Contrail.Num(); ++I)
        if (Contrail[I].Age < ContrailLifetime && IsValid(ContrailSprites[I]))
            Locations.Add(ContrailSprites[I]->GetComponentLocation());
    return Locations;
}

TArray<float> UDSMissileFXComponent::GetContrailSampleAges() const
{
    TArray<float> Ages;
    for (int32 I = 0; I < Contrail.Num(); ++I)
        if (Contrail[I].Age < ContrailLifetime && IsValid(ContrailSprites[I]))
            Ages.Add(Contrail[I].Age);
    return Ages;
}

FVector UDSMissileFXComponent::GetExhaustLocation() const
{
    return IsValid(ExhaustCore) ? ExhaustCore->GetComponentLocation() : FVector::ZeroVector;
}

bool UDSMissileFXComponent::IsExhaustVisible() const
{
    return bActive && IsValid(ExhaustCore) && ExhaustCore->IsVisible() && !ExhaustCore->bHiddenInGame
        && GetVisibleExhaustPlaneCount() == 3;
}

bool UDSMissileFXComponent::HasReadyMaterials() const
{
    return IsValid(ExhaustCoreMID) && IsValid(ExhaustFlameMID) && ContrailMIDs.Num() == ContrailPoolSize
        && !ContrailMIDs.ContainsByPredicate([](const UMaterialInstanceDynamic* Material) { return !IsValid(Material); });
}

float UDSMissileFXComponent::GetExhaustCoreStrength() const
{
    return IsValid(ExhaustCoreMID) ? ExhaustCoreMID->K2_GetScalarParameterValue(TEXT("Strength")) : 0.f;
}

int32 UDSMissileFXComponent::GetVisibleExhaustPlaneCount() const
{
    int32 Count = 0;
    for (const UStaticMeshComponent* Plane : ExhaustPlanes)
        if (IsValid(Plane) && Plane->IsVisible() && !Plane->bHiddenInGame) ++Count;
    return Count;
}

float UDSMissileFXComponent::GetExhaustLengthCm() const
{
    if (GetVisibleExhaustPlaneCount() == 0) return 0.f;
    const FTransform& Transform = ExhaustPlanes[0]->GetComponentTransform();
    return FVector::Distance(Transform.TransformPosition(FVector(-50.f, 0.f, 0.f)),
        Transform.TransformPosition(FVector(50.f, 0.f, 0.f)));
}

void UDSMissileFXComponent::ResetEffects()
{
    bActive = false;
    DistanceToNextSample = ContrailSampleSpacingCm;
    NextContrail = 0;
    SampleOrdinal = 0;
    if (IsValid(ExhaustCore)) ExhaustCore->SetHiddenInGame(true);
    for (UStaticMeshComponent* Plane : ExhaustPlanes) if (IsValid(Plane)) Plane->SetHiddenInGame(true);
    for (int32 I = 0; I < Contrail.Num(); ++I)
    {
        Contrail[I].Age = ContrailLifetime;
        if (IsValid(ContrailSprites[I])) ContrailSprites[I]->SetHiddenInGame(true);
    }
}

void UDSMissileFXComponent::SetExhaustVisible(bool bShowEffect)
{
    if (IsValid(ExhaustCore)) ExhaustCore->SetVisibility(bShowEffect && bActive);
    for (UStaticMeshComponent* Plane : ExhaustPlanes)
        if (IsValid(Plane)) Plane->SetVisibility(bShowEffect && bActive);
}

void UDSMissileFXComponent::SetContrailVisible(bool bShowEffect)
{
    // Per-sample hidden state still controls expiry. Restoring visibility cannot
    // revive expired clouds, and newly emitted samples inherit the chosen state.
    for (UMaterialBillboardComponent* Sprite : ContrailSprites)
        if (IsValid(Sprite)) Sprite->SetVisibility(bShowEffect && bActive);
}

void UDSMissileFXComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{
    ResetEffects();
    // The owner normally destroys all its components together; also support
    // removing just this effects component without leaving orphaned world sprites.
    if (IsValid(ExhaustCore)) ExhaustCore->DestroyComponent();
    for (UStaticMeshComponent* Plane : ExhaustPlanes) if (IsValid(Plane)) Plane->DestroyComponent();
    for (UMaterialBillboardComponent* Sprite : ContrailSprites) if (IsValid(Sprite)) Sprite->DestroyComponent();
    ExhaustPlanes.Empty();
    ContrailSprites.Empty();
    ContrailMIDs.Empty();
    Contrail.Empty();
    Super::OnComponentDestroyed(bDestroyingHierarchy);
}
