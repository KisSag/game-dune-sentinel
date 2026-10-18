#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DSImpactFX.generated.h"
class UStaticMeshComponent;
class UMaterialInstanceDynamic;
UCLASS()
class DUNESENTINEL_API ADSImpactFX : public AActor
{
    GENERATED_BODY()
public:
    ADSImpactFX();
    virtual void BeginPlay() override;
    virtual void Tick(float Dt) override;
    float Size=1.f;
private:
    UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> Particles;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> Glow;
    TArray<FVector> Directions;
    float Age=0;
};
