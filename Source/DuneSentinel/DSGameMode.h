#pragma once
#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "DSGameTypes.h"
#include "DSGameMode.generated.h"

class ADSFlightPawn;
class ADSEnemyAircraft;
class UDataTable;

UCLASS()
class DUNESENTINEL_API ADSGameMode : public AGameModeBase
{
    GENERATED_BODY()
public:
    ADSGameMode();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float AirportMaxHealth = 5000.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float WaveInterval = 300.f;
    UPROPERTY(BlueprintReadOnly) EDSPhase Phase = EDSPhase::Menu;
    UPROPERTY(BlueprintReadOnly) float AirportHealth = 5000.f;
    UPROPERTY(BlueprintReadOnly) float Elapsed = 0.f;
    UPROPERTY(BlueprintReadOnly) int32 Score = 0;
    UPROPERTY(BlueprintReadOnly) int32 Wave = 0;
    UPROPERTY(BlueprintReadOnly) int32 SelectedAircraft = 1;
    UPROPERTY(BlueprintReadOnly) bool bRearming = false;
    UPROPERTY(BlueprintReadOnly) FString RadioLine;
    UPROPERTY(BlueprintReadOnly) FString DefeatReason;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FDSAircraftSpec> Aircraft;
    UPROPERTY() TArray<TObjectPtr<ADSEnemyAircraft>> Enemies;
    UPROPERTY() TObjectPtr<ADSFlightPawn> Player;
    FVector AirportLocation = FVector(0, 0, 2200);
    float ArenaHalfExtent = 1000000.f;
    int32 HighScore = 0;
    UFUNCTION(BlueprintCallable) void SelectAircraft(int32 Index);
    UFUNCTION(BlueprintCallable) void StartSortie();
    UFUNCTION(BlueprintCallable) void ReturnToMenu();
    void DamageAirport(float Damage);
    void RegisterKill(ADSEnemyAircraft* Enemy, bool bPlayerKill);
    void EndSortie(const FString& Reason);
    float SecondsToNextWave() const;
    void PlayCue(const FString& Cue, float Volume=1.f);
    void Radio(const FString& Line, float Duration=7.f);
    float GroundHeightAt(const FVector& Position) const;
    float GetBomberIngressAltitude(const FVector& Origin, int32 Count) const;
    static int32 GetBomberCountForCapacity(int32 RequestedCount, int32 Capacity);
private:
    void SpawnWave();
    void UpdateBot(float DeltaSeconds);
    void SetMissionScore(int32 NewScore, const TCHAR* Reason);
    void EmitStateTelemetry();
    float NextWaveAt = 0.f;
    float RearmAccumulator = 0.f;
    float RadioUntil = 0.f;
    int32 KillScore = 0;
    FRandomStream Random;
    bool bScenarioNotified = false;
    float BotShotAt = 0.f;
    float CaptureElapsed = 0.f;
    int32 CaptureIndex = 0;
    float StateTelemetryAccumulator = 0.f;
};
