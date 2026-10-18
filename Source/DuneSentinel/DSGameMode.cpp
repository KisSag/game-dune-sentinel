#include "DSGameMode.h"
#include "DSFlightPawn.h"
#include "DSEnemyAircraft.h"
#include "DSProjectile.h"
#include "DSHUD.h"
#include "DSPlayerController.h"
#include "DuneSentinelGameInstance.h"
#include "AutoPlay/AutoPlaySubsystem.h"
#include "Kismet/GameplayStatics.h"
#include "EngineUtils.h"
#include "Sound/SoundBase.h"
#include "Misc/ConfigCacheIni.h"
#include "InputActionValue.h"
#include "Camera/PlayerCameraManager.h"
#include "HighResScreenshot.h"
#include "UnrealClient.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

ADSGameMode::ADSGameMode()
{
    PrimaryActorTick.bCanEverTick=true;
    DefaultPawnClass=ADSFlightPawn::StaticClass();
    PlayerControllerClass=ADSPlayerController::StaticClass();
    HUDClass=ADSHUD::StaticClass();
    FDSAircraftSpec Agile;
    Agile.Name=TEXT("F-11 KESTREL"); Agile.Role=TEXT("INTERCEPTOR / AGILE");
    Agile.MeshPath=TEXT("/Game/Art/Aircraft/SM_lancer.SM_lancer");
    Agile.MaxHealth=100; Agile.Cannon=900; Agile.Missiles=12;
    Agile.CruiseSpeed=250; Agile.MaxSpeed=430; Agile.PitchRate=55; Agile.RollRate=150;
    Aircraft.Add(Agile);
    FDSAircraftSpec Balanced;
    Balanced.MeshPath=TEXT("/Game/Art/Aircraft/SM_mirage.SM_mirage");
    Aircraft.Add(Balanced);
    FDSAircraftSpec Heavy;
    Heavy.Name=TEXT("F-37 OSPREY"); Heavy.Role=TEXT("STRIKE FIGHTER / ENDURANCE");
    Heavy.MeshPath=TEXT("/Game/Art/Aircraft/SM_falcon.SM_falcon");
    Heavy.MaxHealth=180; Heavy.Cannon=1600; Heavy.Missiles=20;
    Heavy.CruiseSpeed=250; Heavy.MaxSpeed=500; Heavy.PitchRate=34; Heavy.RollRate=100;
    Aircraft.Add(Heavy);
}

void ADSGameMode::BeginPlay()
{
    Super::BeginPlay();
    Player=Cast<ADSFlightPawn>(UGameplayStatics::GetPlayerPawn(this,0));
    if (APlayerController* PC=UGameplayStatics::GetPlayerController(this,0))
    {
        PC->AddTickPrerequisiteActor(this);
        if(Player) Player->AddTickPrerequisiteActor(PC);
    }
    AirportHealth=AirportMaxHealth;
    GConfig->GetInt(TEXT("DuneSentinel"),TEXT("HighScore"),HighScore,GGameUserSettingsIni);
    UAutoPlaySubsystem* AP=UAutoPlaySubsystem::Get(this);
    Random.Initialize(AP && AP->IsAutoPlay() ? AP->GetSeed() : FMath::Rand());
    SelectAircraft(SelectedAircraft);
    if (AP && AP->IsRunning())
    {
        StartSortie();
        AP->ScenarioStarted(TEXT("Glass Horizon - Airfield Defense"));
        bScenarioNotified=true;
    }
}

void ADSGameMode::SelectAircraft(int32 Index)
{
    if (Phase!=EDSPhase::Menu || !Aircraft.IsValidIndex(Index)) return;
    SelectedAircraft=Index;
    if (Player)
    {
        Player->ConfigureAircraft(Aircraft[Index]);
        Player->SetActorLocation(FVector(-28000,16000,2600));
        Player->SetActorRotation(FRotator(0,18,0));
        Player->Park();
    }
}

