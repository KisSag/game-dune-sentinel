#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "InputActionValue.h"
#include "DSGameTypes.h"
#include "DSFlightPawn.generated.h"

class UInputAction;
class UInputMappingContext;
class USphereComponent;
class UStaticMeshComponent;
class UCameraComponent;
class USpringArmComponent;
class UAudioComponent;
class UDSCannonFXComponent;
class ADSEnemyAircraft;

UCLASS()
class DUNESENTINEL_API ADSFlightPawn : public APawn
{
    GENERATED_BODY()
public:
    ADSFlightPawn();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
    virtual FVector GetVelocity() const override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    UPROPERTY(BlueprintReadOnly) float Health = 140.f;
    UPROPERTY(BlueprintReadOnly) float Speed = 0.f;
    UPROPERTY(BlueprintReadOnly) int32 CannonAmmo = 1200;
    UPROPERTY(BlueprintReadOnly) int32 MissileAmmo = 16;
    UPROPERTY(BlueprintReadOnly) FDSAircraftSpec Spec;
    UPROPERTY(BlueprintReadOnly) TWeakObjectPtr<ADSEnemyAircraft> Target;
    UPROPERTY(BlueprintReadOnly) float LockProgress = 0.f;
    UPROPERTY(BlueprintReadOnly) bool bMissileWarning = false;
    UPROPERTY(BlueprintReadOnly) bool bStalling = false;
    UPROPERTY(BlueprintReadOnly) bool bHighG = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float MouseSensitivity = 1.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bInvertMouseY = true;

    UPROPERTY() TObjectPtr<UInputAction> PitchAction;
    UPROPERTY() TObjectPtr<UInputAction> RollAction;
    UPROPERTY() TObjectPtr<UInputAction> YawAction;
    UPROPERTY() TObjectPtr<UInputAction> ThrottleAction;
    UPROPERTY() TObjectPtr<UInputAction> BrakeAction;
    UPROPERTY() TObjectPtr<UInputAction> GunAction;
    UPROPERTY() TObjectPtr<UInputAction> MissileAction;
    UPROPERTY() TObjectPtr<UInputAction> TargetAction;

    void ConfigureAircraft(const FDSAircraftSpec& Aircraft);
    void Park();
    void BeginFlight();
    void ApplyCombatDamage(float Damage);
    void Rearm(int32 CannonCount, int32 MissileCount);
    void ClearFlightInput() { ResetControls(); }
    bool IsFlying() const { return bFlying; }
    UCameraComponent* GetFlightCamera() const { return Camera; }
    void SetMissileThreat(float Seconds = 1.5f);
    void FireMissile();

private:
    UPROPERTY(VisibleAnywhere) TObjectPtr<USphereComponent> Collision;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> AircraftMesh;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> LandingGear;
    UPROPERTY(VisibleAnywhere) TObjectPtr<USpringArmComponent> CameraArm;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UCameraComponent> Camera;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UAudioComponent> EngineAudio;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UAudioComponent> WindAudio;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UDSCannonFXComponent> CannonFX;
    UPROPERTY() TObjectPtr<UInputMappingContext> FlightMapping;
    UPROPERTY() TObjectPtr<UInputAction> MousePitchAction;
    UPROPERTY() TObjectPtr<UInputAction> MouseRollAction;
    UPROPERTY() TObjectPtr<UInputAction> LookAction;
    UPROPERTY() TObjectPtr<UInputAction> FreeLookAction;
    UPROPERTY() TObjectPtr<UInputAction> CameraAction;
    UPROPERTY() TObjectPtr<UInputAction> YawLeftAction;
    UPROPERTY() TObjectPtr<UInputAction> YawRightAction;
    void MakeInputActions();
    void InstallInputContext();
    void OnPitch(const FInputActionValue& Value);
    void OnRoll(const FInputActionValue& Value);
    void OnYaw(const FInputActionValue& Value);
    void OnThrottle(const FInputActionValue& Value);
    void OnBrake(const FInputActionValue& Value);
    void OnGun(const FInputActionValue& Value);
    void OnMissile(const FInputActionValue& Value);
    void OnTarget(const FInputActionValue& Value);
    void OnTargetHeld(const FInputActionValue& Value);
    void OnYawLeft(const FInputActionValue& Value);
    void OnYawRight(const FInputActionValue& Value);
    void OnMousePitch(const FInputActionValue& Value);
    void OnMouseRoll(const FInputActionValue& Value);
    void OnLook(const FInputActionValue& Value);
    void OnFreeLook(const FInputActionValue& Value);
    void OnCamera(const FInputActionValue& Value);
    void ResetControls();
    void UpdateFlight(float DeltaSeconds);
    void UpdateTarget(float DeltaSeconds);
    void UpdateCamera(float DeltaSeconds);
    void FireGun();
    void SelectNextTarget();
    bool bFlying = false;
    bool bGunHeld = false;
    bool bLookingAtTarget = false;
    bool bTargetHeld = false;
    bool bFreeLooking = false;
    bool bCockpitView = false;
    bool bContextInstalled = false;
    float PitchInput = 0.f;
    float RollInput = 0.f;
    float YawInput = 0.f;
    float YawLeftInput = 0.f;
    float YawRightInput = 0.f;
    float TargetHoldTime = 0.f;
    float ThrottleInput = 0.f;
    float BrakeInput = 0.f;
    float MousePitchInput = 0.f;
    float MouseRollInput = 0.f;
    FVector AngularRates = FVector::ZeroVector;
    FVector FlightVelocity = FVector::ZeroVector;
    FRotator FreeLookRotation = FRotator::ZeroRotator;
    FQuat CameraOrientation = FQuat::Identity;
    float GunCooldown = 0.f;
    float MissileCooldown = 0.f;
    float GunSoundCooldown = 0.f;
    float MissileThreatUntil = 0.f;
    float BoundaryTime = 0.f;
    float EngineVolume = .09f;
    float WindVolume = 0.f;
    float EnginePitch = .75f;
    bool bLockSoundPlayed = false;
};
