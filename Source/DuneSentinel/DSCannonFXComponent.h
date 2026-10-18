#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "DSCannonFXComponent.generated.h"

class UMaterialBillboardComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;

/** Reusable muzzle fire and a bounded pool of smoke, driven only by successful cannon shots. */
UCLASS(ClassGroup=(Effects), meta=(BlueprintSpawnableComponent))
class DUNESENTINEL_API UDSCannonFXComponent : public USceneComponent
{
    GENERATED_BODY()
public:
    UDSCannonFXComponent();
    virtual void BeginPlay() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction) override;
    void FireShot(const FVector& WorldMuzzle, const FVector& ShotDirection, const FVector& InheritedVelocityCmPerSec, bool bHostile);
    void ResetEffects();
    uint64 GetShotCount() const { return ShotCount; }
    int32 GetActiveFlashCount() const { return FlashRemaining > 0.f ? 1 : 0; }
    int32 GetActiveSmokeCount() const;
    FVector GetLastMuzzleLocation() const { return LastMuzzle; }
    FVector GetLastShotDirection() const { return LastDirection; }
private:
    static constexpr int32 SmokePoolSize = 16;
    static constexpr float FlashLifetime = .075f;
    static constexpr float SmokeLifetime = .8f;
    struct FSmokeParticle
    {
        FVector Position = FVector::ZeroVector;
        FVector Velocity = FVector::ZeroVector;
        float Age = SmokeLifetime;
        float Radius = 100.f;
        uint64 BornFrame = MAX_uint64;
    };
    UPROPERTY() TObjectPtr<UMaterialInterface> FlashMaterial;
    UPROPERTY() TObjectPtr<UMaterialInterface> SmokeMaterial;
    UPROPERTY(Transient) TObjectPtr<UMaterialBillboardComponent> Flash;
    UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> FlashMID;
    UPROPERTY(Transient) TArray<TObjectPtr<UMaterialBillboardComponent>> SmokeSprites;
    UPROPERTY(Transient) TArray<TObjectPtr<UMaterialInstanceDynamic>> SmokeMIDs;
    TArray<FSmokeParticle> Smoke;
    FVector LastMuzzle = FVector::ZeroVector;
    FVector LastDirection = FVector::ForwardVector;
    FVector LocalMuzzle = FVector::ZeroVector;
    FVector LocalDirection = FVector::ForwardVector;
    float FlashRemaining = 0.f;
    float SmokeCooldown = 0.f;
    uint64 ShotCount = 0;
    uint64 FlashStartedFrame = MAX_uint64;
    uint64 SmokeStartedFrame = MAX_uint64;
    int32 NextSmoke = 0;
    bool bLastHostile = false;
    void EnsureVisuals();
    void UpdateFlash();
    static void SetSpriteRadius(UMaterialBillboardComponent* Sprite, float Radius);
    UMaterialBillboardComponent* MakeSprite(FName Name, UMaterialInterface* Material, float Radius);
};
