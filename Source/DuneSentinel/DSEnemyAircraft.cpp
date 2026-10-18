#include "DSEnemyAircraft.h"
#include "DSGameMode.h"
#include "DSFlightPawn.h"
#include "DSProjectile.h"
#include "DSImpactFX.h"
#include "DSCannonFXComponent.h"
#include "AutoPlay/AutoPlaySubsystem.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/ConstructorHelpers.h"

ADSEnemyAircraft::ADSEnemyAircraft()
{
    PrimaryActorTick.bCanEverTick = true;
    Collision = CreateDefaultSubobject<USphereComponent>(TEXT("TargetCollision"));
    SetRootComponent(Collision);
    Collision->InitSphereRadius(1050.f);
    Collision->SetCollisionProfileName(TEXT("BlockAllDynamic"));
    Collision->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
    Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Airframe"));
    Mesh->SetupAttachment(Collision);
    Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    CannonFX = CreateDefaultSubobject<UDSCannonFXComponent>(TEXT("CannonFX"));
    CannonFX->SetupAttachment(Collision);
}

void ADSEnemyAircraft::Initialize(bool Bomber, const FVector& Destination)
{
    bBomber = Bomber;
    Objective = Destination;
    Health = bBomber ? 240.f : 65.f;
    Speed = bBomber ? 170.f : 235.f;
    const TCHAR* Path = bBomber ? TEXT("/Game/Art/Aircraft/SM_bomber.SM_bomber") : TEXT("/Game/Art/Aircraft/SM_hostile.SM_hostile");
    Mesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, Path));
    GunCooldown = FMath::FRandRange(.3f, 2.f);
    MissileCooldown = FMath::FRandRange(8.f, 16.f);
    TerrainCheckCooldown = 0.f;
    TerrainAltitudeFloor = 0.f;
}

FVector ADSEnemyAircraft::GetFormationSlotOffset(int32 SlotIndex)
{
    if (SlotIndex<=0) return FVector::ZeroVector;
    const int32 Row=(SlotIndex+1)/2;
    return FVector(-16000.f*Row,(SlotIndex%2==1 ? -14000.f : 14000.f)*Row,0.f);
}

void ADSEnemyAircraft::InitializeFormation(int32 GroupId, int32 SlotIndex, int32 Count, const FVector& Origin, const FVector& Heading)
{
    FormationGroup=GroupId;
    FormationSlot=SlotIndex;
    FormationCount=Count;
    FormationOrigin=Origin;
    FormationHeading=Heading.GetSafeNormal2D();
    FormationElapsed=0.f;
    bFormationActive=bBomber && Count>1 && !FormationHeading.IsNearlyZero();
    TerrainCheckCooldown=0.f;
}

void ADSEnemyAircraft::ApplyTerrainClearance(FVector& Aim, const FVector& Position, ADSGameMode* Mode, float DeltaSeconds)
{
    constexpr float ClearanceCm = 30000.f;
    constexpr float NearProbeCm = 50000.f;
    FVector IntendedHeading = (Aim - Position).GetSafeNormal2D();
    FVector ForwardHeading = GetActorForwardVector().GetSafeNormal2D();
    if (ForwardHeading.IsNearlyZero()) ForwardHeading = IntendedHeading;
    if (IntendedHeading.IsNearlyZero()) IntendedHeading = ForwardHeading;

    TerrainCheckCooldown -= FMath::Max(DeltaSeconds, 0.f);
    if (TerrainCheckCooldown <= 0.f)
    {
        // Sample far enough ahead to climb a mountain with the ordinary turn
        // limit. Stop at the intended target: a ridge beyond the runway must
        // not hold an inbound bomber high after it has cleared the near ridge.
        const float LookaheadLimit = FMath::Max(NearProbeCm, float(FVector::Dist2D(Position, Aim)));
        float HighestGround = Mode->GroundHeightAt(Position);
        for (const float Distance : {25000.f, 50000.f, 75000.f, 100000.f, 150000.f, 200000.f, 250000.f, 300000.f})
        {
            const float ProbeDistance = FMath::Min(Distance, LookaheadLimit);
            HighestGround = FMath::Max(HighestGround, Mode->GroundHeightAt(Position + ForwardHeading * ProbeDistance));
            if (FVector::DotProduct(ForwardHeading, IntendedHeading) < .98f)
                HighestGround = FMath::Max(HighestGround, Mode->GroundHeightAt(Position + IntendedHeading * ProbeDistance));
            if (Distance >= LookaheadLimit) break;
        }
        TerrainAltitudeFloor = HighestGround + ClearanceCm;
        TerrainCheckCooldown = .2f;
    }

    if (Aim.Z < TerrainAltitudeFloor || Position.Z < TerrainAltitudeFloor)
    {
        // A distant target can yield too shallow a climb to clear a near ridge.
        // Use a local steering point, preserving the ordinary turn-rate limit
        // and speed-based movement instead of moving the aircraft vertically.
        const float SteeringDistance = FMath::Clamp(FVector::Dist2D(Position, Aim), 10000.f, NearProbeCm);
        const float DesiredAltitude = FMath::Max(Aim.Z, TerrainAltitudeFloor);
        Aim = Position + IntendedHeading * SteeringDistance;
        Aim.Z = DesiredAltitude;
    }
}

