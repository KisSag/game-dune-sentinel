#include "DSFlightPawn.h"

#include "DSGameMode.h"
#include "DSEnemyAircraft.h"
#include "DSPlayerController.h"
#include "DSProjectile.h"
#include "DSCannonFXComponent.h"
#include "AutoPlay/AutoPlaySubsystem.h"
#include "Camera/CameraComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/AudioComponent.h"
#include "Engine/LocalPlayer.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"

ADSFlightPawn::ADSFlightPawn()
{
    PrimaryActorTick.bCanEverTick = true;
    Collision = CreateDefaultSubobject<USphereComponent>(TEXT("FlightCollision"));
    Collision->InitSphereRadius(430.f);
    Collision->SetCollisionProfileName(TEXT("Pawn"));
    Collision->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
    Collision->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
    SetRootComponent(Collision);
    AircraftMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Aircraft"));
    AircraftMesh->SetupAttachment(Collision);
    AircraftMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    CannonFX = CreateDefaultSubobject<UDSCannonFXComponent>(TEXT("CannonFX"));
    CannonFX->SetupAttachment(Collision);
    LandingGear = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("LandingGear"));
    LandingGear->SetupAttachment(AircraftMesh);
    LandingGear->SetRelativeScale3D(FVector(.8f));
    LandingGear->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    CameraArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("ChaseArm"));
    CameraArm->SetupAttachment(Collision);
    CameraArm->TargetArmLength = 5800.f;
    CameraArm->SocketOffset = FVector(0.f, 0.f, 850.f);
    CameraArm->bDoCollisionTest = false;
    CameraArm->SetUsingAbsoluteRotation(true);
    CameraArm->bUsePawnControlRotation = false;
    Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("FlightCamera"));
    Camera->SetupAttachment(CameraArm, USpringArmComponent::SocketName);
    Camera->FieldOfView = 78.f;
    EngineAudio = CreateDefaultSubobject<UAudioComponent>(TEXT("EngineLoop"));
    EngineAudio->SetupAttachment(Collision);
    EngineAudio->bAutoActivate = false;
    EngineAudio->bAllowSpatialization = false;
    EngineAudio->SetVolumeMultiplier(.09f);
    WindAudio = CreateDefaultSubobject<UAudioComponent>(TEXT("WindLoop"));
    WindAudio->SetupAttachment(Collision);
    WindAudio->bAutoActivate = false;
    WindAudio->bAllowSpatialization = false;
    WindAudio->SetVolumeMultiplier(0.f);
    bUseControllerRotationPitch = false;
    bUseControllerRotationYaw = false;
    bUseControllerRotationRoll = false;
}

void ADSFlightPawn::BeginPlay()
{
    Super::BeginPlay();
    MakeInputActions();
    ConfigureAircraft(Spec);
    CameraOrientation = GetActorQuat();
    if (!bFlying) Park();
    if (USoundBase* Sound = LoadObject<USoundBase>(nullptr, TEXT("/Game/Audio/S_engine.S_engine")))
    {
        EngineAudio->SetSound(Sound);
        EngineAudio->SetPitchMultiplier(EnginePitch);
        EngineAudio->Play();
    }
    if (USoundBase* Sound = LoadObject<USoundBase>(nullptr, TEXT("/Game/Audio/S_wind.S_wind")))
    {
        WindAudio->SetSound(Sound);
    }
}

