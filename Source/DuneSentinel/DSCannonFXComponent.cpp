#include "DSCannonFXComponent.h"

#include "DSGameMode.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/MaterialBillboardComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"

UDSCannonFXComponent::UDSCannonFXComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = false;
    PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

void UDSCannonFXComponent::BeginPlay()
{
    Super::BeginPlay();
    if (GetWorld()->IsGameWorld() && !IsRunningCommandlet())
    {
        EnsureVisuals();
        ResetEffects();
    }
}

UMaterialBillboardComponent* UDSCannonFXComponent::MakeSprite(FName Name, UMaterialInterface* Material, float Radius)
{
    UMaterialBillboardComponent* Sprite = NewObject<UMaterialBillboardComponent>(GetOwner(), Name);
    GetOwner()->AddInstanceComponent(Sprite);
    Sprite->SetupAttachment(this);
    Sprite->SetAbsolute(true, true, true);
    Sprite->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Sprite->SetGenerateOverlapEvents(false);
    Sprite->SetCastShadow(false);
    Sprite->SetHiddenInGame(true);
    Sprite->AddElement(Material, nullptr, false, Radius, Radius, nullptr);
    Sprite->RegisterComponent();
    return Sprite;
}

void UDSCannonFXComponent::EnsureVisuals()
{
    if (Flash || !GetOwner() || !GetWorld()) return;
    // Art is explicitly cooked; lazy loading also lets authoring commandlets create these assets from an empty checkout.
    if (!FlashMaterial) FlashMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Art/Effects/M_CannonFlash.M_CannonFlash"));
    if (!SmokeMaterial) SmokeMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Art/Effects/M_CannonSmoke.M_CannonSmoke"));
    if (!FlashMaterial || !SmokeMaterial) return;
    FlashMID = UMaterialInstanceDynamic::Create(FlashMaterial, this);
    Flash = MakeSprite(TEXT("CannonMuzzleFire"), FlashMID, 170.f);
    Smoke.SetNum(SmokePoolSize);
    for (int32 I = 0; I < SmokePoolSize; ++I)
    {
        UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(SmokeMaterial, this);
        MID->SetVectorParameterValue(TEXT("Color"), FLinearColor(.105f, .12f, .14f));
        MID->SetScalarParameterValue(TEXT("Strength"), 600.f);
        MID->SetScalarParameterValue(TEXT("Opacity"), 0.f);
        SmokeMIDs.Add(MID);
        SmokeSprites.Add(MakeSprite(*FString::Printf(TEXT("CannonSmoke%02d"), I), MID, 100.f));
    }
}

void UDSCannonFXComponent::SetSpriteRadius(UMaterialBillboardComponent* Sprite, float Radius)
{
    if (!Sprite || Sprite->Elements.Num() == 0 || FMath::Abs(Sprite->Elements[0].BaseSizeX - Radius) < 1.f) return;
    Sprite->Elements[0].BaseSizeX = Radius;
    Sprite->Elements[0].BaseSizeY = Radius;
    Sprite->UpdateBounds();
    Sprite->MarkRenderStateDirty();
}

void UDSCannonFXComponent::FireShot(const FVector& WorldMuzzle, const FVector& ShotDirection, const FVector& InheritedVelocityCmPerSec, bool bHostile)
{
    EnsureVisuals();
    if (!Flash) return;
    ++ShotCount;
    LastMuzzle = WorldMuzzle;
    LastDirection = ShotDirection.GetSafeNormal();
    const FTransform OwnerTransform = GetOwner()->GetActorTransform();
    LocalMuzzle = OwnerTransform.InverseTransformPosition(WorldMuzzle);
    LocalDirection = OwnerTransform.InverseTransformVectorNoScale(LastDirection);
    bLastHostile = bHostile;
    FlashRemaining = FlashLifetime;
    FlashStartedFrame = GFrameCounter;
    FlashMID->SetVectorParameterValue(TEXT("Color"), bHostile ? FLinearColor(1.f, .35f, .075f) : FLinearColor(1.f, .62f, .22f));
    FlashMID->SetScalarParameterValue(TEXT("Strength"), bHostile ? 96000.f : 80000.f);
    UpdateFlash();
    if (SmokeCooldown <= 0.f)
    {
        // Separate cosmetic variation from the combat RNG so effect detail cannot change shot spread.
        const float Side = (ShotCount & 1) ? 1.f : -1.f;
        FSmokeParticle& Puff = Smoke[NextSmoke];
        Puff.Position = WorldMuzzle + LastDirection * 60.f;
        Puff.Velocity = InheritedVelocityCmPerSec * .52f - LastDirection * 850.f
            + FVector(0.f, Side * 95.f, 380.f);
        Puff.Age = 0.f;
        Puff.BornFrame = GFrameCounter;
        Puff.Radius = bHostile ? 140.f : 115.f;
        SmokeSprites[NextSmoke]->SetWorldLocation(Puff.Position);
        SetSpriteRadius(SmokeSprites[NextSmoke], Puff.Radius);
        SmokeSprites[NextSmoke]->SetHiddenInGame(false);
        SmokeMIDs[NextSmoke]->SetScalarParameterValue(TEXT("Opacity"), .68f);
        NextSmoke = (NextSmoke + 1) % SmokePoolSize;
        SmokeCooldown = .075f;
        SmokeStartedFrame = GFrameCounter;
    }
    SetComponentTickEnabled(true);
}

