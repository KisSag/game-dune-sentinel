#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "DSDesertScatter.generated.h"

class UHierarchicalInstancedStaticMeshComponent;

/** Persistent batches of decorative desert meshes authored by scene scripts. */
UCLASS(BlueprintType)
class DUNESENTINEL_API ADSDesertScatter : public AActor
{
    GENERATED_BODY()

public:
    ADSDesertScatter();

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Desert Scatter")
    TObjectPtr<UHierarchicalInstancedStaticMeshComponent> Instances;
};
