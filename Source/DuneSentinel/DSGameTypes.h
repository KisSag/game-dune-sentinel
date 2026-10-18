#pragma once
#include "CoreMinimal.h"
#include "DSGameTypes.generated.h"

UENUM(BlueprintType)
enum class EDSPhase : uint8 { Menu, Flying, Defeat };

USTRUCT(BlueprintType)
struct FDSAircraftSpec
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString Name = TEXT("RAFALE");
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString Role = TEXT("MULTIROLE / BALANCED");
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString MeshPath = TEXT("/Game/Art/Aircraft/Downloaded/SM_AU_Rafale.SM_AU_Rafale");
    // Optional separate extended gear in the airframe's coordinate frame.
    // An empty path retains the original shared landing gear.
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString GearMeshPath;
    // Optional complete parked pose when gear doors and struts are animated.
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString ParkedMeshPath = TEXT("/Game/Art/Aircraft/Downloaded/SM_AU_Rafale_Parked.SM_AU_Rafale_Parked");
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float MaxHealth = 140.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Cannon = 1200;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Missiles = 16;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float CruiseSpeed = 240.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float MaxSpeed = 460.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float PitchRate = 42.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float RollRate = 120.f;
};
