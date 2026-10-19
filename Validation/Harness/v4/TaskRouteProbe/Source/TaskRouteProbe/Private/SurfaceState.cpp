#include "SurfaceState.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInstance.h"
#include "MaterialExpressionIO.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshResources.h"
#include "UObject/UnrealType.h"

namespace SurfaceReadback
{
using J = TSharedPtr<FJsonObject>;
using A = TArray<TSharedPtr<FJsonValue>>;
static J Object() { return MakeShared<FJsonObject>(); }
static TSharedPtr<FJsonValue> Value(const J& O) { return MakeShared<FJsonValueObject>(O); }
static A Vec(const FVector& V) { return {MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z)}; }
static J Box(const FVector& Center, const FVector& Extent) { auto O=Object(); O->SetArrayField(TEXT("center_cm"),Vec(Center)); O->SetArrayField(TEXT("extent_cm"),Vec(Extent)); return O; }
static J Transform(const FTransform& T)
{
    auto O=Object(); O->SetArrayField(TEXT("translation_cm"),Vec(T.GetLocation())); O->SetArrayField(TEXT("scale"),Vec(T.GetScale3D()));
    const FQuat Q=T.GetRotation(); O->SetArrayField(TEXT("rotation_xyzw"),{MakeShared<FJsonValueNumber>(Q.X),MakeShared<FJsonValueNumber>(Q.Y),MakeShared<FJsonValueNumber>(Q.Z),MakeShared<FJsonValueNumber>(Q.W)}); return O;
}
static bool Environment(const AActor* Actor) { for(const FName& Tag:Actor->Tags) if(Tag.ToString().StartsWith(TEXT("DSEnvironment"))) return true; return false; }
static bool Terrain(const AActor* Actor) { return Actor && Actor->ActorHasTag(TEXT("DSEnvironmentTerrain")); }
static J Ground(UWorld* World, const FVector& Point, const FString& Subject, const FCollisionQueryParams& Query)
{
    auto O=Object(); O->SetStringField(TEXT("subject"),Subject); O->SetArrayField(TEXT("point_cm"),Vec(Point));
    TArray<FHitResult> Hits; World->LineTraceMultiByObjectType(Hits,FVector(Point.X,Point.Y,1000000),FVector(Point.X,Point.Y,-1000000),FCollisionObjectQueryParams(ECC_WorldStatic),Query);
    const FHitResult* Best=nullptr; for(const auto& Hit:Hits) if(Terrain(Hit.GetActor()) && (!Best || Hit.ImpactPoint.Z>Best->ImpactPoint.Z)) Best=&Hit;
    O->SetBoolField(TEXT("terrain_hit"),Best!=nullptr);
    if(Best) { O->SetArrayField(TEXT("terrain_point_cm"),Vec(Best->ImpactPoint)); O->SetNumberField(TEXT("point_minus_terrain_cm"),Point.Z-Best->ImpactPoint.Z); O->SetStringField(TEXT("terrain_actor"),Best->GetActor()->GetName()); }
    return O;
}
static J Mesh(UStaticMesh* Mesh)
{
    auto O=Object(); O->SetStringField(TEXT("asset_path"),Mesh->GetPathName());
    const auto B=Mesh->GetBounds(); O->SetObjectField(TEXT("local_bounds"),Box(B.Origin,B.BoxExtent));
    const FStaticMeshRenderData* Data=Mesh->GetRenderData();
    if(!Data || Data->LODResources.Num()==0) { O->SetStringField(TEXT("status"),TEXT("missing_render_data")); return O; }
    const FStaticMeshLODResources& L=Data->LODResources[0]; const auto& P=L.VertexBuffers.PositionVertexBuffer; const auto& V=L.VertexBuffers.StaticMeshVertexBuffer;
    O->SetNumberField(TEXT("vertices"),P.GetNumVertices()); O->SetNumberField(TEXT("indices"),L.IndexBuffer.GetNumIndices()); O->SetNumberField(TEXT("triangles"),L.GetNumTriangles()); O->SetNumberField(TEXT("uv_channels"),V.GetNumTexCoords());
    FSHA1 H; FBox Local(ForceInit);
    // Hash semantic numeric values, not buffer padding, pointers or package GUIDs.
    auto Number=[&H](float N){ H.Update(reinterpret_cast<const uint8*>(&N),sizeof(N)); };
    for(uint32 I=0;I<P.GetNumVertices();++I)
    {
        const FVector3f Pos=P.VertexPosition(I); Local+=FVector(Pos); Number(Pos.X);Number(Pos.Y);Number(Pos.Z);
        const FVector3f N=V.VertexTangentZ(I); Number(N.X);Number(N.Y);Number(N.Z);
        for(uint32 U=0;U<V.GetNumTexCoords();++U) { const FVector2f UV=V.GetVertexUV(I,U);Number(UV.X);Number(UV.Y); }
    }
    for(int32 I=0;I<L.IndexBuffer.GetNumIndices();++I) { const uint32 Index=L.IndexBuffer.GetIndex(I); H.Update(reinterpret_cast<const uint8*>(&Index),sizeof(Index)); }
    H.Final(); uint8 Hash[20]; H.GetHash(Hash); O->SetStringField(TEXT("geometry_sha1"),BytesToHex(Hash,20).ToLower());
    O->SetStringField(TEXT("geometry_hash_encoding"),TEXT("LOD0 vertices: little-endian float32 XYZ, normal XYZ, each UV XY; then uint32 triangle indices"));
    O->SetObjectField(TEXT("render_vertex_bounds"),Box(Local.GetCenter(),Local.GetExtent())); O->SetStringField(TEXT("status"),TEXT("observed")); return O;
}
static void Node(UMaterialExpression* E, TSet<const UObject*>& Seen, A& Nodes)
{
#if WITH_EDITOR
    if(!E || Seen.Contains(E)) return; Seen.Add(E); auto O=Object(); O->SetStringField(TEXT("path"),E->GetPathName()); O->SetStringField(TEXT("class"),E->GetClass()->GetName());
    auto Properties=Object();
    for(TFieldIterator<FProperty> It(E->GetClass());It;++It)
    {
        FProperty* P=*It; if(P->GetOwnerClass()==UObject::StaticClass() || P->GetName().Contains(TEXT("Guid")) || P->GetName().Contains(TEXT("Editor"))) continue;
        if(CastField<FNumericProperty>(P) || CastField<FBoolProperty>(P) || CastField<FNameProperty>(P) || CastField<FStrProperty>(P) || CastField<FObjectPropertyBase>(P))
        { FString Text; P->ExportText_InContainer(0,Text,E,E,E,PPF_None); if(Text.Len()<20000) Properties->SetStringField(P->GetName(),Text); }
    }
    O->SetObjectField(TEXT("properties"),Properties); A Inputs;
    for(int32 I=0;;++I)
    {
        const FExpressionInput* Input=E->GetInput(I); if(!Input) break;
        auto Edge=Object(); Edge->SetNumberField(TEXT("index"),I); Edge->SetStringField(TEXT("name"),E->GetInputName(I).ToString());
        Edge->SetStringField(TEXT("node"),Input->Expression?Input->Expression->GetPathName():TEXT("")); Edge->SetNumberField(TEXT("output_index"),Input->OutputIndex); Inputs.Add(Value(Edge));
        if(Input->Expression) Node(Input->Expression,Seen,Nodes);
    }
    O->SetArrayField(TEXT("inputs"),Inputs);
    if(auto Call=Cast<UMaterialExpressionMaterialFunctionCall>(E);Call && Call->MaterialFunction)
    {
        O->SetStringField(TEXT("function"),Call->MaterialFunction->GetPathName());
        for(UMaterialExpression* Child:Call->MaterialFunction->GetExpressions()) Node(Child,Seen,Nodes);
    }
    Nodes.Add(Value(O));
#endif
}
static J Material(UMaterialInterface* M)
{
    auto O=Object(); O->SetStringField(TEXT("asset_path"),M->GetPathName());
    if(auto Instance=Cast<UMaterialInstance>(M)) O->SetStringField(TEXT("parent"),Instance->Parent?Instance->Parent->GetPathName():TEXT(""));
    auto Scalars=Object(),Vectors=Object(),Textures=Object(); TArray<FMaterialParameterInfo> Names; TArray<FGuid> Ids;
    M->GetAllScalarParameterInfo(Names,Ids); for(const auto& N:Names) { float V=0; if(M->GetScalarParameterValue(N,V)) Scalars->SetNumberField(N.Name.ToString(),V); }
    Names.Reset();Ids.Reset();M->GetAllVectorParameterInfo(Names,Ids); for(const auto& N:Names) { FLinearColor V; if(M->GetVectorParameterValue(N,V)) Vectors->SetArrayField(N.Name.ToString(),{MakeShared<FJsonValueNumber>(V.R),MakeShared<FJsonValueNumber>(V.G),MakeShared<FJsonValueNumber>(V.B),MakeShared<FJsonValueNumber>(V.A)}); }
    Names.Reset();Ids.Reset();M->GetAllTextureParameterInfo(Names,Ids); for(const auto& N:Names) { UTexture* V=nullptr; if(M->GetTextureParameterValue(N,V)) Textures->SetStringField(N.Name.ToString(),V?V->GetPathName():TEXT("")); }
    O->SetObjectField(TEXT("resolved_scalars"),Scalars);O->SetObjectField(TEXT("resolved_vectors"),Vectors);O->SetObjectField(TEXT("resolved_textures"),Textures);
#if WITH_EDITOR
    if(UMaterial* Base=M->GetMaterial())
    {
        O->SetStringField(TEXT("base_material"),Base->GetPathName()); auto Roots=Object(); A Nodes; TSet<const UObject*> Seen;
        for(int32 I=0;I<MP_MAX;++I) if(FExpressionInput* Input=Base->GetExpressionInputForProperty(static_cast<EMaterialProperty>(I));Input && Input->Expression)
        { Roots->SetStringField(FString::FromInt(I),Input->Expression->GetPathName()); Node(Input->Expression,Seen,Nodes); }
        O->SetObjectField(TEXT("graph_property_roots"),Roots);O->SetArrayField(TEXT("graph_nodes"),Nodes);
    }
#endif
    return O;
}
}

