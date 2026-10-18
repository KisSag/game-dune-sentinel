#include "DSContentTools.h"
#if WITH_EDITOR
#include "Engine/Font.h"
#include "Engine/FontFace.h"
#include "Fonts/CompositeFont.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#endif

bool UDSContentTools::ImportRuntimeFont(const FString& SourceFilename,const FString& FontPackagePath)
{
#if WITH_EDITOR
    if(!FontPackagePath.StartsWith(TEXT("/Game/"))) return false;
    TArray<uint8> Bytes;
    if(!FFileHelper::LoadFileToArray(Bytes,*SourceFilename) || Bytes.IsEmpty()) return false;
    const FString FacePackagePath=FontPackagePath+TEXT("_Face");
    UPackage* FacePackage=CreatePackage(*FacePackagePath);
    FacePackage->FullyLoad();
    const FString FaceName=FPackageName::GetLongPackageAssetName(FacePackagePath);
    UFontFace* Face=FindObject<UFontFace>(FacePackage,*FaceName);
    if(!Face) Face=NewObject<UFontFace>(FacePackage,*FaceName,RF_Public|RF_Standalone);
    Face->FontFaceData=FFontFaceData::MakeFontFaceData(MoveTemp(Bytes));
    Face->LoadingPolicy=EFontLoadingPolicy::Inline;
    Face->SourceFilename=SourceFilename;
    FPaths::MakePathRelativeTo(Face->SourceFilename,*FPaths::ProjectDir());
    Face->CacheSubFaces();
    Face->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(Face);

    UPackage* FontPackage=CreatePackage(*FontPackagePath);
    FontPackage->FullyLoad();
    const FString FontName=FPackageName::GetLongPackageAssetName(FontPackagePath);
    UFont* Font=FindObject<UFont>(FontPackage,*FontName);
    if(!Font) Font=NewObject<UFont>(FontPackage,*FontName,RF_Public|RF_Standalone);
    Font->FontCacheType=EFontCacheType::Runtime;
    Font->LegacyFontSize=22;
    Font->LegacyFontName=TEXT("Regular");
    FTypefaceEntry Entry(TEXT("Regular"));
    Entry.Font=FFontData(Face);
    Font->GetMutableInternalCompositeFont().DefaultTypeface.Fonts={Entry};
    Font->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(Font);
    FSavePackageArgs Args;
    Args.TopLevelFlags=RF_Public|RF_Standalone;
    Args.SaveFlags=SAVE_NoError;
    const FString FaceFile=FPackageName::LongPackageNameToFilename(FacePackagePath,FPackageName::GetAssetPackageExtension());
    const FString FontFile=FPackageName::LongPackageNameToFilename(FontPackagePath,FPackageName::GetAssetPackageExtension());
    return UPackage::SavePackage(FacePackage,Face,*FaceFile,Args) && UPackage::SavePackage(FontPackage,Font,*FontFile,Args);
#else
    return false;
#endif
}

FVector UDSContentTools::GetMaterialSectionCenter(UStaticMesh* Mesh,const FString& SlotName)
{
#if WITH_EDITOR
    if(!Mesh || !Mesh->GetRenderData() || Mesh->GetRenderData()->LODResources.IsEmpty()) return FVector::ZeroVector;
    const FStaticMeshLODResources& LOD=Mesh->GetRenderData()->LODResources[0];
    const TArray<FStaticMaterial>& Materials=Mesh->GetStaticMaterials();
    FVector Sum=FVector::ZeroVector;
    double Area=0;
    for(const FStaticMeshSection& Section:LOD.Sections)
    {
        if(!Materials.IsValidIndex(Section.MaterialIndex) || !Materials[Section.MaterialIndex].MaterialSlotName.ToString().Contains(SlotName)) continue;
        for(uint32 I=0;I<Section.NumTriangles;++I)
        {
            const uint32 Base=Section.FirstIndex+I*3;
            const FVector A(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(LOD.IndexBuffer.GetIndex(Base)));
            const FVector B(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(LOD.IndexBuffer.GetIndex(Base+1)));
            const FVector C(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(LOD.IndexBuffer.GetIndex(Base+2)));
            const double Weight=FVector::CrossProduct(B-A,C-A).Size();
            Sum+=(A+B+C)/3.0*Weight;
            Area+=Weight;
        }
    }
    return Area>SMALL_NUMBER?Sum/Area:FVector::ZeroVector;
#else
    return FVector::ZeroVector;
#endif
}