void ADSGameMode::StartSortie()
{
    if (!Player) return;
    const bool bRestart = Wave > 0;
    if (UAutoPlaySubsystem* AP=UAutoPlaySubsystem::Get(this); AP && AP->IsRunning() && bRestart)
        AP->Telemetry(TEXT("restart"), TEXT("\"reason\":") + UAutoPlaySubsystem::JsonStr(Phase==EDSPhase::Defeat ? TEXT("retry_after_defeat") : TEXT("sortie_restart")));
    UGameplayStatics::SetGamePaused(this,false);
    for (TActorIterator<ADSEnemyAircraft> It(GetWorld());It;++It) It->Destroy();
    for (TActorIterator<ADSProjectile> It(GetWorld());It;++It) It->Destroy();
    for (TActorIterator<ADSBomb> It(GetWorld());It;++It) It->Destroy();
    Enemies.Reset();
    AirportHealth=AirportMaxHealth;
    Elapsed=0; KillScore=0; Wave=0; NextWaveAt=WaveInterval;
    SetMissionScore(0,bRestart ? TEXT("sortie_restart") : TEXT("sortie_start"));
    RearmAccumulator=0; bRearming=false; DefeatReason.Empty();
    StateTelemetryAccumulator=0.f;
    Player->ConfigureAircraft(Aircraft[SelectedAircraft]);
    Player->SetActorLocation(FVector(-160000,0,68000));
    Player->SetActorRotation(FRotator(4,0,0));
    Player->BeginFlight();
    Phase=EDSPhase::Flying;
    Radio(TEXT("TOWER: Evacuation convoy is moving. Keep this runway alive."),9.f);
    SpawnWave();
    if (APlayerController* PC=UGameplayStatics::GetPlayerController(this,0))
        PC->PlayerCameraManager->StartCameraFade(1.f,0.f,1.25f,FLinearColor::Black,false,false);
    if (UAutoPlaySubsystem* AP=UAutoPlaySubsystem::Get(this))
        AP->Telemetry(TEXT("sortie_started"),FString::Printf(TEXT("\"airframe\":%d,\"wave_interval\":%.1f"),SelectedAircraft,WaveInterval));
    EmitStateTelemetry();
}

void ADSGameMode::ReturnToMenu()
{
    UGameplayStatics::SetGamePaused(this,false);
    Phase=EDSPhase::Menu;
    for (TActorIterator<ADSEnemyAircraft> It(GetWorld());It;++It) It->Destroy();
    for (TActorIterator<ADSProjectile> It(GetWorld());It;++It) It->Destroy();
    for (TActorIterator<ADSBomb> It(GetWorld());It;++It) It->Destroy();
    Enemies.Reset();
    bRearming=false;
    RadioLine.Empty();
    SelectAircraft(SelectedAircraft);
}

void ADSGameMode::Tick(float Dt)
{
    Super::Tick(Dt);
    if (FParse::Param(FCommandLine::Get(),TEXT("CaptureFrames")))
    {
        CaptureElapsed+=Dt;
        if (CaptureIndex<10 && CaptureElapsed>=3.f+CaptureIndex*8.f)
        {
            FScreenshotRequest::RequestScreenshot(FString::Printf(TEXT("DuneSentinel_%s_%02d.png"),Phase==EDSPhase::Menu?TEXT("Menu"):TEXT("Flight"),CaptureIndex),true,false);
            ++CaptureIndex;
        }
    }
    if (!Player) Player=Cast<ADSFlightPawn>(UGameplayStatics::GetPlayerPawn(this,0));
    if (Phase!=EDSPhase::Flying) return;
    Elapsed+=Dt;
    SetMissionScore(KillScore+FMath::FloorToInt(Elapsed),TEXT("survival"));
    if (Elapsed>=NextWaveAt)
    {
        SpawnWave();
        NextWaveAt+=WaveInterval;
    }
    if (Elapsed>RadioUntil) RadioLine.Empty();
    if (Player)
    {
        bRearming=FVector::Dist2D(Player->GetActorLocation(),AirportLocation)<100000.f && Player->GetActorLocation().Z-AirportLocation.Z<100000.f;
        if (bRearming)
        {
            RearmAccumulator+=Dt;
            if (RearmAccumulator>=1.f)
            {
                const int32 Ticks=FMath::FloorToInt(RearmAccumulator);
                Player->Rearm(200*Ticks,2*Ticks);
                RearmAccumulator-=Ticks;
            }
        }
        else RearmAccumulator=0;
        if (Player->Health<=0) EndSortie(TEXT("AIRCRAFT LOST"));
    }
    if (UAutoPlaySubsystem* AP=UAutoPlaySubsystem::Get(this))
    {
        if (AP->IsRunning() && Player)
        {
            StateTelemetryAccumulator+=Dt;
            if (StateTelemetryAccumulator>=.1f)
            {
                StateTelemetryAccumulator=FMath::Fmod(StateTelemetryAccumulator,.1f);
                EmitStateTelemetry();
            }
        }
        if (AP->IsRunning()) UpdateBot(Dt);
    }
}

