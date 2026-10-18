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
    Agile.Name=TEXT("F-16 FALCON"); Agile.Role=TEXT("INTERCEPTOR / AGILE");
    Agile.MeshPath=TEXT("/Game/Art/Aircraft/Downloaded/SM_AU_F16.SM_AU_F16");
    Agile.ParkedMeshPath=TEXT("/Game/Art/Aircraft/Downloaded/SM_AU_F16_Parked.SM_AU_F16_Parked");
    Agile.MaxHealth=100; Agile.Cannon=900; Agile.Missiles=12;
    Agile.CruiseSpeed=250; Agile.MaxSpeed=430; Agile.PitchRate=55; Agile.RollRate=150;
    Aircraft.Add(Agile);
    FDSAircraftSpec Balanced;
    Balanced.Name=TEXT("RAFALE");
    Balanced.MeshPath=TEXT("/Game/Art/Aircraft/Downloaded/SM_AU_Rafale.SM_AU_Rafale");
    Balanced.ParkedMeshPath=TEXT("/Game/Art/Aircraft/Downloaded/SM_AU_Rafale_Parked.SM_AU_Rafale_Parked");
    Aircraft.Add(Balanced);
    FDSAircraftSpec Heavy;
    Heavy.Name=TEXT("F-22 RAPTOR"); Heavy.Role=TEXT("STRIKE FIGHTER / ENDURANCE");
    Heavy.MeshPath=TEXT("/Game/Art/Aircraft/Downloaded/SM_AU_F22.SM_AU_F22");
    Heavy.ParkedMeshPath=TEXT("/Game/Art/Aircraft/Downloaded/SM_AU_F22_Parked.SM_AU_F22_Parked");
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
        Player->SetActorLocation(FVector(-28000,16000,Player->GetParkedActorHeight(AirportLocation.Z)));
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

int32 ADSGameMode::GetBomberCountForCapacity(int32 RequestedCount, int32 Capacity)
{
    const int32 Count=FMath::Clamp(FMath::Min(RequestedCount,Capacity),0,5);
    return Count>0 && Count%2==0 ? Count-1 : Count;
}

float ADSGameMode::GetBomberIngressAltitude(const FVector& Origin, int32 Count) const
{
    // Clear the whole approach once, including both wings of every slot. A
    // common level altitude preserves the triangle while crossing tall ridges.
    const FVector Heading = (AirportLocation-Origin).GetSafeNormal2D();
    const FVector Right = FVector::CrossProduct(FVector::UpVector, Heading);
    const float Distance = FVector::Dist2D(Origin, AirportLocation);
    const int32 Steps = FMath::Max(1, FMath::CeilToInt(Distance/10000.f));
    float HighestGround = AirportLocation.Z;
    for (int32 Slot = 0; Slot < FMath::Clamp(Count, 1, 5); ++Slot)
    {
        const FVector Offset = ADSEnemyAircraft::GetFormationSlotOffset(Slot);
        const FVector SlotOrigin = Origin+Heading*Offset.X+Right*Offset.Y;
        for (int32 I = 0; I <= Steps; ++I)
        {
            const FVector Sample = SlotOrigin+Heading*(Distance*I/Steps);
            for (const float WingOffset : {-3000.f, 0.f, 3000.f})
                HighestGround = FMath::Max(HighestGround, GroundHeightAt(Sample+Right*WingOffset));
        }
    }
    return FMath::Max(100000.f, HighestGround+30000.f);
}

void ADSGameMode::SpawnWave()
{
    ++Wave;
    Enemies.RemoveAll([](ADSEnemyAircraft* Enemy){return !IsValid(Enemy) || Enemy->bDying;});
    const int32 RequestedFighters=FMath::Min(Random.RandRange(2,4)+(Wave-1)/2,8);
    const int32 RequestedBombers=FMath::Min(1+2*(Random.RandRange(0,1)+(Wave-1)/3),5);
    const int32 BomberCapacity=GetBomberCountForCapacity(RequestedBombers,24-Enemies.Num());
    const float Heading=Random.FRandRange(0,2*PI);
    FVector FormationOrigin(FMath::Cos(Heading)*850000.f,FMath::Sin(Heading)*850000.f,0.f);
    FormationOrigin.Z=GetBomberIngressAltitude(FormationOrigin,BomberCapacity);
    const FVector FormationHeading=(AirportLocation-FormationOrigin).GetSafeNormal2D();
    const FVector FormationRight=FVector::CrossProduct(FVector::UpVector,FormationHeading);
    auto SpawnEnemy=[this](bool bBomber,const FVector& Position,const FRotator& Rotation)
    {
        FActorSpawnParameters Params;
        Params.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        ADSEnemyAircraft* Enemy=GetWorld()->SpawnActor<ADSEnemyAircraft>(Position,Rotation,Params);
        if (Enemy) Enemy->Initialize(bBomber,AirportLocation);
        return Enemy;
    };
    auto SpawnBomber=[&](int32 Slot)
    {
        const FVector Offset=ADSEnemyAircraft::GetFormationSlotOffset(Slot);
        return SpawnEnemy(true,FormationOrigin+FormationHeading*Offset.X+FormationRight*Offset.Y,FormationHeading.Rotation());
    };
    TArray<ADSEnemyAircraft*> NewBombers;
    if (BomberCapacity>0)
    {
        if (ADSEnemyAircraft* Lead=SpawnBomber(0))
        {
            NewBombers.Add(Lead);
            for (int32 Slot=1;Slot<BomberCapacity;Slot+=2)
            {
                ADSEnemyAircraft* Left=SpawnBomber(Slot);
                ADSEnemyAircraft* Right=SpawnBomber(Slot+1);
                // A wing pair joins atomically, before either aircraft can tick or become a target.
                if (!Left || !Right)
                {
                    if (Left) Left->Destroy();
                    if (Right) Right->Destroy();
                    break;
                }
                NewBombers.Add(Left);
                NewBombers.Add(Right);
            }
        }
    }
    for (int32 Slot=0;Slot<NewBombers.Num();++Slot)
    {
        ADSEnemyAircraft* Bomber=NewBombers[Slot];
        Bomber->InitializeFormation(Wave,Slot,NewBombers.Num(),FormationOrigin,FormationHeading);
        Enemies.Add(Bomber);
    }
    int32 Fighters=0;
    for (int32 I=0;I<RequestedFighters && Enemies.Num()<24;++I)
    {
        const float Angle=Heading+(I-RequestedFighters*.5f)*.065f;
        FVector Pos(FMath::Cos(Angle)*850000.f,FMath::Sin(Angle)*850000.f,130000.f+I*6000);
        Pos.Z=FMath::Max(Pos.Z,GetBomberIngressAltitude(Pos,1));
        const FRotator Facing=(AirportLocation+FVector(0,0,65000)-Pos).Rotation();
        if (ADSEnemyAircraft* Enemy=SpawnEnemy(false,Pos,Facing))
        {
            Enemies.Add(Enemy);
            ++Fighters;
        }
    }
    if (Wave>1) Radio(FString::Printf(TEXT("OVERWATCH: Wave %02d inbound. Prioritize the bombers."),Wave));
    if (UAutoPlaySubsystem* AP=UAutoPlaySubsystem::Get(this))
        AP->Telemetry(TEXT("wave_spawned"),FString::Printf(TEXT("\"wave\":%d,\"fighters\":%d,\"bombers\":%d,\"formation_group\":%d"),Wave,Fighters,NewBombers.Num(),Wave));
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
