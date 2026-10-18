#include "DSPlayerController.h"
#include "DSGameMode.h"
#include "DSFlightPawn.h"
#include "Components/InputComponent.h"
#include "GameFramework/GameUserSettings.h"
#include "GameFramework/PlayerInput.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"

namespace
{
    const TCHAR* PilotSection = TEXT("DuneSentinel.PilotSettings");
    const FIntPoint DisplaySizes[] = { FIntPoint(1280,720), FIntPoint(1600,900), FIntPoint(1920,1080), FIntPoint(2560,1440) };
}

ADSPlayerController::ADSPlayerController()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bTickEvenWhenPaused = true;
    bShouldPerformFullTickWhenPaused = true;
    bShowMouseCursor = true;
    bEnableClickEvents = true;
    bEnableMouseOverEvents = true;
}

void ADSPlayerController::BeginPlay()
{
    Super::BeginPlay();
    LoadPilotSettings();
    RefreshInputMode();
}

void ADSPlayerController::SetupInputComponent()
{
    Super::SetupInputComponent();
    InputComponent->BindKey(EKeys::Escape, IE_Pressed, this, &ADSPlayerController::TogglePauseMenu).bExecuteWhenPaused = true;
    InputComponent->BindKey(EKeys::Enter, IE_Pressed, this, &ADSPlayerController::ConfirmMenu).bExecuteWhenPaused = true;
}

void ADSPlayerController::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    const ADSGameMode* Mode = GetWorld()->GetAuthGameMode<ADSGameMode>();
    if (Mode && Mode->Phase != LastPhase)
    {
        LastPhase = Mode->Phase;
        if (Mode->Phase != EDSPhase::Flying) ClearFlightControls();
        if (Mode->Phase != EDSPhase::Flying && bPaused)
        {
            SetPause(false);
            bPaused = false;
        }
        bShowSettings = false;
        bShowControls = false;
        RefreshInputMode();
    }
}

void ADSPlayerController::RefreshInputMode()
{
    const ADSGameMode* Mode = GetWorld()->GetAuthGameMode<ADSGameMode>();
    const bool bInteractive = !Mode || Mode->Phase != EDSPhase::Flying || bPaused;
    if (bInputInitialized && bInteractive == bLastInteractive) return;
    bInputInitialized = true;
    bLastInteractive = bInteractive;
    bShowMouseCursor = bInteractive;
    bEnableClickEvents = bInteractive;
    bEnableMouseOverEvents = bInteractive;
    if (bInteractive)
    {
        FInputModeGameAndUI ModeUI;
        ModeUI.SetHideCursorDuringCapture(false);
        ModeUI.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
        SetInputMode(ModeUI);
    }
    else
    {
        FInputModeGameOnly ModeGame;
        ModeGame.SetConsumeCaptureMouseDown(true);
        SetInputMode(ModeGame);
    }
}

void ADSPlayerController::ResumeFlight()
{
    ClearFlightControls();
    SetPause(false);
    bPaused = false;
    bShowSettings = false;
    bShowControls = false;
    RefreshInputMode();
}

void ADSPlayerController::ClearFlightControls()
{
    if (ADSFlightPawn* Flight = Cast<ADSFlightPawn>(GetPawn())) Flight->ClearFlightInput();
    if (PlayerInput) PlayerInput->FlushPressedKeys();
}

void ADSPlayerController::TogglePauseMenu()
{
    if (bShowSettings || bShowControls)
    {
        bShowSettings = false;
        bShowControls = false;
        return;
    }
    ADSGameMode* Mode = GetWorld()->GetAuthGameMode<ADSGameMode>();
    if (!Mode || Mode->Phase != EDSPhase::Flying) return;
    if (bPaused) ResumeFlight();
    else
    {
        ClearFlightControls();
        bPaused = SetPause(true);
        RefreshInputMode();
    }
}

void ADSPlayerController::ConfirmMenu()
{
    if (bShowControls || bShowSettings) return;
    const ADSGameMode* Mode = GetWorld()->GetAuthGameMode<ADSGameMode>();
    if (!Mode) return;
    if (bPaused) HandleMenuAction(TEXT("resume"));
    else if (Mode->Phase == EDSPhase::Menu) HandleMenuAction(TEXT("start"));
    else if (Mode->Phase == EDSPhase::Defeat) HandleMenuAction(TEXT("restart"));
}

