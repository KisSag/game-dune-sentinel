#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DSEnemyAircraft.generated.h"

class UStaticMeshComponent;
class USphereComponent;
class UDSCannonFXComponent;
UCLASS()
class DUNESENTINEL_API ADSEnemyAircraft : public AActor
{
    GENERATED_BODY()
public:
    ADSEnemyAircraft();
    virtual void Tick(float DeltaSeconds) override;
    virtual FVector GetVelocity() const override { return bDying ? FVector::ZeroVector : GetActorForwardVector()*Speed*100.f; }
    void Initialize(bool Bomber, const FVector& Destination);
    void ApplyCombatDamage(float Damage, bool bPlayerOwned);
    UPROPERTY(BlueprintReadOnly) bool bBomber = false;
    UPROPERTY(BlueprintReadOnly) bool bDying = false;
    UPROPERTY(BlueprintReadOnly) float Health = 65.f;
    UPROPERTY(BlueprintReadOnly) float Speed = 220.f;
    UPROPERTY(VisibleAnywhere) TObjectPtr<USphereComponent> Collision;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Mesh;
    bool bEgress = false;
private:
    UPROPERTY(VisibleAnywhere) TObjectPtr<UDSCannonFXComponent> CannonFX;
    FVector Objective;
    float GunCooldown = 1.f;
    float MissileCooldown = 5.f;
    float BombCooldown = 0.f;
    float LifeTime = 0.f;
    int32 BombsDropped = 0;
};

UCLASS()
class DUNESENTINEL_API ADSBomb : public AActor
{
    GENERATED_BODY()
public:
    ADSBomb();
    virtual void Tick(float DeltaSeconds) override;
    FVector Velocity = FVector::ZeroVector;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Mesh;
private:
    float Age = 0.f;
};