float ADSGameMode::SecondsToNextWave() const
{
    return FMath::Max(0.f,NextWaveAt-Elapsed);
}

void ADSGameMode::SpawnWave()
{
    ++Wave;
    Enemies.RemoveAll([](ADSEnemyAircraft* Enemy){return !IsValid(Enemy) || Enemy->bDying;});
    const int32 Fighters=FMath::Min(Random.RandRange(2,4)+(Wave-1)/2,8);
    const int32 Bombers=FMath::Min(Random.RandRange(1,2)+(Wave-1)/3,4);
    const float Heading=Random.FRandRange(0,2*PI);
    for (int32 I=0;I<Fighters+Bombers && Enemies.Num()<24;++I)
    {
        const bool Bomber=I>=Fighters;
        const float Angle=Heading+(I-(Fighters+Bombers)*.5f)*.065f;
        FVector Pos(FMath::Cos(Angle)*850000.f,FMath::Sin(Angle)*850000.f,Bomber?100000.f:130000.f+I*6000);
        const FRotator Facing=(AirportLocation+FVector(0,0,65000)-Pos).Rotation();
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        ADSEnemyAircraft* Enemy=GetWorld()->SpawnActor<ADSEnemyAircraft>(Pos,Facing,Params);
        if (Enemy) {Enemy->Initialize(Bomber,AirportLocation);Enemies.Add(Enemy);}
    }
    if (Wave>1) Radio(FString::Printf(TEXT("OVERWATCH: Wave %02d inbound. Prioritize the bombers."),Wave));
    if (UAutoPlaySubsystem* AP=UAutoPlaySubsystem::Get(this))
        AP->Telemetry(TEXT("wave_spawned"),FString::Printf(TEXT("\"wave\":%d,\"fighters\":%d,\"bombers\":%d"),Wave,Fighters,Bombers));
}

void ADSGameMode::RegisterKill(ADSEnemyAircraft* Enemy,bool bPlayerKill)
{
    if (!Enemy || Phase!=EDSPhase::Flying || Enemies.Remove(Enemy)==0) return;
    if (bPlayerKill)
    {
        const int32 Points=Enemy->bBomber?200:100;
        KillScore+=Points;
        SetMissionScore(KillScore+FMath::FloorToInt(Elapsed),Enemy->bBomber?TEXT("bomber_destroyed"):TEXT("fighter_destroyed"));
        Radio(Enemy->bBomber?TEXT("OVERWATCH: Bomber destroyed. Airfield threat reduced. +200"):TEXT("SENTINEL: Splash one. +100"),3.f);
        if (UAutoPlaySubsystem* AP=UAutoPlaySubsystem::Get(this))
            AP->Telemetry(TEXT("enemy_destroyed"),FString::Printf(TEXT("\"bomber\":%s,\"points\":%d"),Enemy->bBomber?TEXT("true"):TEXT("false"),Points));
    }
}

void ADSGameMode::DamageAirport(float Damage)
{
    if (Phase!=EDSPhase::Flying || Damage<=0.f) return;
    AirportHealth=FMath::Clamp(AirportHealth-Damage,0.f,AirportMaxHealth);
    Radio(TEXT("TOWER: Runway perimeter hit. We need air cover!"),5.f);
    if (UAutoPlaySubsystem* AP=UAutoPlaySubsystem::Get(this))
        AP->Telemetry(TEXT("airport_damaged"),FString::Printf(TEXT("\"health\":%.1f,\"damage\":%.1f"),AirportHealth,Damage));
    if (AirportHealth<=0.f) EndSortie(TEXT("AIRFIELD DESTROYED"));
}

void ADSGameMode::EndSortie(const FString& Reason)
{
    if (Phase!=EDSPhase::Flying) return;
    Phase=EDSPhase::Defeat;
    DefeatReason=Reason;
    bRearming=false;
    if (FParse::Param(FCommandLine::Get(),TEXT("CaptureFrames")))
        FScreenshotRequest::RequestScreenshot(TEXT("DuneSentinel_Debrief.png"),true,false);
    HighScore=FMath::Max(HighScore,Score);
    if (GetWorld()->HasBegunPlay())
    {
        GConfig->SetInt(TEXT("DuneSentinel"),TEXT("HighScore"),HighScore,GGameUserSettingsIni);
        GConfig->Flush(false,GGameUserSettingsIni);
    }
    if (UAutoPlaySubsystem* AP=UAutoPlaySubsystem::Get(this); AP && AP->IsRunning())
    {
        EmitStateTelemetry();
        AP->FinishRun(TEXT("lose"),Score,TEXT("\"reason\":")+UAutoPlaySubsystem::JsonStr(Reason));
    }
}