void ADSFlightPawn::MakeInputActions()
{
    if (FlightMapping) return;
    FlightMapping = NewObject<UInputMappingContext>(this, TEXT("FlightKeyboardMouse"));
    auto Action = [this](const TCHAR* Name, EInputActionValueType Type)
    {
        UInputAction* Result = NewObject<UInputAction>(this, FName(*(FString(TEXT("IA_")) + Name)));
        Result->ValueType = Type;
        return Result;
    };
    PitchAction = Action(TEXT("FlightPitch"), EInputActionValueType::Axis1D);
    RollAction = Action(TEXT("FlightRoll"), EInputActionValueType::Axis1D);
    YawAction = Action(TEXT("FlightYaw"), EInputActionValueType::Axis1D);
    YawLeftAction = Action(TEXT("FlightYawLeft"), EInputActionValueType::Axis1D);
    YawRightAction = Action(TEXT("FlightYawRight"), EInputActionValueType::Axis1D);
    ThrottleAction = Action(TEXT("FlightThrottle"), EInputActionValueType::Axis1D);
    BrakeAction = Action(TEXT("FlightBrake"), EInputActionValueType::Axis1D);
    GunAction = Action(TEXT("FlightGun"), EInputActionValueType::Boolean);
    MissileAction = Action(TEXT("FlightMissile"), EInputActionValueType::Boolean);
    TargetAction = Action(TEXT("FlightNextTarget"), EInputActionValueType::Boolean);
    MousePitchAction = Action(TEXT("FlightMousePitch"), EInputActionValueType::Axis1D);
    MouseRollAction = Action(TEXT("FlightMouseRoll"), EInputActionValueType::Axis1D);
    LookAction = Action(TEXT("FlightLookTarget"), EInputActionValueType::Boolean);
    FreeLookAction = Action(TEXT("FlightFreeLook"), EInputActionValueType::Boolean);
    CameraAction = Action(TEXT("FlightCamera"), EInputActionValueType::Boolean);
    auto Map = [this](UInputAction* Input, FKey Key, bool bNegative = false)
    {
        FEnhancedActionKeyMapping& Mapping = FlightMapping->MapKey(Input, Key);
        if (bNegative) Mapping.Modifiers.Add(NewObject<UInputModifierNegate>(FlightMapping));
    };
    Map(PitchAction, EKeys::Down); Map(PitchAction, EKeys::Up, true);
    Map(RollAction, EKeys::D); Map(RollAction, EKeys::A, true);
    Map(YawRightAction, EKeys::E); Map(YawLeftAction, EKeys::Q);
    Map(ThrottleAction, EKeys::W); Map(BrakeAction, EKeys::S);
    Map(GunAction, EKeys::LeftMouseButton); Map(GunAction, EKeys::LeftControl);
    Map(MissileAction, EKeys::RightMouseButton); Map(MissileAction, EKeys::SpaceBar);
    Map(TargetAction, EKeys::Tab); Map(LookAction, EKeys::F);
    Map(FreeLookAction, EKeys::LeftAlt); Map(CameraAction, EKeys::V);
    Map(MousePitchAction, EKeys::MouseY); Map(MouseRollAction, EKeys::MouseX);
}

void ADSFlightPawn::InstallInputContext()
{
    if (bContextInstalled) return;
    MakeInputActions();
    if (APlayerController* PC = Cast<APlayerController>(GetController()))
    {
        if (ULocalPlayer* LocalPlayer = PC->GetLocalPlayer())
        {
            if (UEnhancedInputLocalPlayerSubsystem* Input = LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
            {
                Input->AddMappingContext(FlightMapping, 0);
                bContextInstalled = true;
            }
        }
    }
}

void ADSFlightPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
    Super::SetupPlayerInputComponent(PlayerInputComponent);
    MakeInputActions();
    UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent);
    if (!Input) return;
    auto BindContinuous = [this, Input](UInputAction* Action, auto Function)
    {
        Input->BindAction(Action, ETriggerEvent::Triggered, this, Function);
        Input->BindAction(Action, ETriggerEvent::Completed, this, Function);
        Input->BindAction(Action, ETriggerEvent::Canceled, this, Function);
    };
    BindContinuous(PitchAction, &ADSFlightPawn::OnPitch);
    BindContinuous(RollAction, &ADSFlightPawn::OnRoll);
    BindContinuous(YawAction, &ADSFlightPawn::OnYaw);
    BindContinuous(YawLeftAction, &ADSFlightPawn::OnYawLeft);
    BindContinuous(YawRightAction, &ADSFlightPawn::OnYawRight);
    BindContinuous(ThrottleAction, &ADSFlightPawn::OnThrottle);
    BindContinuous(BrakeAction, &ADSFlightPawn::OnBrake);
    BindContinuous(GunAction, &ADSFlightPawn::OnGun);
    BindContinuous(MousePitchAction, &ADSFlightPawn::OnMousePitch);
    BindContinuous(MouseRollAction, &ADSFlightPawn::OnMouseRoll);
    BindContinuous(LookAction, &ADSFlightPawn::OnLook);
    BindContinuous(FreeLookAction, &ADSFlightPawn::OnFreeLook);
    BindContinuous(TargetAction, &ADSFlightPawn::OnTargetHeld);
    Input->BindAction(MissileAction, ETriggerEvent::Started, this, &ADSFlightPawn::OnMissile);
    Input->BindAction(TargetAction, ETriggerEvent::Started, this, &ADSFlightPawn::OnTarget);
    Input->BindAction(CameraAction, ETriggerEvent::Started, this, &ADSFlightPawn::OnCamera);
    InstallInputContext();
}

