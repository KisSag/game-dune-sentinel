#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "DSGameTypes.h"
#include "DSPlayerController.generated.h"

UCLASS()
class DUNESENTINEL_API ADSPlayerController : public APlayerController
{
    GENERATED_BODY()
public:
    ADSPlayerController();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void SetupInputComponent() override;

    UPROPERTY(BlueprintReadOnly) float MouseSensitivity = 1.f;
    UPROPERTY(BlueprintReadOnly) bool bInvertMouse = false;
    UPROPERTY(BlueprintReadOnly) float MasterVolume = .7f;
    UPROPERTY(BlueprintReadOnly) bool bPaused = false;
    bool bShowSettings = false;
    bool bShowControls = false;
    FIntPoint PendingResolution = FIntPoint(1920, 1080);
    FString SettingsFeedback;

    void HandleMenuAction(FName Action);
    void TogglePauseMenu();
    void RefreshInputMode();
    FString ResolutionLabel() const;
private:
    void LoadPilotSettings();
    void SavePilotSettings();
    void ApplyResolution();
    void CycleResolution();
    void ConfirmMenu();
    void ResumeFlight();
    void ClearFlightControls();
    bool bInputInitialized = false;
    bool bLastInteractive = false;
    EDSPhase LastPhase = EDSPhase::Menu;
};
