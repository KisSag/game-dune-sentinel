#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "DSAircraftExhaustComponent.generated.h"

class UMaterialBillboardComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMeshComponent;

/** Persistent engine flames mounted on measured aircraft nozzles. */
UCLASS(ClassGroup=(Effects), meta=(BlueprintSpawnableComponent))
class DUNESENTINEL_API UDSAircraftExhaustComponent : public USceneComponent
{
    GENERATED_BODY()
public:
    UDSAircraftExhaustComponent();
    virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;

    void ConfigureAircraft(const FString& MeshPath, bool bBomber = false);
    void UpdateExhaust(float DeltaSeconds, float Throttle, float Brake);
    void ResetToCruise();
    void SetExhaustVisible(bool bShow);

    int32 GetNozzleCount() const { return NozzleLocations.Num(); }
    TArray<FVector> GetNozzleWorldLocations() const;
    TArray<FVector> GetNozzleWorldAftDirections() const;
    const TArray<float>& GetNozzleRadiiCm() const { return NozzleRadiiCm; }
    float GetFlameLengthCm() const;
    float GetCoreRadiusCm() const;
    float GetGlowStrength() const;
    bool IsExhaustVisible() const;
    bool HasReadyMaterials() const;
    int32 GetVisiblePrimitiveCount() const;
    int32 GetVisualPoolSize() const { return FlameCards.Num() + Cores.Num(); }

private:
    static constexpr int32 MaximumNozzles = 4;
    static constexpr int32 CardsPerNozzle = 2;
    UPROPERTY() TObjectPtr<UMaterialInterface> FlameMaterial;
    UPROPERTY() TObjectPtr<UMaterialInterface> CoreMaterial;
    UPROPERTY(Transient) TArray<TObjectPtr<UStaticMeshComponent>> FlameCards;
    UPROPERTY(Transient) TArray<TObjectPtr<UMaterialBillboardComponent>> Cores;
    UPROPERTY(Transient) TArray<TObjectPtr<UMaterialInstanceDynamic>> FlameMIDs;
    UPROPERTY(Transient) TArray<TObjectPtr<UMaterialInstanceDynamic>> CoreMIDs;

    TArray<FVector> NozzleLocations;
    TArray<FQuat> NozzleRotations;
    TArray<float> NozzleRadiiCm;
    float Thrust = .45f;
    bool bExhaustVisible = false;
    bool bConfigured = false;

    bool EnsureVisuals();
    void DestroyVisuals();
    void ApplyVisuals();
    static void SetCoreRadius(UMaterialBillboardComponent* Core, float Radius);
};