void ADSFlightPawn::EndPlay(const EEndPlayReason::Type Reason)
{
    EngineAudio->Stop();
    WindAudio->Stop();
    if (APlayerController* PC = Cast<APlayerController>(GetController()))
    {
        if (ULocalPlayer* LocalPlayer = PC->GetLocalPlayer())
            if (UEnhancedInputLocalPlayerSubsystem* Input = LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
                Input->RemoveMappingContext(FlightMapping);
    }
    Super::EndPlay(Reason);
}

void ADSFlightPawn::OnPitch(const FInputActionValue& Value) { PitchInput = Value.Get<float>(); }
void ADSFlightPawn::OnRoll(const FInputActionValue& Value) { RollInput = Value.Get<float>(); }
void ADSFlightPawn::OnYaw(const FInputActionValue& Value) { YawInput = Value.Get<float>(); }
void ADSFlightPawn::OnThrottle(const FInputActionValue& Value) { ThrottleInput = Value.Get<float>(); }
void ADSFlightPawn::OnBrake(const FInputActionValue& Value) { BrakeInput = Value.Get<float>(); }
void ADSFlightPawn::OnGun(const FInputActionValue& Value) { bGunHeld = Value.Get<bool>(); }
void ADSFlightPawn::OnMissile(const FInputActionValue& Value) { if (Value.Get<bool>()) FireMissile(); }
void ADSFlightPawn::OnTarget(const FInputActionValue& Value) { if (bFlying && Value.Get<bool>()) SelectNextTarget(); }
void ADSFlightPawn::OnTargetHeld(const FInputActionValue& Value) { bTargetHeld = Value.Get<bool>(); if (!bTargetHeld) TargetHoldTime = 0.f; }
void ADSFlightPawn::OnYawLeft(const FInputActionValue& Value) { YawLeftInput = Value.Get<float>(); }
void ADSFlightPawn::OnYawRight(const FInputActionValue& Value) { YawRightInput = Value.Get<float>(); }
void ADSFlightPawn::OnMousePitch(const FInputActionValue& Value) { MousePitchInput = Value.Get<float>(); }
void ADSFlightPawn::OnMouseRoll(const FInputActionValue& Value) { MouseRollInput = Value.Get<float>(); }
void ADSFlightPawn::OnLook(const FInputActionValue& Value) { bLookingAtTarget = Value.Get<bool>(); }
void ADSFlightPawn::OnFreeLook(const FInputActionValue& Value) { bFreeLooking = Value.Get<bool>(); }
void ADSFlightPawn::OnCamera(const FInputActionValue& Value)
{
    if (bFlying && Value.Get<bool>()) bCockpitView = !bCockpitView;
}

void ADSFlightPawn::ResetControls()
{
    PitchInput = RollInput = YawInput = ThrottleInput = BrakeInput = 0.f;
    YawLeftInput = YawRightInput = TargetHoldTime = 0.f;
    MousePitchInput = MouseRollInput = 0.f;
    bGunHeld = bLookingAtTarget = bFreeLooking = bTargetHeld = false;
    AngularRates = FVector::ZeroVector;
    FreeLookRotation = FRotator::ZeroRotator;
}

void ADSFlightPawn::ConfigureAircraft(const FDSAircraftSpec& Aircraft)
{
    Spec = Aircraft;
    Health = FMath::Max(1.f, Spec.MaxHealth);
    CannonAmmo = FMath::Max(0, Spec.Cannon);
    MissileAmmo = FMath::Max(0, Spec.Missiles);
    if (UStaticMesh* Model = LoadObject<UStaticMesh>(nullptr, *Spec.MeshPath))
        AircraftMesh->SetStaticMesh(Model);
    if (UStaticMesh* Gear = LoadObject<UStaticMesh>(nullptr, TEXT("/Game/Art/Aircraft/SM_landinggear.SM_landinggear")))
        LandingGear->SetStaticMesh(Gear);
    Target.Reset();
    LockProgress = 0.f;
}

void ADSFlightPawn::Park()
{
    CannonFX->ResetEffects();
    bFlying = false;
    Speed = 0.f;
    FlightVelocity = FVector::ZeroVector;
    Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    LandingGear->SetVisibility(true);
    bCockpitView = bHighG = bStalling = bMissileWarning = false;
    GunCooldown = MissileCooldown = GunSoundCooldown = 0.f;
    Target.Reset();
    LockProgress = 0.f;
    ResetControls();
}

void ADSFlightPawn::BeginFlight()
{
    CannonFX->ResetEffects();
    ResetControls();
    bFlying = true;
    Speed = Spec.CruiseSpeed;
    bCockpitView = false;
    BoundaryTime = 0.f;
    MissileThreatUntil = 0.f;
    Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    LandingGear->SetVisibility(false);
    CameraOrientation = GetActorQuat();
    InstallInputContext();
    SelectNextTarget();
}

FVector ADSFlightPawn::GetVelocity() const { return FlightVelocity; }

void ADSFlightPawn::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    InstallInputContext();
    if (const ADSPlayerController* PC = Cast<ADSPlayerController>(GetController()))
    {
        MouseSensitivity = FMath::Clamp(PC->MouseSensitivity, .1f, 4.f);
        bInvertMouseY = PC->bInvertMouse;
    }
    GunCooldown = FMath::Max(0.f, GunCooldown - DeltaSeconds);
    MissileCooldown = FMath::Max(0.f, MissileCooldown - DeltaSeconds);
    GunSoundCooldown = FMath::Max(0.f, GunSoundCooldown - DeltaSeconds);
    bMissileWarning = bFlying && GetWorld()->GetTimeSeconds() < MissileThreatUntil;
    const ADSGameMode* Mode = GetWorld()->GetAuthGameMode<ADSGameMode>();
    const bool bAudibleFlight = bFlying && Health > 0.f && Mode && Mode->Phase == EDSPhase::Flying;
    const float SpeedFraction = FMath::Clamp(Speed / FMath::Max(Spec.MaxSpeed, 1.f), 0.f, 1.f);
    EngineVolume = FMath::FInterpTo(EngineVolume, bAudibleFlight ? .13f + SpeedFraction * .08f : .065f, DeltaSeconds, 2.f);
    WindVolume = FMath::FInterpTo(WindVolume, bAudibleFlight ? .025f + SpeedFraction * .09f : 0.f, DeltaSeconds, 2.f);
    EnginePitch = FMath::FInterpTo(EnginePitch, bAudibleFlight ? .85f + SpeedFraction * .35f + ThrottleInput * .06f : .72f, DeltaSeconds, 1.5f);
    EngineAudio->SetVolumeMultiplier(Health > 0.f ? EngineVolume : 0.f);
    EngineAudio->SetPitchMultiplier(EnginePitch);
    WindAudio->SetVolumeMultiplier(WindVolume);
    if (bAudibleFlight && !WindAudio->IsPlaying()) WindAudio->Play();
    else if (!bAudibleFlight && WindVolume < .001f && WindAudio->IsPlaying()) WindAudio->Stop();
    if (bFlying && Health > 0.f && Mode && Mode->Phase == EDSPhase::Flying)
    {
        if (bTargetHeld) TargetHoldTime += DeltaSeconds;
        UpdateFlight(DeltaSeconds);
        UpdateTarget(DeltaSeconds);
        if (bGunHeld) FireGun();
    }
    UpdateCamera(DeltaSeconds);
}

