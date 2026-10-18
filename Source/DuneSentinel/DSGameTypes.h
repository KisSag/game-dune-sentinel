#pragma once
#include "CoreMinimal.h"
#include "DSGameTypes.generated.h"

UENUM(BlueprintType)
enum class EDSPhase : uint8 { Menu, Flying, Defeat };

USTRUCT(BlueprintType)
struct FDSAircraftSpec
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString Name = TEXT("F-24 PEREGRINE");
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString Role = TEXT("MULTIROLE / BALANCED");
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString MeshPath = TEXT("/Game/Art/Aircraft/SM_falcon.SM_falcon");
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float MaxHealth = 140.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Cannon = 1200;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Missiles = 16;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float CruiseSpeed = 240.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float MaxSpeed = 460.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float PitchRate = 42.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float RollRate = 120.f;
};
