#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "DSHUD.generated.h"

class ADSGameMode;
class ADSPlayerController;
class ADSFlightPawn;
class UFont;

UCLASS()
class DUNESENTINEL_API ADSHUD : public AHUD
{
    GENERATED_BODY()
public:
    virtual void BeginPlay() override;
    virtual void DrawHUD() override;
    virtual void NotifyHitBoxClick(FName BoxName) override;
private:
    UPROPERTY() TObjectPtr<UFont> DisplayFont;
    UPROPERTY() TObjectPtr<UFont> BodyFont;
    float UIScale = 1.f;
    FVector2D Offset = FVector2D::ZeroVector;
    FVector2D Mouse = FVector2D(-1000, -1000);
    void Text(const FString& Value, float X, float Y, float Size, FLinearColor Color, bool bDisplay = false, bool bCenter = false);
    void Rect(float X, float Y, float W, float H, FLinearColor Color);
    void Line(float X1, float Y1, float X2, float Y2, FLinearColor Color, float Width = 1.f);
    void Triangle(FVector2D A, FVector2D B, FVector2D C, FLinearColor Color);
    void Frame(float X, float Y, float W, float H, FLinearColor Color, float Corner = 16.f);
    void Panel(float X, float Y, float W, float H, const FString& Label);
    void Button(FName Name, const FString& Label, float X, float Y, float W, float H, bool bPrimary = false, bool bSelected = false);
    void Bar(float X, float Y, float W, float H, float Fraction, FLinearColor Color);
    void AmmoIcon(float X, float Y, bool bMissile, FLinearColor Color);
    void Menu(ADSGameMode* Mode);
    void Combat(ADSGameMode* Mode, ADSFlightPawn* Player);
    void Radar(ADSGameMode* Mode, ADSFlightPawn* Player);
    void Paused(ADSGameMode* Mode);
    void Debrief(ADSGameMode* Mode);
    void Settings(ADSPlayerController* PC);
    void Controls();
    void ModalBackdrop(const FString& Kicker, const FString& Title);
};
