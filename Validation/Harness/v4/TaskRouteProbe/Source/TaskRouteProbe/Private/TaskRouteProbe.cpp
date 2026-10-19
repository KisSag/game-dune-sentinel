#include "Modules/ModuleManager.h"
#include "SurfaceState.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTLS.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "InputKeyEventArgs.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"
#include "UnrealClient.h"

namespace TaskRoute
{
using FObject = TSharedPtr<FJsonObject>;
static FObject Object() { return MakeShared<FJsonObject>(); }
static TSharedPtr<FJsonValue> Value(const FObject& O) { return MakeShared<FJsonValueObject>(O); }
static TArray<TSharedPtr<FJsonValue>> Vector(const FVector& V)
{
    return { MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z) };
}
static FString Serialize(const FObject& O)
{
    FString Text;
    const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
    FJsonSerializer::Serialize(O.ToSharedRef(), Writer);
    return Text + TEXT("\n");
}
static void Reflect(const UObject* O, const TCHAR* Name, const FObject& Out)
{
    if (!O) return;
    const FProperty* P = O->GetClass()->FindPropertyByName(Name);
    if (!P) return;
    const void* Address = P->ContainerPtrToValuePtr<void>(O);
    if (const FBoolProperty* B = CastField<FBoolProperty>(P)) Out->SetBoolField(Name, B->GetPropertyValue(Address));
    else if (const FEnumProperty* E = CastField<FEnumProperty>(P))
    {
        const int64 N = E->GetUnderlyingProperty()->GetSignedIntPropertyValue(Address);
        Out->SetNumberField(Name, N);
        Out->SetStringField(FString(Name) + TEXT("_name"), E->GetEnum()->GetNameStringByValue(N));
    }
    else if (const FNumericProperty* N = CastField<FNumericProperty>(P))
        Out->SetNumberField(Name, N->IsFloatingPoint() ? N->GetFloatingPointPropertyValue(Address) : double(N->GetSignedIntPropertyValue(Address)));
    else if (const FStrProperty* S = CastField<FStrProperty>(P)) Out->SetStringField(Name, S->GetPropertyValue(Address));
    else if (const FObjectPropertyBase* Reference = CastField<FObjectPropertyBase>(P))
    {
        const UObject* Target = Reference->GetObjectPropertyValue(Address);
        Out->SetStringField(Name, Target ? Target->GetName() : TEXT(""));
    }
}
struct FEvent { double Time = 0; FKey Key; bool Press = false; };

// Diagnostic-only spans. Ordinary observation/input code remains synchronous.
// Numeric records are buffered; the additional stream is written once at shutdown.
struct FTimingEvent
{
    const TCHAR* Stage = nullptr;
    double Begin = 0, End = 0, WorldBegin = 0, WorldEnd = 0, RouteBegin = 0, RouteEnd = 0;
    uint64 FrameBegin = 0, FrameEnd = 0;
    uint32 ThreadBegin = 0, ThreadEnd = 0;
};
struct FTimingBuffer
{
    TArray<FTimingEvent> Events;
    FCriticalSection Mutex;
    void Add(const FTimingEvent& Event) { FScopeLock Lock(&Mutex); Events.Add(Event); }
};
class FTimingSpan
{
public:
    FTimingSpan(FTimingBuffer& InBuffer, const TCHAR* Stage, UWorld* InWorld, const double* InRoute)
        : Buffer(InBuffer), ObservedWorld(InWorld), Route(InRoute)
    {
        Event.Stage = Stage; Event.ThreadBegin = FPlatformTLS::GetCurrentThreadId();
        Event.FrameBegin = GFrameCounter; Event.WorldBegin = InWorld ? InWorld->GetTimeSeconds() : -1;
        Event.RouteBegin = Route ? *Route : -1; Event.Begin = FPlatformTime::Seconds();
    }
    ~FTimingSpan() { Stop(); }
    void Stop()
    {
        if (Stopped) return;
        Event.End = FPlatformTime::Seconds(); Event.ThreadEnd = FPlatformTLS::GetCurrentThreadId();
        Event.FrameEnd = GFrameCounter; Event.WorldEnd = ObservedWorld.IsValid() ? ObservedWorld->GetTimeSeconds() : -1;
        Event.RouteEnd = Route ? *Route : -1; Buffer.Add(Event); Stopped = true;
    }
private:
    FTimingBuffer& Buffer;
    TWeakObjectPtr<UWorld> ObservedWorld;
    const double* Route;
    FTimingEvent Event;
    bool Stopped = false;
};
}

