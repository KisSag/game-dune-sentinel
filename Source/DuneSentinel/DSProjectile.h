#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DSProjectile.generated.h"

class USphereComponent;
class UStaticMeshComponent;
class UPointLightComponent;

UCLASS()
class DUNESENTINEL_API ADSProjectile : public AActor
{
    GENERATED_BODY()
public:
    ADSProjectile();
    virtual void Tick(float DeltaSeconds) override;
    void Initialize(AActor* OwnerActor, AActor* TargetActor, bool bPlayerOwned, bool bMissile, float Damage, float SpeedMps);
    bool IsPlayerOwned() const { return bFriendly; }
private:
    UPROPERTY() TObjectPtr<USphereComponent> Collision;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> Mesh;
    UPROPERTY() TObjectPtr<UPointLightComponent> Glow;
    UPROPERTY() TWeakObjectPtr<AActor> HomingTarget;
    UPROPERTY() TWeakObjectPtr<AActor> FiredBy;
    FVector TravelDirection = FVector::ForwardVector;
    bool bFriendly = false;
    bool bGuided = false;
    float HitDamage = 20.f;
    float VelocityMps = 800.f;
    float Age = 0.f;
    float LifeSeconds = 4.f;
    bool bSpent = false;
    void Impact(AActor* HitActor);
};
