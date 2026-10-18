#include "DSProjectile.h"

#include "DSEnemyAircraft.h"
#include "DSFlightPawn.h"
#include "DSGameMode.h"
#include "DSImpactFX.h"
#include "Components/PointLightComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

ADSProjectile::ADSProjectile()
{
    PrimaryActorTick.bCanEverTick = true;
    Collision = CreateDefaultSubobject<USphereComponent>(TEXT("ProjectileCollision"));
    Collision->InitSphereRadius(35.f);
    Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    SetRootComponent(Collision);
    Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ProjectileVisual"));
    Mesh->SetupAttachment(Collision);
    Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Mesh->SetCastShadow(false);
    static ConstructorHelpers::FObjectFinder<UStaticMesh> TracerMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    if (TracerMesh.Succeeded()) Mesh->SetStaticMesh(TracerMesh.Object);
    Mesh->SetRelativeScale3D(FVector(2.4f, .06f, .06f));
    Glow = CreateDefaultSubobject<UPointLightComponent>(TEXT("ProjectileGlow"));
    Glow->SetupAttachment(Collision);
    Glow->SetIntensity(0.f);
    Glow->SetAttenuationRadius(850.f);
    Glow->SetCastShadows(false);
}

void ADSProjectile::Initialize(AActor* OwnerActor, AActor* TargetActor, bool bPlayerOwned, bool bMissile, float Damage, float SpeedMps)
{
    FiredBy = OwnerActor;
    SetOwner(OwnerActor);
    HomingTarget = TargetActor;
    bFriendly = bPlayerOwned;
    bGuided = bMissile;
    HitDamage = FMath::Max(0.f, Damage);
    VelocityMps = FMath::Max(1.f, SpeedMps);
    TravelDirection = GetActorForwardVector();
    LifeSeconds = bGuided ? 10.f : 1.6f;
    if (!bGuided)
        if (UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Art/Effects/M_Tracer.M_Tracer")))
            Mesh->SetMaterial(0, Material);
    if (bGuided)
    {
        if (UStaticMesh* MissileMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Game/Art/Effects/SM_missile.SM_missile")))
        {
            Mesh->SetStaticMesh(MissileMesh);
            Mesh->SetRelativeScale3D(FVector::OneVector);
        }
        else Mesh->SetRelativeScale3D(FVector(4.f, .18f, .18f));
        Glow->SetIntensity(4500.f);
        Glow->SetLightColor(FLinearColor(1.f, .32f, .055f));
        Glow->SetRelativeLocation(FVector(-180.f, 0.f, 0.f));
    }
    SetLifeSpan(LifeSeconds + .1f);
}

void ADSProjectile::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (bSpent) return;
    ADSGameMode* Mode = GetWorld()->GetAuthGameMode<ADSGameMode>();
    if (!Mode || Mode->Phase != EDSPhase::Flying) { Destroy(); return; }
    Age += DeltaSeconds;
    if (Age > LifeSeconds) { Destroy(); return; }
    AActor* TargetActor = HomingTarget.Get();
    if (bGuided && IsValid(TargetActor))
    {
        if (ADSEnemyAircraft* Enemy = Cast<ADSEnemyAircraft>(TargetActor))
            if (Enemy->bDying || Enemy->Health <= 0.f) { HomingTarget.Reset(); TargetActor = nullptr; }
        if (TargetActor)
        {
            const float Distance = FVector::Distance(GetActorLocation(), TargetActor->GetActorLocation());
            const float LeadSeconds = FMath::Clamp(Distance / (VelocityMps * 100.f), 0.f, 1.1f);
            const FVector Aim = TargetActor->GetActorLocation() + TargetActor->GetVelocity() * LeadSeconds * .7f;
            const FVector Desired = (Aim - GetActorLocation()).GetSafeNormal();
            const float Angle = FMath::Acos(FMath::Clamp(FVector::DotProduct(TravelDirection, Desired), -1.f, 1.f));
            const float MaxAngle = FMath::DegreesToRadians(62.f) * FMath::Min(DeltaSeconds, .1f);
            if (Angle > KINDA_SMALL_NUMBER)
            {
                const FQuat Turn = FQuat::FindBetweenNormals(TravelDirection, Desired);
                TravelDirection = FQuat::Slerp(FQuat::Identity, Turn, FMath::Min(1.f, MaxAngle / Angle)).RotateVector(TravelDirection).GetSafeNormal();
            }
            if (!bFriendly)
                if (ADSFlightPawn* Player = Cast<ADSFlightPawn>(TargetActor)) Player->SetMissileThreat(.35f);
        }
    }
    SetActorRotation(TravelDirection.Rotation());
    const FVector Start = GetActorLocation();
    const FVector End = Start + TravelDirection * VelocityMps * 100.f * DeltaSeconds;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(ProjectileFlight), false, this);
    Params.AddIgnoredActor(FiredBy.Get());
    // Friendly fire is disabled; collision still checks all terrain and buildings.
    if (bFriendly)
    {
        if (Mode->Player) Params.AddIgnoredActor(Mode->Player);
    }
    else
    {
        for (ADSEnemyAircraft* Enemy : Mode->Enemies) if (IsValid(Enemy)) Params.AddIgnoredActor(Enemy);
    }
    FHitResult Hit;
    const bool bHit = GetWorld()->SweepSingleByChannel(Hit, Start, End, FQuat::Identity, ECC_Visibility,
        FCollisionShape::MakeSphere(bGuided ? 110.f : 75.f), Params);
    if (bHit)
    {
        SetActorLocation(Hit.Location);
        Impact(Hit.GetActor());
        return;
    }
    // A proximity fuse accounts for targets crossing the swept segment within a frame.
    if (bGuided && TargetActor && FVector::DistSquared(TargetActor->GetActorLocation(), FMath::ClosestPointOnSegment(TargetActor->GetActorLocation(), Start, End)) < FMath::Square(650.f))
    {
        Impact(TargetActor);
        return;
    }
    SetActorLocation(End);
    if (End.Z <= Mode->GroundHeightAt(End) + 20.f) Impact(nullptr);
}

void ADSProjectile::Impact(AActor* HitActor)
{
    if (bSpent) return;
    bSpent = true;
    if (bFriendly)
    {
        if (ADSEnemyAircraft* Enemy = Cast<ADSEnemyAircraft>(HitActor)) Enemy->ApplyCombatDamage(HitDamage, true);
    }
    else
    {
        if (ADSFlightPawn* Player = Cast<ADSFlightPawn>(HitActor)) Player->ApplyCombatDamage(HitDamage);
    }
    if (bGuided)
    {
        const FTransform Transform(GetActorRotation(), GetActorLocation());
        if (ADSImpactFX* Effect = GetWorld()->SpawnActorDeferred<ADSImpactFX>(ADSImpactFX::StaticClass(), Transform))
        {
            Effect->Size = .35f;
            Effect->FinishSpawning(Transform);
        }
        if (ADSGameMode* Mode = GetWorld()->GetAuthGameMode<ADSGameMode>()) Mode->PlayCue(TEXT("explosion"), .35f);
    }
    Destroy();
}