void ADSFlightPawn::UpdateFlight(float DeltaSeconds)
{
    const float Dt = DeltaSeconds;
    // Mouse axes report per-frame deltas; normalize to a 60 Hz reference before
    // feeding angular rates so physical mouse motion is independent of FPS.
    const float MouseScale = MouseSensitivity * .12f / FMath::Max(Dt * 60.f, .001f);
    const float MousePitch = bFreeLooking ? 0.f : MousePitchInput * MouseScale * (bInvertMouseY ? -1.f : 1.f);
    const float MouseRoll = bFreeLooking ? 0.f : MouseRollInput * MouseScale;
    const float Pitch = FMath::Clamp(PitchInput + MousePitch, -1.f, 1.f);
    const float Roll = FMath::Clamp(RollInput + MouseRoll, -1.f, 1.f);
    const float Yaw = FMath::Clamp(YawInput + YawRightInput - YawLeftInput, -1.f, 1.f);
    bHighG = ThrottleInput > .5f && BrakeInput > .5f && Pitch > .1f && Speed > 90.f;
    const float DesiredSpeed = ThrottleInput > .1f ? Spec.MaxSpeed : (BrakeInput > .1f ? 65.f : Spec.CruiseSpeed);
    float Acceleration = FMath::Clamp((DesiredSpeed - Speed) * .35f, -65.f, 55.f);
    if (bHighG) Acceleration = -75.f;
    Acceleration -= GetActorForwardVector().Z * 12.f;
    Speed = FMath::Clamp(Speed + Acceleration * Dt, 45.f, Spec.MaxSpeed);
    bStalling = Speed < 95.f;
    const float Authority = FMath::GetMappedRangeValueClamped(FVector2D(45.f, 140.f), FVector2D(.22f, 1.f), Speed);
    const float HighGFactor = bHighG ? 1.8f : 1.f;
    const FVector DesiredRates(Roll * Spec.RollRate * Authority, Pitch * Spec.PitchRate * Authority * HighGFactor, Yaw * 18.f * Authority);
    AngularRates = FMath::VInterpTo(AngularRates, DesiredRates, Dt, 6.f);
    const FQuat LocalRotation = FQuat(FVector::UpVector, FMath::DegreesToRadians(AngularRates.Z * Dt))
        * FQuat(FVector::RightVector, FMath::DegreesToRadians(-AngularRates.Y * Dt))
        * FQuat(FVector::ForwardVector, FMath::DegreesToRadians(-AngularRates.X * Dt));
    SetActorRotation((GetActorQuat() * LocalRotation).GetNormalized());
    if (YawLeftInput > .5f && YawRightInput > .5f)
    {
        const FQuat Level = FRotator(0.f, GetActorForwardVector().Rotation().Yaw, 0.f).Quaternion();
        SetActorRotation(FQuat::Slerp(GetActorQuat(), Level, 1.f - FMath::Exp(-2.f * Dt)).GetNormalized());
    }
    FlightVelocity = GetActorForwardVector() * Speed * 100.f;
    if (bStalling)
    {
        FlightVelocity.Z -= (95.f - Speed) * 150.f;
        const FVector NoseDown = (GetActorForwardVector() + FVector(0.f, 0.f, -.25f * Dt)).GetSafeNormal();
        const FQuat DownCorrection = FQuat::FindBetweenNormals(GetActorForwardVector(), NoseDown);
        SetActorRotation((DownCorrection * GetActorQuat()).GetNormalized());
    }
    FHitResult Hit;
    AddActorWorldOffset(FlightVelocity * Dt, true, &Hit);
    ADSGameMode* Mode = GetWorld()->GetAuthGameMode<ADSGameMode>();
    if (!Mode) return;
    if (Hit.bBlockingHit || GetActorLocation().Z < Mode->GroundHeightAt(GetActorLocation()) + 230.f)
    {
        ApplyCombatDamage(Health + 1.f);
        return;
    }
    const FVector Position = GetActorLocation();
    const bool bOutside = FMath::Abs(Position.X) > Mode->ArenaHalfExtent || FMath::Abs(Position.Y) > Mode->ArenaHalfExtent || Position.Z > 600000.f;
    if (bOutside)
    {
        if (BoundaryTime == 0.f) Mode->Radio(TEXT("BOUNDARY WARNING / RETURN TO THE OPERATION ZONE"), 8.f);
        BoundaryTime += Dt;
        if (BoundaryTime > 15.f) Mode->EndSortie(TEXT("LEFT THE OPERATION ZONE"));
    }
    else BoundaryTime = 0.f;
}