void ADSGameMode::SetMissionScore(int32 NewScore,const TCHAR* Reason)
{
    if (NewScore==Score) return;
    const int32 Delta=NewScore-Score;
    Score=NewScore;
    if (UAutoPlaySubsystem* AP=UAutoPlaySubsystem::Get(this))
    {
        AP->SetScore(Score);
        if (AP->IsRunning())
            AP->Telemetry(TEXT("score"),FString::Printf(TEXT("\"score\":%d,\"delta\":%d,\"reason\":%s,\"wave\":%d"),Score,Delta,*UAutoPlaySubsystem::JsonStr(Reason),Wave));
    }
}

void ADSGameMode::EmitStateTelemetry()
{
    UAutoPlaySubsystem* AP=UAutoPlaySubsystem::Get(this);
    if (!AP || !AP->IsRunning() || !Player) return;
    const FRotator Rotation=Player->GetActorRotation();
    AP->Telemetry(TEXT("state"),FString::Printf(TEXT("\"health\":%.2f,\"airport_health\":%.2f,\"cannon\":%d,\"missiles\":%d,\"speed_mps\":%.2f,\"rearming\":%s,\"position\":%s,\"rotation\":%s,\"velocity\":%s"),
        Player->Health,AirportHealth,Player->CannonAmmo,Player->MissileAmmo,Player->Speed,bRearming?TEXT("true"):TEXT("false"),
        *UAutoPlaySubsystem::JsonVec(Player->GetActorLocation()),*UAutoPlaySubsystem::JsonVec(FVector(Rotation.Pitch,Rotation.Yaw,Rotation.Roll)),*UAutoPlaySubsystem::JsonVec(Player->GetVelocity())));
}

void ADSGameMode::PlayCue(const FString& Cue,float Volume)
{
    const FString Path=FString::Printf(TEXT("/Game/Audio/S_%s.S_%s"),*Cue,*Cue);
    if (USoundBase* Sound=LoadObject<USoundBase>(nullptr,*Path)) UGameplayStatics::PlaySound2D(this,Sound,Volume);
}

void ADSGameMode::Radio(const FString& Line,float Duration)
{
    RadioLine=Line;
    RadioUntil=Elapsed+Duration;
}

float ADSGameMode::GroundHeightAt(const FVector& Position) const
{
    if (Position.SizeSquared2D()<180000.f*180000.f) return 2200.f;
    FHitResult Hit;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(GroundHeight),true);
    if (Player) Params.AddIgnoredActor(Player);
    FCollisionObjectQueryParams GroundObjects;
    GroundObjects.AddObjectTypesToQuery(ECC_WorldStatic);
    if (GetWorld()->LineTraceSingleByObjectType(Hit,FVector(Position.X,Position.Y,1500000.f),FVector(Position.X,Position.Y,-50000.f),GroundObjects,Params)) return Hit.ImpactPoint.Z;
    return 2180.f;
}

void ADSGameMode::UpdateBot(float Dt)
{
    UAutoPlaySubsystem* AP=UAutoPlaySubsystem::Get(this);
    APlayerController* PC=UGameplayStatics::GetPlayerController(this,0);
    if (!AP || !Player || !PC || Phase!=EDSPhase::Flying) return;
    const float Skill=AP->GetSkill();
    const float Roll=FMath::Sin(Elapsed*.14f)*.18f*Skill;
    const float Pitch=(Player->GetActorLocation().Z<120000.f?.17f:.02f)*Skill;
    AP->Inject(PC,Player->RollAction,FInputActionValue(Roll),Roll<0.f?TEXT("A"):TEXT("D"));
    AP->Inject(PC,Player->PitchAction,FInputActionValue(Pitch),Pitch<0.f?TEXT("Up"):TEXT("Down"));
    if (Elapsed<4.f) AP->Inject(PC,Player->ThrottleAction,FInputActionValue(.35f),TEXT("W"));
    if (FMath::Fmod(Elapsed,11.f)<Dt) AP->Inject(PC,Player->TargetAction,FInputActionValue(true),TEXT("Tab"));
    if (FMath::Fmod(Elapsed,6.f)>4.5f) AP->Inject(PC,Player->GunAction,FInputActionValue(true),TEXT("LeftMouseButton"));
    if (FMath::Fmod(Elapsed,9.f)<Dt) AP->Inject(PC,Player->MissileAction,FInputActionValue(true),TEXT("RightMouseButton"));
}