class FTaskRouteProbeModule final : public IModuleInterface
{
public:
    virtual void StartupModule() override
    {
        if (!FParse::Value(FCommandLine::Get(), TEXT("TaskRoute="), RoutePath)) return;
        StartWall = FPlatformTime::Seconds();
        Timing.Events.Reserve(32768);
        FParse::Value(FCommandLine::Get(), TEXT("TaskRouteOutput="), OutputDir);
        if (OutputDir.IsEmpty()) OutputDir = FPaths::ProjectSavedDir() / TEXT("TaskRouteProbe");
        FParse::Value(FCommandLine::Get(), TEXT("TaskRouteSeconds="), Duration);
        Duration = FMath::Clamp(Duration, 1.f, 120.f);
        IFileManager::Get().MakeDirectory(*OutputDir, true);
        if (FPaths::FileExists(OutputDir / TEXT("summary.json")) || FPaths::FileExists(OutputDir / TEXT("telemetry.jsonl")))
        {
            UE_LOG(LogTemp, Error, TEXT("TaskRouteProbe refuses to overwrite existing evidence."));
            FPlatformMisc::RequestExitWithStatus(false, 3);
            return;
        }
        FString Text;
        TArray<TSharedPtr<FJsonValue>> Array;
        if (!FFileHelper::LoadFileToString(Text, *RoutePath) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Array) || Array.Num() > 400)
        { Finish(TEXT("Route JSON missing, invalid, or over 400 events.")); return; }
        double Previous = -1;
        TSet<FKey> Held;
        for (const auto& Item : Array)
        {
            const auto O = Item->AsObject();
            double Time = 0; FString Key; bool Press = false;
            if (!O.IsValid() || !O->TryGetNumberField(TEXT("t"), Time) || !O->TryGetStringField(TEXT("key"), Key) || !O->TryGetBoolField(TEXT("press"), Press))
            { Finish(TEXT("Route event must contain t, key and press.")); return; }
            const FKey K{FName(*Key)};
            if (!K.IsValid() || K.IsGamepadKey() || K.IsAxis1D() || K.IsAxis2D() || K.IsAxis3D() || Time < Previous || Time < 0 || Time > 118 || Time > Duration)
            { Finish(TEXT("Route contains unsupported key or event time.")); return; }
            if ((Press && Held.Contains(K)) || (!Press && !Held.Contains(K)))
            { Finish(TEXT("Route key transitions are unbalanced.")); return; }
            if (Press) Held.Add(K); else Held.Remove(K);
            Events.Add({ Time, K, Press });
            Previous = Time;
        }
        if (Held.Num()) { Finish(TEXT("Route ends with held keys.")); return; }
        PreTickHandle = FWorldDelegates::OnWorldPreActorTick.AddRaw(this, &FTaskRouteProbeModule::BeforeTick);
        PostTickHandle = FWorldDelegates::OnWorldPostActorTick.AddRaw(this, &FTaskRouteProbeModule::AfterTick);
        RenderHandle = UGameViewportClient::OnViewportRendered().AddRaw(this, &FTaskRouteProbeModule::AfterRender);
        WatchHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FTaskRouteProbeModule::Watchdog));
        UE_LOG(LogTemp, Display, TEXT("TaskRouteProbe armed with %d keyboard events for %.2f seconds; no media output."), Events.Num(), Duration);
    }

    virtual void ShutdownModule() override
    {
        if (PreTickHandle.IsValid()) FWorldDelegates::OnWorldPreActorTick.Remove(PreTickHandle);
        if (PostTickHandle.IsValid()) FWorldDelegates::OnWorldPostActorTick.Remove(PostTickHandle);
        if (RenderHandle.IsValid()) UGameViewportClient::OnViewportRendered().Remove(RenderHandle);
        if (WatchHandle.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(WatchHandle);
        FlushDiagnosticTiming();
    }

private:
    FString RoutePath, OutputDir;
    TArray<TaskRoute::FEvent> Events;
    TSet<FKey> HeldKeys;
    TWeakObjectPtr<UWorld> World;
    TWeakObjectPtr<APlayerController> Controller;
    FDelegateHandle PreTickHandle, PostTickHandle, RenderHandle;
    FTSTicker::FDelegateHandle WatchHandle;
    double StartWall = 0, ClockZero = 0, LastSample = -1, LastScene = -1, LastPixels = -1, Elapsed = 0;
    float Duration = 35;
    int32 NextEvent = 0, Samples = 0, PixelSamples = 0, RenderFrames = 0;
    bool Started = false, Finished = false, PixelRequested = false;
    bool SeenFlying = false, SeenDefeat = false;
    TaskRoute::FTimingBuffer Timing;

    void FlushDiagnosticTiming()
    {
        if (OutputDir.IsEmpty() || Timing.Events.IsEmpty()) return;
        FString Text;
        FScopeLock Lock(&Timing.Mutex);
        for (const auto& E : Timing.Events)
        {
            auto O = TaskRoute::Object(); O->SetStringField(TEXT("schema"), TEXT("route-observer-timing-diagnostic/1"));
            O->SetStringField(TEXT("stage"), E.Stage); O->SetNumberField(TEXT("wall_begin_seconds"), E.Begin);
            O->SetNumberField(TEXT("wall_end_seconds"), E.End); O->SetNumberField(TEXT("duration_seconds"), E.End - E.Begin);
            O->SetNumberField(TEXT("module_start_wall_seconds"), StartWall);
            O->SetNumberField(TEXT("world_begin_seconds"), E.WorldBegin); O->SetNumberField(TEXT("world_end_seconds"), E.WorldEnd);
            O->SetNumberField(TEXT("route_begin_seconds"), E.RouteBegin); O->SetNumberField(TEXT("route_end_seconds"), E.RouteEnd);
            O->SetNumberField(TEXT("thread_begin"), E.ThreadBegin); O->SetNumberField(TEXT("thread_end"), E.ThreadEnd);
            O->SetNumberField(TEXT("frame_begin"), double(E.FrameBegin)); O->SetNumberField(TEXT("frame_end"), double(E.FrameEnd));
            O->SetBoolField(TEXT("diagnostic_only"), true); O->SetBoolField(TEXT("formal_acceptance"), false);
            Text += TaskRoute::Serialize(O);
        }
        if (FPaths::FileExists(OutputDir / TEXT("timing.jsonl")) || !FFileHelper::SaveStringToFile(Text, *(OutputDir / TEXT("timing.jsonl")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
            UE_LOG(LogTemp, Error, TEXT("Diagnostic timing stream could not be saved once without overwrite."));
    }

    void Write(const TCHAR* File, const TaskRoute::FObject& O)
    {
        TaskRoute::FTimingSpan TimingWrite(Timing, File, World.Get(), &Elapsed);
        if (!FFileHelper::SaveStringToFile(TaskRoute::Serialize(O), *(OutputDir / File), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
            &IFileManager::Get(), FILEWRITE_Append)) UE_LOG(LogTemp, Error, TEXT("TaskRouteProbe could not append evidence."));
    }

    bool Watchdog(float)
    {
        if (!Finished && FPlatformTime::Seconds() - StartWall > 480.) Finish(TEXT("Wall-clock watchdog expired."));
        return !Finished;
    }

    void BeforeTick(UWorld* W, ELevelTick, float)
    {
        if (Finished || !W || !W->IsGameWorld() || !W->HasBegunPlay()) return;
        TaskRoute::FTimingSpan TimingBefore(Timing, TEXT("before_tick"), W, &Elapsed);
        if (!Started)
        {
            APlayerController* PC = W->GetFirstPlayerController();
            if (!PC || !PC->PlayerInput || !PC->GetPawn()) return;
            bool SurfaceRead = false;
            { TaskRoute::FTimingSpan TimingSurface(Timing, TEXT("initial_surface"), W, &Elapsed); SurfaceRead = WriteNativeSurfaceState(W, OutputDir); }
            if (!SurfaceRead) { Finish(TEXT("Native surface readback failed.")); return; }
            World = W; Controller = PC; ClockZero = W->GetTimeSeconds(); Started = true;
            auto O = TaskRoute::Object(); O->SetStringField(TEXT("event"), TEXT("route_clock_started"));
            O->SetNumberField(TEXT("world_seconds_offset"), ClockZero); O->SetNumberField(TEXT("wall_seconds_since_module_start"), FPlatformTime::Seconds() - StartWall);
            O->SetStringField(TEXT("map"), W->GetMapName()); O->SetStringField(TEXT("clock"), TEXT("world seconds from first begun-play tick with controller, PlayerInput and pawn; no warm-up freeze"));
            O->SetStringField(TEXT("input"), TEXT("APlayerController::InputKey / FInputKeyEventArgs::CreateSimulated"));
            Write(TEXT("events.jsonl"), O);
        }
        if (W != World.Get() || !Controller.IsValid()) return;
        Elapsed = W->GetTimeSeconds() - ClockZero;
        while (NextEvent < Events.Num() && Events[NextEvent].Time <= Elapsed)
        {
            const auto& E = Events[NextEvent++];
            TaskRoute::FTimingSpan TimingInput(Timing, TEXT("input_key"), W, &Elapsed);
            const bool Accepted = Controller->InputKey(FInputKeyEventArgs::CreateSimulated(E.Key, E.Press ? IE_Pressed : IE_Released, E.Press ? 1.f : 0.f));
            TimingInput.Stop();
            if (E.Press) HeldKeys.Add(E.Key); else HeldKeys.Remove(E.Key);
            auto O = TaskRoute::Object(); O->SetStringField(TEXT("event"), TEXT("key")); O->SetStringField(TEXT("key"), E.Key.ToString());
            O->SetBoolField(TEXT("press"), E.Press); O->SetNumberField(TEXT("scheduled_t"), E.Time); O->SetNumberField(TEXT("actual_t"), Elapsed);
            O->SetBoolField(TEXT("inputkey_return_not_success_verdict"), Accepted); Write(TEXT("events.jsonl"), O);
        }
    }

    TaskRoute::FObject ProjectBounds(AActor* A, const FVector& Center, const FVector& Extent)
    {
        auto O = TaskRoute::Object(); O->SetArrayField(TEXT("center"), TaskRoute::Vector(Center)); O->SetArrayField(TEXT("extent"), TaskRoute::Vector(Extent));
        FVector2D Min(1.e12, 1.e12), Max(-1.e12, -1.e12); int32 Front = 0;
        for (int32 X : {-1, 1}) for (int32 Y : {-1, 1}) for (int32 Z : {-1, 1})
        {
            FVector2D P;
            if (Controller->ProjectWorldLocationToScreen(Center + Extent * FVector(X, Y, Z), P))
            { ++Front; Min.X = FMath::Min(Min.X, P.X); Min.Y = FMath::Min(Min.Y, P.Y); Max.X = FMath::Max(Max.X, P.X); Max.Y = FMath::Max(Max.Y, P.Y); }
        }
        int32 SX = 0, SY = 0; Controller->GetViewportSize(SX, SY);
        const bool InFrame = Front && Max.X >= 0 && Max.Y >= 0 && Min.X < SX && Min.Y < SY;
        O->SetBoolField(TEXT("projected_intersects_viewport"), InFrame); O->SetNumberField(TEXT("front_corners"), Front);
        if (Front) O->SetArrayField(TEXT("screen_box"), { MakeShared<FJsonValueNumber>(Min.X), MakeShared<FJsonValueNumber>(Min.Y), MakeShared<FJsonValueNumber>(Max.X), MakeShared<FJsonValueNumber>(Max.Y) });
        FVector Cam; FRotator Rot; Controller->GetPlayerViewPoint(Cam, Rot);
        FHitResult Hit; FCollisionQueryParams Query(SCENE_QUERY_STAT(TaskRouteObservation), true);
        if (Controller->GetPawn()) Query.AddIgnoredActor(Controller->GetPawn());
        const bool Blocked = World->LineTraceSingleByChannel(Hit, Cam, Center, ECC_Visibility, Query);
        O->SetBoolField(TEXT("center_visibility_trace_clear_or_self"), !Blocked || Hit.GetActor() == A);
        O->SetStringField(TEXT("projection_scope"), TEXT("geometric candidate only; center trace is not full raster visibility"));
        return O;
    }

    TaskRoute::FObject ActorState(AActor* A, bool Components)
    {
        auto O = TaskRoute::Object(); O->SetStringField(TEXT("name"), A->GetName()); O->SetStringField(TEXT("class"), A->GetClass()->GetName());
        O->SetStringField(TEXT("owner"), A->GetOwner() ? A->GetOwner()->GetName() : TEXT(""));
        TArray<TSharedPtr<FJsonValue>> Tags; for (const FName& Tag : A->Tags) Tags.Add(MakeShared<FJsonValueString>(Tag.ToString())); O->SetArrayField(TEXT("tags"), Tags);
#if WITH_EDITOR
        O->SetStringField(TEXT("label"), A->GetActorLabel(false));
#endif
        O->SetArrayField(TEXT("position"), TaskRoute::Vector(A->GetActorLocation())); O->SetArrayField(TEXT("velocity"), TaskRoute::Vector(A->GetVelocity()));
        const FRotator R = A->GetActorRotation(); O->SetArrayField(TEXT("rotation_pitch_yaw_roll"), TaskRoute::Vector(FVector(R.Pitch, R.Yaw, R.Roll)));
        FVector Center, Extent; A->GetActorBounds(false, Center, Extent); O->SetObjectField(TEXT("bounds"), ProjectBounds(A, Center, Extent));
        O->SetBoolField(TEXT("hidden"), A->IsHidden());
        for (const TCHAR* Name : {TEXT("Health"), TEXT("Speed"), TEXT("CannonAmmo"), TEXT("MissileAmmo"), TEXT("LockProgress"), TEXT("bStalling"), TEXT("bHighG"), TEXT("bMissileWarning"), TEXT("bMissile"), TEXT("bPlayerOwned"), TEXT("FiredBy"), TEXT("HomingTarget"), TEXT("Target"), TEXT("MaxMissileTravelDistanceCm")}) TaskRoute::Reflect(A, Name, O);
        if (Components)
        {
            TInlineComponentArray<UPrimitiveComponent*> Primitives(A); TArray<TSharedPtr<FJsonValue>> Items;
            for (UPrimitiveComponent* P : Primitives)
            {
                if (Items.Num() >= 100) break;
                auto C = TaskRoute::Object(); C->SetStringField(TEXT("name"), P->GetName()); C->SetStringField(TEXT("class"), P->GetClass()->GetName());
                C->SetBoolField(TEXT("visible"), P->IsVisible()); C->SetBoolField(TEXT("hidden_in_game"), P->bHiddenInGame); C->SetBoolField(TEXT("registered"), P->IsRegistered());
                C->SetArrayField(TEXT("scale"), TaskRoute::Vector(P->GetComponentScale())); C->SetArrayField(TEXT("position"), TaskRoute::Vector(P->GetComponentLocation()));
                C->SetObjectField(TEXT("bounds"), ProjectBounds(A, P->Bounds.Origin, P->Bounds.BoxExtent));
                if (P->GetName().Contains(TEXT("Gear")))
                {
                    const FVector Bottom = P->Bounds.Origin - FVector(0, 0, P->Bounds.BoxExtent.Z);
                    FHitResult Ground; FCollisionQueryParams Query(SCENE_QUERY_STAT(TaskRouteGearGround), true); Query.AddIgnoredActor(A);
                    const bool Hit = World->LineTraceSingleByChannel(Ground, Bottom + FVector(0,0,1000), Bottom - FVector(0,0,20000), ECC_Visibility, Query);
                    C->SetBoolField(TEXT("gear_ground_trace_hit"), Hit);
                    if (Hit) { C->SetNumberField(TEXT("gear_bbox_bottom_minus_ground_cm"), Bottom.Z - Ground.ImpactPoint.Z); C->SetArrayField(TEXT("ground_hit"), TaskRoute::Vector(Ground.ImpactPoint)); }
                }
                if (const UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(P); Mesh && Mesh->GetStaticMesh()) C->SetStringField(TEXT("mesh"), Mesh->GetStaticMesh()->GetPathName());
                TArray<TSharedPtr<FJsonValue>> Materials;
                for (int32 I = 0; I < P->GetNumMaterials(); ++I)
                {
                    UMaterialInterface* Material = P->GetMaterial(I); if (!Material) continue;
                    auto M = TaskRoute::Object(); M->SetStringField(TEXT("path"), Material->GetPathName()); M->SetNumberField(TEXT("slot"), I);
                    if (UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Material))
                    {
                        for (const TCHAR* Param : {TEXT("Strength"), TEXT("Glow"), TEXT("Opacity"), TEXT("Phase"), TEXT("Speed"), TEXT("RippleSpeed"), TEXT("Depth")})
                            M->SetNumberField(Param, MID->K2_GetScalarParameterValue(Param));
                    }
                    Materials.Add(TaskRoute::Value(M));
                }
                C->SetArrayField(TEXT("materials"), Materials); Items.Add(TaskRoute::Value(C));
            }
            O->SetArrayField(TEXT("primitives"), Items);
        }
        return O;
    }

    void AfterTick(UWorld* W, ELevelTick, float)
    {
        if (!Started || Finished || W != World.Get() || !Controller.IsValid()) return;
        TaskRoute::FTimingSpan TimingAfter(Timing, TEXT("after_tick"), W, &Elapsed);
        Elapsed = W->GetTimeSeconds() - ClockZero;
        if (Elapsed - LastSample >= .2)
        {
            TaskRoute::FTimingSpan TimingTelemetry(Timing, TEXT("telemetry_observation"), W, &Elapsed);
            LastSample = Elapsed; ++Samples; auto O = TaskRoute::Object(); O->SetNumberField(TEXT("t"), Elapsed); O->SetNumberField(TEXT("world_seconds"), W->GetTimeSeconds());
            auto Mode = TaskRoute::Object(); AGameModeBase* GM = W->GetAuthGameMode();
            for (const TCHAR* Name : { TEXT("Phase"), TEXT("RadioLine"), TEXT("DefeatReason"), TEXT("AirportHealth"), TEXT("Elapsed"), TEXT("Score"), TEXT("Wave"), TEXT("SelectedAircraft"), TEXT("bRearming") }) TaskRoute::Reflect(GM, Name, Mode);
            FString Phase; if (Mode->TryGetStringField(TEXT("Phase_name"), Phase)) { SeenFlying |= Phase == TEXT("Flying"); SeenDefeat |= Phase == TEXT("Defeat"); }
            O->SetObjectField(TEXT("mode"), Mode);
            if (APawn* Pawn = Controller->GetPawn()) O->SetObjectField(TEXT("pawn"), ActorState(Pawn, true));
            FVector Cam; FRotator Rot; Controller->GetPlayerViewPoint(Cam, Rot); auto Camera = TaskRoute::Object(); Camera->SetArrayField(TEXT("position"), TaskRoute::Vector(Cam));
            Camera->SetArrayField(TEXT("rotation_pitch_yaw_roll"), TaskRoute::Vector(FVector(Rot.Pitch, Rot.Yaw, Rot.Roll)));
            if (Controller->PlayerCameraManager) Camera->SetNumberField(TEXT("fov"), Controller->PlayerCameraManager->GetFOVAngle()); O->SetObjectField(TEXT("camera"), Camera);
            TArray<TSharedPtr<FJsonValue>> Projectiles;
            for (TActorIterator<AActor> It(W); It; ++It) if (It->GetClass()->GetName().Contains(TEXT("Projectile")) && Projectiles.Num() < 40) Projectiles.Add(TaskRoute::Value(ActorState(*It, true)));
            O->SetArrayField(TEXT("projectiles"), Projectiles); Write(TEXT("telemetry.jsonl"), O);
        }
        if (Elapsed - LastScene >= 1.)
        {
            TaskRoute::FTimingSpan TimingScene(Timing, TEXT("scene_observation"), W, &Elapsed);
            LastScene = Elapsed; auto O = TaskRoute::Object(); O->SetNumberField(TEXT("t"), Elapsed); TArray<TSharedPtr<FJsonValue>> Actors;
            int32 StaticCount = 0, InFrameCount = 0;
            TMap<FString,int32> ProjectedTagCounts, UnoccludedCenterTagCounts;
            for (TActorIterator<AActor> It(W); It; ++It)
            {
                AActor* A = *It; const UStaticMeshComponent* Mesh = A->FindComponentByClass<UStaticMeshComponent>();
                if (!Mesh || Mesh->Mobility != EComponentMobility::Static) continue;
                ++StaticCount; auto Bounds = ProjectBounds(A, Mesh->Bounds.Origin, Mesh->Bounds.BoxExtent);
                if (!Bounds->GetBoolField(TEXT("projected_intersects_viewport"))) continue;
                ++InFrameCount; FString Name = A->GetName();
                for (const FName& Tag : A->Tags)
                {
                    ++ProjectedTagCounts.FindOrAdd(Tag.ToString());
                    if (Bounds->GetBoolField(TEXT("center_visibility_trace_clear_or_self"))) ++UnoccludedCenterTagCounts.FindOrAdd(Tag.ToString());
                }
#if WITH_EDITOR
                Name += TEXT(" ") + A->GetActorLabel(false);
#endif
                const FString MeshPath = Mesh->GetStaticMesh() ? Mesh->GetStaticMesh()->GetPathName() : TEXT("");
                FString TagText; for (const FName& Tag : A->Tags) TagText += TEXT(" ") + Tag.ToString();
                const FString Search = (Name + TEXT(" ") + MeshPath + TagText).ToLower();
                bool Relevant = false;
                for (const TCHAR* Word : {TEXT("town"),TEXT("house"),TEXT("water"),TEXT("lake"),TEXT("oasis"),TEXT("bank"),TEXT("terrain"),TEXT("rugged"),TEXT("hangar"),TEXT("airport"),TEXT("ground"),TEXT("mountain"),TEXT("palm")}) Relevant |= Search.Contains(Word);
                if (!Relevant || Actors.Num() >= 1000) continue;
                auto Item = TaskRoute::Object(); Item->SetStringField(TEXT("name"), Name); Item->SetStringField(TEXT("mesh"), MeshPath); Item->SetStringField(TEXT("tags"), TagText); Item->SetObjectField(TEXT("bounds"), Bounds); Actors.Add(TaskRoute::Value(Item));
            }
            auto ProjectedCounts = TaskRoute::Object(), CenterCounts = TaskRoute::Object();
            for (const auto& Pair : ProjectedTagCounts) ProjectedCounts->SetNumberField(Pair.Key, Pair.Value);
            for (const auto& Pair : UnoccludedCenterTagCounts) CenterCounts->SetNumberField(Pair.Key, Pair.Value);
            O->SetObjectField(TEXT("projected_actor_count_by_tag"), ProjectedCounts); O->SetObjectField(TEXT("center_clear_actor_count_by_tag"), CenterCounts);
            O->SetNumberField(TEXT("static_mesh_actor_count"), StaticCount); O->SetNumberField(TEXT("projected_static_mesh_actor_count"), InFrameCount); O->SetArrayField(TEXT("relevant_projected_actors"), Actors); Write(TEXT("scene.jsonl"), O);
        }
        if (Elapsed - LastPixels >= .5) { LastPixels = Elapsed; PixelRequested = true; }
        if (Elapsed >= Duration) Finish(TEXT(""));
    }

    void AfterRender(FViewport* Viewport)
    {
        if (Finished || !Started || !GEngine || !GEngine->GameViewport || Viewport != GEngine->GameViewport->Viewport) return;
        TaskRoute::FTimingSpan TimingRender(Timing, TEXT("after_render"), World.Get(), &Elapsed);
        ++RenderFrames; if (!PixelRequested) return; PixelRequested = false;
        const FIntPoint Size = Viewport->GetSizeXY(); TArray<FColor> Pixels;
        if (Size.X <= 0 || Size.Y <= 0) return;
        bool PixelsRead = false;
        { TaskRoute::FTimingSpan TimingRead(Timing, TEXT("read_pixels"), World.Get(), &Elapsed); PixelsRead = Viewport->ReadPixels(Pixels); }
        if (!PixelsRead || Pixels.Num() != Size.X * Size.Y) return;
        TaskRoute::FTimingSpan TimingStatistics(Timing, TEXT("pixel_statistics_and_write"), World.Get(), &Elapsed);
        ++PixelSamples; auto O = TaskRoute::Object(); O->SetNumberField(TEXT("t"), Elapsed); O->SetNumberField(TEXT("width"), Size.X); O->SetNumberField(TEXT("height"), Size.Y);
        struct FRegion { const TCHAR* Name; float X0, Y0, X1, Y1; };
        TArray<TSharedPtr<FJsonValue>> Regions;
        for (const FRegion& R : { FRegion{TEXT("full"),0,0,1,1}, FRegion{TEXT("aircraft_tail_lower_center"),.32f,.5f,.68f,.9f}, FRegion{TEXT("forward_muzzle_center"),.35f,.3f,.65f,.65f}, FRegion{TEXT("terrain_water_lower_half"),.08f,.48f,.92f,.9f}, FRegion{TEXT("radio_upper_middle"),.28f,.18f,.75f,.4f} })
        {
            int32 Count = 0, Bright = 0, NonBlack = 0, Peak = 0; double Sum = 0, SumSquared = 0;
            for (int32 Y = int32(Size.Y * R.Y0); Y < int32(Size.Y * R.Y1); Y += 2)
                for (int32 X = int32(Size.X * R.X0); X < int32(Size.X * R.X1); X += 2)
                {
                    const FColor C = Pixels[Y * Size.X + X]; const double L = .2126 * C.R + .7152 * C.G + .0722 * C.B;
                    ++Count; Sum += L; SumSquared += L * L; Bright += L >= 220.; NonBlack += L >= 8.; Peak = FMath::Max(Peak, int32(FMath::Max3(C.R,C.G,C.B)));
                }
            auto M = TaskRoute::Object(); M->SetStringField(TEXT("region"), R.Name); M->SetNumberField(TEXT("sampled_pixels"), Count); M->SetNumberField(TEXT("luminance_mean"), Count ? Sum / Count : 0);
            M->SetNumberField(TEXT("luminance_variance"), Count ? SumSquared / Count - FMath::Square(Sum / Count) : 0); M->SetNumberField(TEXT("bright_pixels_ge220"), Bright); M->SetNumberField(TEXT("nonblack_pixels_ge8"), NonBlack); M->SetNumberField(TEXT("peak_channel"), Peak); Regions.Add(TaskRoute::Value(M));
        }
        O->SetStringField(TEXT("scope"), TEXT("nonblank/range observation; regional brightness alone does not isolate effects or prove artistic quality")); O->SetArrayField(TEXT("regions"), Regions); Write(TEXT("render.jsonl"), O);
    }

    void Finish(const FString& Error)
    {
        if (Finished) return; Finished = true;
        if (Controller.IsValid()) for (const FKey& Key : HeldKeys) Controller->InputKey(FInputKeyEventArgs::CreateSimulated(Key, IE_Released, 0.f));
        auto O = TaskRoute::Object(); O->SetStringField(TEXT("schema"), TEXT("keyboard-route-probe/2")); O->SetStringField(TEXT("status"), Error.IsEmpty() ? TEXT("completed") : TEXT("failed"));
        O->SetStringField(TEXT("error"), Error); O->SetStringField(TEXT("route_file"), FPaths::GetCleanFilename(RoutePath)); O->SetNumberField(TEXT("duration_requested"), Duration); O->SetNumberField(TEXT("duration_observed"), Elapsed);
        O->SetNumberField(TEXT("world_clock_offset"), ClockZero); O->SetNumberField(TEXT("route_events"), Events.Num()); O->SetNumberField(TEXT("events_dispatched"), NextEvent); O->SetNumberField(TEXT("telemetry_samples"), Samples);
        O->SetNumberField(TEXT("render_frames"), RenderFrames); O->SetNumberField(TEXT("numeric_pixel_samples"), PixelSamples); O->SetNumberField(TEXT("media_files_written"), 0);
        O->SetBoolField(TEXT("seen_flying"), SeenFlying); O->SetBoolField(TEXT("seen_defeat"), SeenDefeat); O->SetBoolField(TEXT("route_completed"), Started && NextEvent == Events.Num() && Elapsed >= Duration);
        O->SetStringField(TEXT("scope"), TEXT("keyboard input with read-only runtime, geometric and numeric framebuffer observations; no teleport, actor-state override, autoplay, image writer or artistic verdict"));
        Write(TEXT("summary.json"), O);
        UE_LOG(LogTemp, Display, TEXT("TaskRouteProbe finished: %s events %d/%d, samples %d, render samples %d."), *Error, NextEvent, Events.Num(), Samples, PixelSamples);
        FPlatformMisc::RequestExitWithStatus(false, Error.IsEmpty() ? 0 : 2);
    }
};

IMPLEMENT_MODULE(FTaskRouteProbeModule, TaskRouteProbe)
