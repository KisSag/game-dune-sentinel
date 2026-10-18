#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "DSLandscapeTools.generated.h"

class AActor;
class UTexture2D;

/** Editor authoring bridge. Raw file paths are relative to the project directory. */
UCLASS()
class DUNESENTINEL_API UDSLandscapeTools : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    /** Create a new Landscape from a JSON specification. The caller updates layers and saves packages. */
    UFUNCTION(BlueprintCallable, Category="Content Authoring", meta=(WorldContext="WorldContextObject"))
    static AActor* ImportTerrainLandscape(UObject* WorldContextObject, const FString& SpecRelativePath, FString& OutError);

    /** Export the complete, loaded final heightfield and paint weights into a new RawAssets directory. */
    UFUNCTION(BlueprintCallable, Category="Content Inspection")
    static bool ExportLandscapeSource(AActor* SourceActor, const FString& OutputRelativeDirectory, FString& OutError);

    /** Export one source mip without interpreting channels, applying gamma, or converting precision. */
    UFUNCTION(BlueprintCallable, Category="Content Inspection")
    static bool ExportTextureSourceMip0(UTexture2D* Texture, const FString& OutputRelativePath, FString& OutError);
};
