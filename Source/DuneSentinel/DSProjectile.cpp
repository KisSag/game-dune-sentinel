#include "DSProjectile.h"

#include "DSEnemyAircraft.h"
#include "DSFlightPawn.h"
#include "DSGameMode.h"
#include "DSImpactFX.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/PointLightComponent.h"
#include "Components/MaterialBillboardComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
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
    static ConstructorHelpers::FObjectFinder<UStaticMesh> TrailPlane(TEXT("/Engine/BasicShapes/Plane.Plane"));
    for (int32 Index = 0; Index < 3; ++Index)
    {
        UStaticMeshComponent* Plane = CreateDefaultSubobject<UStaticMeshComponent>(*FString::Printf(TEXT("CannonTrail%d"), Index));
        Plane->SetupAttachment(Collision);
        Plane->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Plane->SetCastShadow(false);
        Plane->SetReceivesDecals(false);
        Plane->SetVisibility(false);
        Plane->SetRelativeRotation(FRotator(0.f, 0.f, Index * 60.f));
        if (TrailPlane.Succeeded()) Plane->SetStaticMesh(TrailPlane.Object);
        CannonTrailPlanes.Add(Plane);
    }
    CannonHead = CreateDefaultSubobject<UMaterialBillboardComponent>(TEXT("CannonHead"));
    CannonHead->SetupAttachment(Collision);
    CannonHead->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    CannonHead->SetGenerateOverlapEvents(false);
    CannonHead->SetCastShadow(false);
    CannonHead->SetReceivesDecals(false);
    CannonHead->SetVisibility(false);
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
    Mesh->SetVisibility(bGuided);
    if (!bGuided)
    {
        if (UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Art/Effects/M_CannonTrail.M_CannonTrail")))
        {
            CannonTrailMaterial = UMaterialInstanceDynamic::Create(Material, this);
            CannonTrailMaterial->SetVectorParameterValue(TEXT("Color"), bFriendly ? FLinearColor(1.f, .62f, .17f) : FLinearColor(1.f, .22f, .045f));
            CannonTrailMaterial->SetScalarParameterValue(TEXT("Opacity"), .9f);
            for (UStaticMeshComponent* Plane : CannonTrailPlanes) Plane->SetMaterial(0, CannonTrailMaterial);
        }
        // A small facing core stays visible when the camera looks directly along the crossed tail planes.
        if (UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Art/Effects/M_CannonHead.M_CannonHead")))
        {
            CannonHeadMaterial = UMaterialInstanceDynamic::Create(Material, this);
            CannonHeadMaterial->SetVectorParameterValue(TEXT("Color"), bFriendly ? FLinearColor(1.f, .62f, .17f) : FLinearColor(1.f, .22f, .045f));
            CannonHeadMaterial->SetScalarParameterValue(TEXT("Opacity"), .9f);
            CannonHeadMaterial->SetScalarParameterValue(TEXT("Strength"), 40000.f);
            CannonHead->AddElement(CannonHeadMaterial, nullptr, false, 7.f, 7.f, nullptr);
            CannonHead->SetVisibility(true);
        }
        UpdateCannonTrail();
    }
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
    if (!bGuided)
    {
        DistanceTravelledCm += FVector::Distance(Start, End);
        UpdateCannonTrail();
    }
    if (End.Z <= Mode->GroundHeightAt(End) + 20.f) Impact(nullptr);
}

void ADSProjectile::UpdateCannonTrail()
{
    if (bGuided) return;
    // The tail ends at the real projectile and cannot extend behind its firing position.
    const float Length = FMath::Min(DistanceTravelledCm, FMath::Clamp(VelocityMps * 100.f * .018f, 1800.f, 4200.f));
    float Width = 14.f;
    if (APlayerController* PC = GetWorld()->GetFirstPlayerController(); PC && PC->PlayerCameraManager)
    {
        int32 ViewWidth = 0, ViewHeight = 0;
        PC->GetViewportSize(ViewWidth, ViewHeight);
        if (ViewWidth > 0)
        {
            const float Distance = FVector::Distance(PC->PlayerCameraManager->GetCameraLocation(), GetActorLocation());
            const float HalfFOV = FMath::DegreesToRadians(PC->PlayerCameraManager->GetFOVAngle() * .5f);
            const float TwoPixelWidth = 4.f * Distance * FMath::Tan(HalfFOV) / ViewWidth;
            Width = FMath::Clamp(TwoPixelWidth, 14.f, 180.f);
        }
    }
    for (UStaticMeshComponent* Plane : CannonTrailPlanes)
    {
        Plane->SetRelativeLocation(FVector(-Length * .5f, 0.f, 0.f));
        Plane->SetRelativeScale3D(FVector(Length / 100.f, Width / 100.f, 1.f));
        Plane->SetVisibility(CannonTrailMaterial && Length > 1.f);
    }
    if (CannonHeadMaterial && CannonHead->Elements.Num() > 0)
    {
        // Billboard geometry uses BaseSize directly; its camera basis cancels component scaling.
        FMaterialSpriteElement& Element = CannonHead->Elements[0];
        const float Radius = Width * .5f;
        if (!FMath::IsNearlyEqual(Element.BaseSizeX, Radius, .25f))
        {
            Element.BaseSizeX = Element.BaseSizeY = Radius;
            CannonHead->UpdateBounds();
            CannonHead->MarkRenderStateDirty();
        }
    }
}

FVector ADSProjectile::GetCannonTrailHeadLocation() const
{
    return CannonTrailPlanes.Num() > 0
        ? CannonTrailPlanes[0]->GetComponentTransform().TransformPosition(FVector(50.f, 0.f, 0.f))
        : GetActorLocation();
}

FVector ADSProjectile::GetCannonTrailTailLocation() const
{
    return CannonTrailPlanes.Num() > 0
        ? CannonTrailPlanes[0]->GetComponentTransform().TransformPosition(FVector(-50.f, 0.f, 0.f))
        : GetActorLocation();
}

float ADSProjectile::GetCannonTrailLengthCm() const
{
    return GetVisibleCannonTrailPlaneCount() > 0 ? FVector::Distance(GetCannonTrailHeadLocation(), GetCannonTrailTailLocation()) : 0.f;
}

int32 ADSProjectile::GetVisibleCannonTrailPlaneCount() const
{
    int32 Count = 0;
    for (const UStaticMeshComponent* Plane : CannonTrailPlanes) if (Plane && Plane->IsVisible()) ++Count;
    return Count;
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