bool WriteNativeSurfaceState(UWorld* World, const FString& OutputDirectory)
{
    using namespace SurfaceReadback;
    const FString Filename=OutputDirectory/TEXT("surface-state.json"); if(FPaths::FileExists(Filename)) return false;
    auto Report=Object(); Report->SetStringField(TEXT("schema"),TEXT("native-surface-observation/1")); Report->SetStringField(TEXT("engine"),FEngineVersion::Current().ToString());
    Report->SetNumberField(TEXT("media_files_written"),0); Report->SetBoolField(TEXT("actor_or_material_mutations"),false); Report->SetNumberField(TEXT("package_save_calls"),0);
    Report->SetStringField(TEXT("scope"),TEXT("Read-only current native render geometry, material graph/parameters and terrain collision before ordinary keyboard dispatch; no image-quality verdict."));
    TSet<UStaticMesh*> Meshes; TSet<UMaterialInterface*> Materials; A Actors,Samples,Grid; FCollisionQueryParams Query(SCENE_QUERY_STAT(SurfaceGroundObservation),true); FBox TerrainBounds(ForceInit);
    for(TActorIterator<AActor> It(World);It;++It) { if(!Terrain(*It)) Query.AddIgnoredActor(*It); else TerrainBounds+=It->GetComponentsBoundingBox(true); }
    for(TActorIterator<AActor> It(World);It;++It)
    {
        AActor* Actor=*It; if(!Environment(Actor)) continue; auto O=Object();O->SetStringField(TEXT("name"),Actor->GetName());
#if WITH_EDITOR
        O->SetStringField(TEXT("label"),Actor->GetActorLabel(false));
#endif
        A Tags;for(const FName& Tag:Actor->Tags) Tags.Add(MakeShared<FJsonValueString>(Tag.ToString()));O->SetArrayField(TEXT("tags"),Tags);
        O->SetObjectField(TEXT("transform"),Transform(Actor->GetActorTransform()));FVector Center,Extent;Actor->GetActorBounds(false,Center,Extent);O->SetObjectField(TEXT("world_bounds"),Box(Center,Extent)); A Components;
        TInlineComponentArray<UStaticMeshComponent*> StaticComponents(Actor);
        for(auto* C:StaticComponents)
        {
            auto Item=Object();Item->SetStringField(TEXT("name"),C->GetName());Item->SetObjectField(TEXT("transform"),Transform(C->GetComponentTransform()));Item->SetNumberField(TEXT("collision"),int32(C->GetCollisionEnabled()));
            if(UStaticMesh* M=C->GetStaticMesh()) { Meshes.Add(M);Item->SetStringField(TEXT("mesh"),M->GetPathName()); }
            A Bound; for(int32 I=0;I<C->GetNumMaterials();++I) if(auto* M=C->GetMaterial(I)) { Materials.Add(M);Bound.Add(MakeShared<FJsonValueString>(M->GetPathName())); }
            Item->SetArrayField(TEXT("material_bindings"),Bound);
            if(auto* Instanced=Cast<UInstancedStaticMeshComponent>(C)) { A Instances;for(int32 I=0;I<Instanced->GetInstanceCount();++I) { FTransform T;Instanced->GetInstanceTransform(I,T,true);Instances.Add(Value(Transform(T))); }Item->SetArrayField(TEXT("instance_transforms"),Instances); }
            Components.Add(Value(Item));
        }
        O->SetArrayField(TEXT("components"),Components); Actors.Add(Value(O));
        if(!Terrain(Actor))
        {
            Samples.Add(Value(Ground(World,Actor->GetActorLocation(),Actor->GetName()+TEXT(":origin"),Query)));
            Samples.Add(Value(Ground(World,Center-FVector(0,0,Extent.Z),Actor->GetName()+TEXT(":bounds_bottom"),Query)));
        }
        if(Actor->ActorHasTag(TEXT("DSEnvironmentBank")))
        {
            for(auto* C:StaticComponents) if(UStaticMesh* M=C->GetStaticMesh(); M && M->GetRenderData() && M->GetRenderData()->LODResources.Num())
            {
                const auto& Positions=M->GetRenderData()->LODResources[0].VertexBuffers.PositionVertexBuffer;
                const uint32 Step=FMath::Max(1u,Positions.GetNumVertices()/1024u);
                for(uint32 I=0;I<Positions.GetNumVertices();I+=Step) Samples.Add(Value(Ground(World,C->GetComponentTransform().TransformPosition(FVector(Positions.VertexPosition(I))),Actor->GetName()+FString::Printf(TEXT(":vertex:%u"),I),Query)));
            }
        }
    }
    if(TerrainBounds.IsValid)
    {
        const FVector Min=TerrainBounds.Min.ComponentMax(FVector(-1000000,-1000000,-1000000)),Max=TerrainBounds.Max.ComponentMin(FVector(1000000,1000000,1000000));
        for(int32 X=0;X<=40;++X) for(int32 Y=0;Y<=40;++Y) Grid.Add(Value(Ground(World,FVector(FMath::Lerp(Min.X,Max.X,double(X)/40),FMath::Lerp(Min.Y,Max.Y,double(Y)/40),0),FString::Printf(TEXT("grid:%d:%d"),X,Y),Query)));
    }
    A MeshRows,MaterialRows; for(auto* M:Meshes) MeshRows.Add(Value(Mesh(M)));for(auto* M:Materials) MaterialRows.Add(Value(Material(M)));
    Report->SetArrayField(TEXT("actors"),Actors);Report->SetArrayField(TEXT("meshes"),MeshRows);Report->SetArrayField(TEXT("materials"),MaterialRows);Report->SetArrayField(TEXT("ground_samples"),Samples);Report->SetArrayField(TEXT("terrain_grid"),Grid);
    Report->SetStringField(TEXT("status"),TEXT("observed")); FString Text;auto Writer=TJsonWriterFactory<>::Create(&Text);FJsonSerializer::Serialize(Report.ToSharedRef(),Writer);
    const bool Saved=FFileHelper::SaveStringToFile(Text+TEXT("\n"),*Filename,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    UE_LOG(LogTemp,Display,TEXT("Surface observation retained: %d actors, %d meshes, %d materials, %d ground samples; no media."),Actors.Num(),MeshRows.Num(),MaterialRows.Num(),Samples.Num()+Grid.Num());return Saved;
}