void ADSFlightPawn::SelectNextTarget()
{
    ADSGameMode* Mode = GetWorld()->GetAuthGameMode<ADSGameMode>();
    if (!Mode) return;
    TArray<ADSEnemyAircraft*> Candidates;
    for (ADSEnemyAircraft* Enemy : Mode->Enemies)
        if (IsValid(Enemy) && !Enemy->bDying && Enemy->Health > 0.f) Candidates.Add(Enemy);
    Candidates.Sort([this](const ADSEnemyAircraft& A, const ADSEnemyAircraft& B)
    {
        return FVector::DistSquared(A.GetActorLocation(), GetActorLocation()) < FVector::DistSquared(B.GetActorLocation(), GetActorLocation());
    });
    const int32 Current = Candidates.IndexOfByKey(Target.Get());
    Target = Candidates.IsEmpty() ? nullptr : Candidates[(Current + 1) % Candidates.Num()];
    LockProgress = 0.f;
    bLockSoundPlayed = false;
}

void ADSFlightPawn::UpdateTarget(float DeltaSeconds)
{
    if (!Target.IsValid() || Target->bDying || Target->Health <= 0.f) SelectNextTarget();
    if (!Target.IsValid()) { LockProgress = 0.f; return; }
    const FVector ToTarget = Target->GetActorLocation() - GetActorLocation();
    const float Distance = ToTarget.Size();
    const float Alignment = FVector::DotProduct(GetActorForwardVector(), ToTarget.GetSafeNormal());
    bool bCanLock = Distance > 1500.f && Distance < 600000.f && Alignment > FMath::Cos(FMath::DegreesToRadians(16.f));
    if (bCanLock)
    {
        FCollisionQueryParams Params(SCENE_QUERY_STAT(MissileLock), false, this);
        Params.AddIgnoredActor(Target.Get());
        FHitResult Hit;
        bCanLock = !GetWorld()->LineTraceSingleByObjectType(Hit, GetActorLocation(), Target->GetActorLocation(), FCollisionObjectQueryParams(ECC_WorldStatic), Params);
    }
    LockProgress = bCanLock ? FMath::Min(1.f, LockProgress + DeltaSeconds * 1.25f) : 0.f;
    if (LockProgress >= 1.f && !bLockSoundPlayed)
    {
        if (ADSGameMode* Mode = GetWorld()->GetAuthGameMode<ADSGameMode>()) Mode->PlayCue(TEXT("lock"), .65f);
        bLockSoundPlayed = true;
    }
    if (LockProgress < .5f) bLockSoundPlayed = false;
}

