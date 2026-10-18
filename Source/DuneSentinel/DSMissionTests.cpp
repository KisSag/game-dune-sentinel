#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "DSGameMode.h"
#include "DSFlightPawn.h"
#include "DSEnemyAircraft.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDSMissionRulesTest,"DuneSentinel.Mission.CombatInvariants",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FDSMissionRulesTest::RunTest(const FString& Parameters)
{
    const UWorld::InitializationValues InitValues=UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(false).RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false,FName(TEXT("DuneMissionTest")),nullptr,true,ERHIFeatureLevel::Num,&InitValues);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ADSGameMode* Mode=World->SpawnActor<ADSGameMode>();
    TestEqual(TEXT("Three distinct selectable airframes"),Mode->Aircraft.Num(),3);
    TestEqual(TEXT("Waves are separated by five minutes"),Mode->WaveInterval,300.f);
    TestTrue(TEXT("Agile airframe has higher turn rate"),Mode->Aircraft[0].PitchRate>Mode->Aircraft[2].PitchRate);
    TestTrue(TEXT("Heavy airframe carries more ordnance"),Mode->Aircraft[2].Missiles>Mode->Aircraft[0].Missiles);
    Mode->Phase=EDSPhase::Flying;
    ADSEnemyAircraft* Fighter=World->SpawnActor<ADSEnemyAircraft>();
    Mode->Enemies.Add(Fighter);
    Mode->RegisterKill(Fighter,true);
    TestEqual(TEXT("Fighter destruction awards 100"),Mode->Score,100);
    Mode->RegisterKill(Fighter,true);
    TestEqual(TEXT("Repeated destruction cannot award twice"),Mode->Score,100);
    ADSEnemyAircraft* Bomber=World->SpawnActor<ADSEnemyAircraft>();
    Bomber->bBomber=true; Mode->Enemies.Add(Bomber); Mode->RegisterKill(Bomber,true);
    TestEqual(TEXT("Bomber destruction awards 200"),Mode->Score,300);
    ADSEnemyAircraft* Escape=World->SpawnActor<ADSEnemyAircraft>();
    Mode->Enemies.Add(Escape); Mode->RegisterKill(Escape,false);
    TestEqual(TEXT("Escaping enemy awards no points"),Mode->Score,300);
    Mode->AirportHealth=5000.f;
    Mode->DamageAirport(300.f);
    TestEqual(TEXT("Bomb damage reaches the airport"),Mode->AirportHealth,4700.f);
    Mode->DamageAirport(6000.f);
    TestEqual(TEXT("Airport health clamps at zero"),Mode->AirportHealth,0.f);
    TestTrue(TEXT("Destroyed airport ends sortie"),Mode->Phase==EDSPhase::Defeat);
    const int32 FinalScore=Mode->Score;
    Mode->Tick(10.f);
    TestEqual(TEXT("Survival score freezes after defeat"),Mode->Score,FinalScore);
    ADSFlightPawn* Pawn=World->SpawnActor<ADSFlightPawn>();
    Pawn->ConfigureAircraft(Mode->Aircraft[0]);
    Pawn->CannonAmmo=1; Pawn->MissileAmmo=0;
    Pawn->Rearm(100000,100000);
    TestEqual(TEXT("Rearm cannot exceed cannon capacity"),Pawn->CannonAmmo,Mode->Aircraft[0].Cannon);
    TestEqual(TEXT("Rearm cannot exceed missile capacity"),Pawn->MissileAmmo,Mode->Aircraft[0].Missiles);
    Mode->Player=Pawn;
    Mode->Phase=EDSPhase::Menu;
    Mode->SelectAircraft(0);
    const int32 ParkedScore=Mode->Score;
    Mode->Tick(10.f);
    TestEqual(TEXT("Menu never advances survival score"),Mode->Score,ParkedScore);
    Pawn->Health=1.f;
    Mode->StartSortie();
    TestTrue(TEXT("Start enables flight"),Pawn->IsFlying());
    TestEqual(TEXT("Restart restores player health"),Pawn->Health,Mode->Aircraft[0].MaxHealth);
    TestEqual(TEXT("Restart restores airport health"),Mode->AirportHealth,Mode->AirportMaxHealth);
    TestEqual(TEXT("Restart resets score"),Mode->Score,0);
    TestEqual(TEXT("Start spawns the initial wave"),Mode->Wave,1);
    TestTrue(TEXT("Initial wave contains randomized bounded enemies"),Mode->Enemies.Num()>=3 && Mode->Enemies.Num()<=7);
    int32 InitialBombers=0;
    for (ADSEnemyAircraft* Enemy : Mode->Enemies) if (IsValid(Enemy) && Enemy->bBomber) ++InitialBombers;
    TestTrue(TEXT("Initial bomber wave contains one or three aircraft"),InitialBombers==1 || InitialBombers==3);
    Mode->Tick(299.f);
    TestEqual(TEXT("No second wave before five minutes"),Mode->Wave,1);
    Mode->Tick(1.1f);
    TestEqual(TEXT("Second wave after five minutes"),Mode->Wave,2);
    TestEqual(TEXT("Survival earns one point per second"),Mode->Score,300);
    Pawn->CannonAmmo=0; Pawn->MissileAmmo=0;
    Pawn->SetActorLocation(Mode->AirportLocation+FVector(0,0,30000.f));
    Mode->Tick(1.f);
    TestEqual(TEXT("Airfield proximity grants cannon supplies"),Pawn->CannonAmmo,200);
    TestEqual(TEXT("Airfield proximity grants missiles"),Pawn->MissileAmmo,2);
    Pawn->SetActorLocation(Mode->AirportLocation+FVector(200000.f,0,30000.f));
    Mode->Tick(2.f);
    TestEqual(TEXT("Leaving the airfield stops replenishment"),Pawn->CannonAmmo,200);
    Mode->ReturnToMenu();
    TestFalse(TEXT("Returning to menu parks the aircraft"),Pawn->IsFlying());
    TestEqual(TEXT("Returning to menu removes hostile actors"),Mode->Enemies.Num(),0);
    GEngine->DestroyWorldContext(World);
    World->DestroyWorld(false);
    return true;
}
#endif
