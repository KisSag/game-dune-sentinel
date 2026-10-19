#pragma once
#include "CoreMinimal.h"
class UWorld;
// Observation only: never saves packages, changes actors, or produces media.
bool WriteNativeSurfaceState(UWorld* World, const FString& OutputDirectory);