void ADSFlightPawn::FireGun()
{
    if (!bFlying || CannonAmmo <= 0 || GunCooldown > 0.f) return;
    ADSGameMode* Mode = GetWorld()->GetAuthGameMode<ADSGameMode>();
    if (!Mode || Mode->Phase != EDSPhase::Flying) return;
    const FVector Muzzle = GetActorLocation() + GetActorForwardVector() * 1100.f
        + GetActorRightVector() * 160.f + GetActorUpVector() * 80.f;
    FActorSpawnParameters Params;
    Params.Owner = this;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    ADSProjectile* Shot = GetWorld()->SpawnActor<ADSProjectile>(Muzzle, GetActorRotation(), Params);
    if (!Shot) return;
    Shot->Initialize(this, nullptr, true, false, 8.f, 1450.f + Speed);
    CannonFX->FireShot(Muzzle, GetActorForwardVector(), GetVelocity(), false);
    --CannonAmmo;
    if (UAutoPlaySubsystem* AP = UAutoPlaySubsystem::Get(this); AP && AP->IsRunning())
        AP->Telemetry(TEXT("weapon_fired"), FString::Printf(TEXT("\"weapon\":\"cannon\",\"ammo\":%d"), CannonAmmo));
    GunCooldown = .055f;
    if (GunSoundCooldown <= 0.f)
    {
        Mode->PlayCue(TEXT("gun"), .38f);
        GunSoundCooldown = .12f;
    }
}

void ADSFlightPawn::FireMissile()
{
    if (!bFlying || MissileAmmo <= 0 || MissileCooldown > 0.f) return;
    if (!Target.IsValid() || Target->bDying || LockProgress < .99f) return;
    ADSGameMode* Mode = GetWorld()->GetAuthGameMode<ADSGameMode>();
    if (!Mode || Mode->Phase != EDSPhase::Flying) return;
    const FVector Muzzle = GetActorLocation() + GetActorForwardVector() * 700.f
        + GetActorRightVector() * ((MissileAmmo % 2) ? 350.f : -350.f) - GetActorUpVector() * 100.f;
    FActorSpawnParameters Params;
    Params.Owner = this;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    ADSProjectile* Shot = GetWorld()->SpawnActor<ADSProjectile>(Muzzle, GetActorRotation(), Params);
    if (!Shot) return;
    AActor* LockedTarget = LockProgress >= .99f && Target.IsValid() ? Target.Get() : nullptr;
    Shot->Initialize(this, LockedTarget, true, true, 70.f, 760.f + Speed * .3f);
    --MissileAmmo;
    MissileCooldown = 1.2f;
    if (UAutoPlaySubsystem* AP = UAutoPlaySubsystem::Get(this); AP && AP->IsRunning())
        AP->Telemetry(TEXT("weapon_fired"), FString::Printf(TEXT("\"weapon\":\"missile\",\"ammo\":%d,\"locked\":true"), MissileAmmo));
    Mode->PlayCue(TEXT("missile"), .7f);
}