void ADSEnemyAircraft::Tick(float Dt)
{
    Super::Tick(Dt);
    ADSGameMode* Mode = GetWorld()->GetAuthGameMode<ADSGameMode>();
    if (!Mode || Mode->Phase != EDSPhase::Flying || bDying) return;
    LifeTime += Dt;
    GunCooldown -= Dt;
    MissileCooldown -= Dt;
    FVector Aim = Objective;
    ADSFlightPawn* Player = Mode->Player;
    const FVector Pos = GetActorLocation();
    const float PlayerDist = IsValid(Player) ? FVector::Dist(Pos, Player->GetActorLocation()) : BIG_NUMBER;
    if (bFormationActive)
    {
        const FVector Anchor=GetFormationAnchor();
        if (FVector::Dist2D(Anchor,Objective)<=FormationBreakDistance || bEgress)
        {
            bFormationActive=false;
            Speed=170.f;
            if (UAutoPlaySubsystem* AP=UAutoPlaySubsystem::Get(this))
                AP->Telemetry(TEXT("bomber_formation_released"),FString::Printf(TEXT("\"group\":%d,\"slot\":%d,\"distance_m\":%.1f"),FormationGroup,FormationSlot,FVector::Dist2D(Pos,Objective)/100.f));
        }
        else
        {
            const FVector Offset=GetFormationSlotOffset(FormationSlot);
            const FVector Right=FVector::CrossProduct(FVector::UpVector,FormationHeading);
            const FVector SlotPosition=Anchor+FormationHeading*Offset.X+Right*Offset.Y;
            const FVector Error=SlotPosition-Pos;
            // Follow a moving reference rather than a live leader; casualties leave honest gaps.
            Aim=SlotPosition+FormationHeading*50000.f;
            Speed=FMath::Clamp(170.f+FVector::DotProduct(Error,FormationHeading)*.008f,140.f,200.f);
            FormationElapsed+=Dt;
        }
    }
    if (!bBomber && IsValid(Player))
    {
        Aim = Player->GetActorLocation() + Player->GetActorForwardVector() * Player->Speed * 100.f * FMath::Clamp(PlayerDist / 60000.f, .1f, 2.f);
        if (PlayerDist < 18000.f) Aim = Pos + GetActorForwardVector() * 160000.f + FVector(0,0,30000.f);
    }
    if (bBomber && !bEgress && !bFormationActive)
    {
        Aim.Z = Objective.Z + 65000.f;
        if (FVector::Dist2D(Pos, Objective) < 210000.f)
        {
            BombCooldown -= Dt;
            // Mountain approaches can arrive higher than the original flight
            // lane. Release only when the actual ballistic arc reaches the
            // airfield, rather than consuming the payload during a high pass.
            const FVector ReleaseVelocity = GetActorForwardVector()*Speed*100.f;
            const float Height = FMath::Max(0.f, float(Pos.Z-400.f-Objective.Z));
            const float FallTime = (ReleaseVelocity.Z+FMath::Sqrt(ReleaseVelocity.Z*ReleaseVelocity.Z+2.f*981.f*Height))/981.f;
            const FVector Impact = Pos+ReleaseVelocity*FallTime;
            const bool bPayloadOnTarget = FVector::Dist2D(Impact,Objective) < 170000.f;
            if (BombCooldown <= 0.f && BombsDropped < 3 && bPayloadOnTarget)
            {
                ADSBomb* Bomb = GetWorld()->SpawnActor<ADSBomb>(Pos - FVector(0,0,400), FRotator(-70, GetActorRotation().Yaw, 0));
                if (Bomb) Bomb->Velocity = GetActorForwardVector() * Speed * 100.f;
                ++BombsDropped;
                BombCooldown = .8f;
                if (BombsDropped == 1) Mode->Radio(TEXT("AIRFIELD: Bombs inbound. Intercept the next formation."));
            }
            if (BombsDropped >= 3)
            {
                bEgress = true;
                Objective = Pos + GetActorForwardVector() * 1800000.f + FVector(0,0,100000.f);
                Aim = Objective;
            }
        }
    }
    // Group ingress has a shared, terrain-cleared altitude. Independent
    // avoidance would distort its triangular slots before the release point.
    if (!bFormationActive) ApplyTerrainClearance(Aim, Pos, Mode, Dt);
    FRotator Desired = (Aim - Pos).Rotation();
    const float YawError = FMath::FindDeltaAngleDegrees(GetActorRotation().Yaw, Desired.Yaw);
    Desired.Roll = FMath::Clamp(YawError * (bBomber ? .5f : 1.3f), -65.f, 65.f);
    FRotator NewRot = FMath::RInterpConstantTo(GetActorRotation(), Desired, Dt, bBomber ? 14.f : 30.f);
    SetActorRotation(NewRot);
    SetActorLocation(Pos + GetActorForwardVector() * Speed * 100.f * Dt);
    if (IsValid(Player))
    {
        const FVector TowardPlayer = (Player->GetActorLocation() - Pos).GetSafeNormal();
        const float Facing = FVector::DotProduct(GetActorForwardVector(), TowardPlayer);
        if (PlayerDist < (bBomber ? 95000.f : 180000.f) && (bBomber || Facing > .94f) && GunCooldown <= 0.f)
        {
            FRotator ShotRot = TowardPlayer.Rotation();
            ShotRot.Pitch += FMath::FRandRange(-1.5f,1.5f);
            ShotRot.Yaw += FMath::FRandRange(-1.5f,1.5f);
            const FVector Muzzle = Pos + TowardPlayer * 1300.f;
            ADSProjectile* Shot = GetWorld()->SpawnActor<ADSProjectile>(Muzzle, ShotRot);
            if (Shot)
            {
                Shot->Initialize(this, Player, false, false, bBomber ? 4.f : 5.f, 1100.f);
                CannonFX->FireShot(Muzzle, ShotRot.Vector(), GetActorForwardVector() * Speed * 100.f, true);
            }
            GunCooldown = bBomber ? .4f : .22f;
        }
        if (!bBomber && PlayerDist < 450000.f && PlayerDist > 70000.f && Facing > .85f && MissileCooldown <= 0.f)
        {
            ADSProjectile* Missile = GetWorld()->SpawnActor<ADSProjectile>(Pos + GetActorForwardVector()*1500.f, GetActorRotation());
            if (Missile) Missile->Initialize(this, Player, false, true, 25.f, 650.f);
            MissileCooldown = 14.f;
            Mode->Radio(TEXT("SENTINEL: Incoming missile. Break and change altitude."), 3.f);
            Mode->PlayCue(TEXT("lock"), .4f);
        }
    }
    if ((bEgress && FVector::Dist2D(GetActorLocation(), Mode->AirportLocation) > Mode->ArenaHalfExtent*1.35f) || LifeTime > 420.f)
    {
        bDying = true;
        Mode->RegisterKill(this, false);
        Destroy();
    }
}

