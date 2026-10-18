#include "DSImpactFX.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

ADSImpactFX::ADSImpactFX()
{
    PrimaryActorTick.bCanEverTick=true;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Origin")));
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Shape(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    for (int32 I=0;I<12;++I)
    {
        UStaticMeshComponent* P=CreateDefaultSubobject<UStaticMeshComponent>(*FString::Printf(TEXT("Ember%d"),I));
        P->SetupAttachment(RootComponent);
        P->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        P->SetCastShadow(false);
        if(Shape.Succeeded()) P->SetStaticMesh(Shape.Object);
        Particles.Add(P);
    }
    SetLifeSpan(1.7f);
}
void ADSImpactFX::BeginPlay()
{
    Super::BeginPlay();
    if (UMaterialInterface* Material=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Art/Effects/M_Explosion.M_Explosion")))
        Glow=UMaterialInstanceDynamic::Create(Material,this);
    for(int32 I=0;I<Particles.Num();++I)
    {
        if(Glow) Particles[I]->SetMaterial(0,Glow);
        Directions.Add(FMath::VRand()*FMath::FRandRange(1000.f,2800.f)*Size);
    }
}
void ADSImpactFX::Tick(float Dt)
{
    Super::Tick(Dt);
    Age+=Dt;
    if(Glow)
    {
        Glow->SetScalarParameterValue(TEXT("Opacity"),FMath::Clamp((1.65f-Age)*.8f,0.f,1.f));
        Glow->SetVectorParameterValue(TEXT("Color"),FLinearColor(12.f, FMath::Max(.15f,4.f-Age*4.f),.05f));
    }
    for(int32 I=0;I<Particles.Num();++I)
    {
        Particles[I]->SetRelativeLocation(Directions[I]*Age+FVector(0,0,300.f*Age*Age));
        const float Radius=Size*(I<4?10.f:2.5f)*(1.f+Age*2.f);
        Particles[I]->SetRelativeScale3D(FVector(Radius,Radius*(I<4?1.f:.35f),Radius));
    }
}