void ADSFlightPawn::UpdateCamera(float DeltaSeconds)
{
    FRotator Desired;
    float ArmLength = 5800.f;
    FVector Offset(0.f, 0.f, 850.f);
    if (!bFlying)
    {
        Desired = FRotator(-11.f, GetActorRotation().Yaw + 145.f, 0.f);
        ArmLength = 4500.f;
        Offset = FVector(0.f, -850.f, -200.f);
    }
    else
    {
        const FRotator FlightRotation = GetActorForwardVector().Rotation();
        Desired = FRotator(FlightRotation.Pitch - 4.f, FlightRotation.Yaw, 0.f);
        if (bFreeLooking)
        {
            FreeLookRotation.Yaw = FMath::Clamp(FreeLookRotation.Yaw + MouseRollInput * MouseSensitivity * .65f, -140.f, 140.f);
            FreeLookRotation.Pitch = FMath::Clamp(FreeLookRotation.Pitch + MousePitchInput * MouseSensitivity * .65f, -65.f, 65.f);
        }
        else FreeLookRotation = FMath::RInterpTo(FreeLookRotation, FRotator::ZeroRotator, DeltaSeconds, 4.f);
        Desired += FreeLookRotation;
        if ((bLookingAtTarget || TargetHoldTime > .35f) && Target.IsValid())
            Desired = (Target->GetActorLocation() - GetActorLocation()).Rotation();
        if (bCockpitView)
        {
            ArmLength = 0.f;
            Offset = FVector(1250.f, 0.f, 80.f);
        }
    }
    const float Alpha = 1.f - FMath::Exp(-3.5f * DeltaSeconds);
    CameraOrientation = FQuat::Slerp(CameraOrientation, Desired.Quaternion(), Alpha).GetNormalized();
    CameraArm->SetWorldRotation(CameraOrientation);
    CameraArm->TargetArmLength = FMath::FInterpTo(CameraArm->TargetArmLength, ArmLength, DeltaSeconds, 3.f);
    CameraArm->SocketOffset = FMath::VInterpTo(CameraArm->SocketOffset, Offset, DeltaSeconds, 3.f);
    const float DesiredFov = bFlying ? FMath::GetMappedRangeValueClamped(FVector2D(100.f, 460.f), FVector2D(76.f, 87.f), Speed) : 63.f;
    Camera->SetFieldOfView(FMath::FInterpTo(Camera->FieldOfView, DesiredFov, DeltaSeconds, 2.f));
    AircraftMesh->SetVisibility(!bCockpitView);
}

void ADSFlightPawn::ApplyCombatDamage(float Damage)
{
    if (!bFlying || Health <= 0.f || Damage <= 0.f) return;
    Health = FMath::Max(0.f, Health - Damage);
    if (ADSGameMode* Mode = GetWorld()->GetAuthGameMode<ADSGameMode>())
    {
        Mode->PlayCue(Health <= 0.f ? TEXT("explosion") : TEXT("hit"), .7f);
        if (Health <= 0.f)
        {
            bGunHeld = false;
            Mode->EndSortie(TEXT("AIRCRAFT LOST"));
        }
    }
}

void ADSFlightPawn::Rearm(int32 CannonCount, int32 MissileCount)
{
    CannonAmmo = FMath::Clamp(CannonAmmo + FMath::Max(0, CannonCount), 0, Spec.Cannon);
    MissileAmmo = FMath::Clamp(MissileAmmo + FMath::Max(0, MissileCount), 0, Spec.Missiles);
}

void ADSFlightPawn::SetMissileThreat(float Seconds)
{
    MissileThreatUntil = FMath::Max(MissileThreatUntil, GetWorld()->GetTimeSeconds() + Seconds);
    bMissileWarning = true;
}