void ADSEnemyAircraft::ApplyCombatDamage(float Damage, bool bPlayerOwned)
{
    if (bDying || Damage <= 0.f) return;
    Health = FMath::Max(0.f, Health-Damage);
    if (Health <= 0.f)
    {
        bDying = true;
        if (ADSGameMode* Mode = GetWorld()->GetAuthGameMode<ADSGameMode>())
        {
            Mode->RegisterKill(this, bPlayerOwned);
            Mode->PlayCue(TEXT("explosion"), .65f);
        }
        SetActorEnableCollision(false);
        GetWorld()->SpawnActor<ADSImpactFX>(GetActorLocation(), FRotator::ZeroRotator);
        Mesh->SetVisibility(false);
        SetLifeSpan(.1f);
    }
}

ADSBomb::ADSBomb()
{
    PrimaryActorTick.bCanEverTick=true;
    Mesh=CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Bomb"));
    SetRootComponent(Mesh);
    Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Model(TEXT("/Game/Art/Effects/SM_missile.SM_missile"));
    if (Model.Succeeded()) Mesh->SetStaticMesh(Model.Object);
}

void ADSBomb::Tick(float Dt)
{
    Super::Tick(Dt);
    ADSGameMode* Mode = GetWorld()->GetAuthGameMode<ADSGameMode>();
    if (!Mode || Mode->Phase != EDSPhase::Flying) return;
    Age += Dt;
    Velocity.Z -= 981.f * Dt;
    SetActorLocation(GetActorLocation()+Velocity*Dt);
    SetActorRotation(Velocity.Rotation());
    if (GetActorLocation().Z <= Mode->GroundHeightAt(GetActorLocation())+100.f)
    {
        if (FVector::Dist2D(GetActorLocation(),Mode->AirportLocation)<180000.f) Mode->DamageAirport(300.f);
        Mode->PlayCue(TEXT("explosion"), .5f);
        GetWorld()->SpawnActor<ADSImpactFX>(GetActorLocation(), FRotator::ZeroRotator);
        Destroy();
    }
    else if (Age > 40.f) Destroy();
}
