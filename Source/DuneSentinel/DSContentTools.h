#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "DSContentTools.generated.h"
class UStaticMesh;

UCLASS()
class DUNESENTINEL_API UDSContentTools : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="Content Authoring")
    static bool ImportRuntimeFont(const FString& SourceFilename, const FString& FontPackagePath);
    UFUNCTION(BlueprintCallable, Category="Content Inspection")
    static FVector GetMaterialSectionCenter(UStaticMesh* Mesh, const FString& SlotName);
};