void UDSCannonFXComponent::UpdateFlash()
{
    if (!Flash || !GetOwner()) return;
    if (FlashRemaining <= 0.f) { Flash->SetHiddenInGame(true); return; }
    const FTransform OwnerTransform = GetOwner()->GetActorTransform();
    const FVector Position = OwnerTransform.TransformPosition(LocalMuzzle);
    const FVector Direction = OwnerTransform.TransformVectorNoScale(LocalDirection).GetSafeNormal();
    float SizeScale = 1.f;
    if (bLastHostile)
    {
        if (const APlayerController* PC = GetWorld()->GetFirstPlayerController(); PC && PC->PlayerCameraManager)
        {
            const float Distance = FVector::Distance(Position, PC->PlayerCameraManager->GetCameraLocation());
            SizeScale = FMath::Clamp(Distance / 35000.f, 1.f, 3.8f);
        }
    }
    const float Pulse = FMath::Clamp(FlashRemaining / FlashLifetime, 0.f, 1.f);
    Flash->SetWorldLocation(Position + Direction * 110.f);
    SetSpriteRadius(Flash, 170.f * SizeScale * (.75f + .25f * Pulse));
    FlashMID->SetScalarParameterValue(TEXT("Opacity"), .3f + .7f * Pulse);
    Flash->SetHiddenInGame(false);
}

int32 UDSCannonFXComponent::GetActiveSmokeCount() const
{
    int32 Count = 0;
    for (const FSmokeParticle& Puff : Smoke) if (Puff.Age < SmokeLifetime) ++Count;
    return Count;
}

void UDSCannonFXComponent::ResetEffects()
{
    FlashRemaining = 0.f;
    SmokeCooldown = 0.f;
    if (Flash) Flash->SetHiddenInGame(true);
    for (int32 I = 0; I < Smoke.Num(); ++I)
    {
        Smoke[I].Age = SmokeLifetime;
        SmokeSprites[I]->SetHiddenInGame(true);
    }
    SetComponentTickEnabled(false);
}

void UDSCannonFXComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction)
{
    Super::TickComponent(DeltaTime, TickType, TickFunction);
    if (const ADSGameMode* Mode = GetWorld()->GetAuthGameMode<ADSGameMode>(); Mode && Mode->Phase != EDSPhase::Flying)
    {
        ResetEffects();
        return;
    }
    // A shot created earlier in this tick has not yet been rendered. Do not age it
    // by the time that elapsed before it existed, even during a slow frame.
    if (FlashStartedFrame != GFrameCounter) FlashRemaining = FMath::Max(0.f, FlashRemaining - DeltaTime);
    if (SmokeStartedFrame != GFrameCounter) SmokeCooldown -= DeltaTime;
    UpdateFlash();
    for (int32 I = 0; I < Smoke.Num(); ++I)
    {
        FSmokeParticle& Puff = Smoke[I];
        if (Puff.Age >= SmokeLifetime) continue;
        if (Puff.BornFrame == GFrameCounter) continue;
        Puff.Age += DeltaTime;
        if (Puff.Age >= SmokeLifetime) { SmokeSprites[I]->SetHiddenInGame(true); continue; }
        Puff.Position += Puff.Velocity * DeltaTime;
        Puff.Velocity *= FMath::Exp(-3.8f * DeltaTime);
        Puff.Velocity.Z += 500.f * DeltaTime;
        const float T = Puff.Age / SmokeLifetime;
        SmokeSprites[I]->SetWorldLocation(Puff.Position);
        SetSpriteRadius(SmokeSprites[I], Puff.Radius + T * 300.f);
        SmokeMIDs[I]->SetScalarParameterValue(TEXT("Opacity"), .68f * FMath::Pow(1.f - T, 1.3f));
    }
    if (FlashRemaining <= 0.f && GetActiveSmokeCount() == 0) SetComponentTickEnabled(false);
}
