#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "DSMissileFXComponent.generated.h"

class UMaterialBillboardComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMeshComponent;

/** Missile exhaust follows the engine; contrail samples remain on the actual flight path. */
UCLASS(ClassGroup=(Effects), meta=(BlueprintSpawnableComponent))
class DUNESENTINEL_API UDSMissileFXComponent : public USceneComponent
{
    GENERATED_BODY()
public:
    UDSMissileFXComponent();
    virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;

    void InitializeEffects(bool bHostile);
    void UpdateFlight(const FVector& Start, const FVector& End, const FVector& Direction, float DeltaSeconds);
    void ResetEffects();
    void SetExhaustVisible(bool bVisible);
    void SetContrailVisible(bool bVisible);

    int32 GetActiveContrailCount() const;
    static constexpr int32 GetContrailPoolSize() { return ContrailPoolSize; }
    static constexpr float GetContrailSampleSpacingCm() { return ContrailSampleSpacingCm; }
    static constexpr float GetContrailLifetimeSeconds() { return ContrailLifetime; }
    TArray<FVector> GetContrailWorldLocations() const;
    TArray<float> GetContrailSampleAges() const;
    FVector GetExhaustLocation() const;
    bool IsExhaustVisible() const;
    bool HasReadyMaterials() const;
    float GetExhaustCoreStrength() const;
    int32 GetVisibleExhaustPlaneCount() const;
    float GetExhaustLengthCm() const;

private:
    static constexpr int32 ContrailPoolSize = 64;
    static constexpr float ContrailSampleSpacingCm = 2500.f;
    static constexpr float ContrailLifetime = 1.8f;
    static constexpr float EngineTailOffsetCm = 180.f;
    static constexpr float ExhaustLengthCm = 1400.f;

    struct FContrailSample
    {
        FVector Position = FVector::ZeroVector;
        float Age = ContrailLifetime;
    };

    UPROPERTY() TObjectPtr<UMaterialInterface> CoreMaterial;
    UPROPERTY() TObjectPtr<UMaterialInterface> FlameMaterial;
    UPROPERTY() TObjectPtr<UMaterialInterface> SmokeMaterial;
    UPROPERTY(Transient) TObjectPtr<UMaterialBillboardComponent> ExhaustCore;
    UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> ExhaustCoreMID;
    UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> ExhaustFlameMID;
    UPROPERTY(Transient) TArray<TObjectPtr<UStaticMeshComponent>> ExhaustPlanes;
    UPROPERTY(Transient) TArray<TObjectPtr<UMaterialBillboardComponent>> ContrailSprites;
    UPROPERTY(Transient) TArray<TObjectPtr<UMaterialInstanceDynamic>> ContrailMIDs;

    TArray<FContrailSample> Contrail;
    double DistanceToNextSample = ContrailSampleSpacingCm;
    uint64 SampleOrdinal = 0;
    int32 NextContrail = 0;
    bool bActive = false;

    bool EnsureVisuals();
    void UpdateExhaust(const FVector& MissilePosition, const FVector& Direction);
    void UpdateContrailVisual(int32 Index);
    void AddContrailSample(const FVector& Position, float Age);
    UMaterialBillboardComponent* MakeSprite(FName Name, UMaterialInterface* Material, float Radius);
    static void SetSpriteRadius(UMaterialBillboardComponent* Sprite, float Radius);
};