void ADSPlayerController::HandleMenuAction(FName Action)
{
    ADSGameMode* Mode = GetWorld()->GetAuthGameMode<ADSGameMode>();
    if (!Mode) return;
    if (Mode->Phase == EDSPhase::Flying && !bPaused) return;
    if (Action == TEXT("start") || Action == TEXT("restart"))
    {
        ResumeFlight();
        Mode->StartSortie();
        LastPhase = Mode->Phase;
        RefreshInputMode();
    }
    else if (Action == TEXT("resume")) ResumeFlight();
    else if (Action == TEXT("menu"))
    {
        ResumeFlight();
        Mode->ReturnToMenu();
        LastPhase = Mode->Phase;
        RefreshInputMode();
    }
    else if (Action == TEXT("settings"))
    {
        bShowSettings = true;
        bShowControls = false;
        SettingsFeedback.Empty();
    }
    else if (Action == TEXT("controls"))
    {
        bShowControls = true;
        bShowSettings = false;
    }
    else if (Action == TEXT("back")) { bShowControls = false; bShowSettings = false; }
    else if (Action == TEXT("quit")) UKismetSystemLibrary::QuitGame(this, this, EQuitPreference::Quit, false);
    else if (Action == TEXT("aircraft0") || Action == TEXT("aircraft1") || Action == TEXT("aircraft2"))
    {
        if (Mode->Phase == EDSPhase::Menu)
            Mode->SelectAircraft(Action == TEXT("aircraft0") ? 0 : Action == TEXT("aircraft1") ? 1 : 2);
    }
    else if (Action == TEXT("volume_down") || Action == TEXT("volume_up"))
    {
        MasterVolume = FMath::Clamp(MasterVolume + (Action == TEXT("volume_up") ? .1f : -.1f), 0.f, 1.f);
        SavePilotSettings();
    }
    else if (Action == TEXT("sensitivity_down") || Action == TEXT("sensitivity_up"))
    {
        MouseSensitivity = FMath::Clamp(MouseSensitivity + (Action == TEXT("sensitivity_up") ? .1f : -.1f), .2f, 3.f);
        SavePilotSettings();
    }
    else if (Action == TEXT("invert")) { bInvertMouse = !bInvertMouse; SavePilotSettings(); }
    else if (Action == TEXT("resolution")) CycleResolution();
    else if (Action == TEXT("apply_display")) ApplyResolution();
}

void ADSPlayerController::LoadPilotSettings()
{
    if (GConfig)
    {
        GConfig->GetFloat(PilotSection, TEXT("MasterVolume"), MasterVolume, GGameUserSettingsIni);
        GConfig->GetFloat(PilotSection, TEXT("MouseSensitivity"), MouseSensitivity, GGameUserSettingsIni);
        GConfig->GetBool(PilotSection, TEXT("InvertMouse"), bInvertMouse, GGameUserSettingsIni);
    }
    MasterVolume = FMath::Clamp(MasterVolume, 0.f, 1.f);
    MouseSensitivity = FMath::Clamp(MouseSensitivity, .2f, 3.f);
    FApp::SetVolumeMultiplier(MasterVolume);
    if (const UGameUserSettings* Settings = UGameUserSettings::GetGameUserSettings())
    {
        const FIntPoint Size = Settings->GetScreenResolution();
        if (Size.X > 0 && Size.Y > 0) PendingResolution = Size;
    }
}

void ADSPlayerController::SavePilotSettings()
{
    FApp::SetVolumeMultiplier(MasterVolume);
    if (GConfig)
    {
        GConfig->SetFloat(PilotSection, TEXT("MasterVolume"), MasterVolume, GGameUserSettingsIni);
        GConfig->SetFloat(PilotSection, TEXT("MouseSensitivity"), MouseSensitivity, GGameUserSettingsIni);
        GConfig->SetBool(PilotSection, TEXT("InvertMouse"), bInvertMouse, GGameUserSettingsIni);
        GConfig->Flush(false, GGameUserSettingsIni);
    }
    SettingsFeedback = TEXT("PILOT PREFERENCES SAVED");
}

void ADSPlayerController::CycleResolution()
{
    int32 Index = -1;
    for (int32 I = 0; I < UE_ARRAY_COUNT(DisplaySizes); ++I) if (DisplaySizes[I] == PendingResolution) Index = I;
    PendingResolution = DisplaySizes[(Index + 1) % UE_ARRAY_COUNT(DisplaySizes)];
    SettingsFeedback = TEXT("SELECT APPLY DISPLAY TO CONFIRM");
}

void ADSPlayerController::ApplyResolution()
{
    if (UGameUserSettings* Settings = UGameUserSettings::GetGameUserSettings())
    {
        Settings->SetScreenResolution(PendingResolution);
        Settings->SetFullscreenMode(EWindowMode::Windowed);
        Settings->ApplySettings(false);
        Settings->SaveSettings();
        SettingsFeedback = TEXT("DISPLAY SAVED  /  WINDOWED");
    }
}

FString ADSPlayerController::ResolutionLabel() const
{
    return FString::Printf(TEXT("%d x %d"), PendingResolution.X, PendingResolution.Y);
}
