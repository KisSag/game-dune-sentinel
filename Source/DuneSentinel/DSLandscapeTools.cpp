#include "DSLandscapeTools.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "Containers/ArrayView.h"
#include "Dom/JsonObject.h"
#include "Engine/CollisionProfile.h"
#include "Engine/Engine.h"
#include "Engine/LevelStreaming.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#include "LandscapeEdit.h"
#include "LandscapeEditLayer.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeProxy.h"
#include "LandscapeUtils.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

namespace DSLandscapeAuthoring
{
    constexpr int64 MaxSamples = int64(8193) * 8193;

    bool Fail(FString& OutError, const FString& Message)
    {
        OutError = Message;
        return false;
    }

    bool ResolveProjectPath(const FString& Relative, bool bOutput, FString& Absolute, FString& OutError)
    {
        FString Normalized = Relative;
        FPaths::NormalizeFilename(Normalized);
        if (Normalized.IsEmpty() || !FPaths::IsRelative(Normalized) || Normalized.Contains(TEXT(":")))
            return Fail(OutError, TEXT("File paths must be nonempty project-relative paths."));
        if (!FPaths::CollapseRelativeDirectories(Normalized) || Normalized.StartsWith(TEXT("../")) || Normalized == TEXT(".."))
            return Fail(OutError, TEXT("File paths must remain inside the project."));
        if (bOutput && !Normalized.StartsWith(TEXT("RawAssets/"), ESearchCase::CaseSensitive))
            return Fail(OutError, TEXT("Source exports must be placed inside RawAssets/."));
        FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
        FPaths::NormalizeDirectoryName(Root);
        Absolute = FPaths::ConvertRelativePathToFull(FPaths::Combine(Root, Normalized));
        FPaths::NormalizeFilename(Absolute);
        if (!Absolute.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase))
            return Fail(OutError, TEXT("Resolved file path is outside the project."));
        return true;
    }

    bool ReadInteger(const TSharedPtr<FJsonObject>& Spec, const TCHAR* Key, int32 Min, int32 Max, int32& Out, FString& Error)
    {
        double Value = 0;
        if (!Spec->TryGetNumberField(Key, Value) || !FMath::IsFinite(Value) || Value < Min || Value > Max || Value != static_cast<int32>(Value))
            return Fail(Error, FString::Printf(TEXT("%s must be an integer between %d and %d."), Key, Min, Max));
        Out = static_cast<int32>(Value);
        return true;
    }

    bool ReadVector(const TSharedPtr<FJsonObject>& Spec, const TCHAR* Key, FVector& Out, FString& Error)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Spec->TryGetArrayField(Key, Values) || Values->Num() != 3)
            return Fail(Error, FString::Printf(TEXT("%s must contain exactly three finite numbers."), Key));
        for (int32 Axis = 0; Axis < 3; ++Axis)
        {
            double Value = 0;
            if (!(*Values)[Axis].IsValid() || !(*Values)[Axis]->TryGetNumber(Value) || !FMath::IsFinite(Value))
                return Fail(Error, FString::Printf(TEXT("%s contains a non-finite or nonnumeric component."), Key));
            Out[Axis] = Value;
        }
        return true;
    }

    bool ReadRequiredString(const TSharedPtr<FJsonObject>& Spec, const TCHAR* Key, FString& Out, FString& Error)
    {
        return (Spec->TryGetStringField(Key, Out) && !Out.IsEmpty()) || Fail(Error, FString::Printf(TEXT("%s must be a nonempty string."), Key));
    }

    bool ReadRaw(const FString& Relative, int64 ExpectedBytes, TArray<uint8>& Bytes, FString& Error)
    {
        FString Absolute;
        if (!ResolveProjectPath(Relative, false, Absolute, Error)) return false;
        if (IFileManager::Get().FileSize(*Absolute) != ExpectedBytes)
            return Fail(Error, FString::Printf(TEXT("%s has an unexpected byte count."), *Relative));
        return FFileHelper::LoadFileToArray(Bytes, *Absolute) || Fail(Error, TEXT("Could not read a source raw file."));
    }

    bool ResolveAssetPath(const FString& Path, FString& PackagePath, FString& ObjectPath, FString& Error)
    {
        PackagePath = FPackageName::ObjectPathToPackageName(Path);
        if (!PackagePath.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(PackagePath))
            return Fail(Error, TEXT("LayerInfo paths must be valid /Game/ package or object paths."));
        ObjectPath = PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackagePath);
        if (Path != PackagePath && Path != ObjectPath)
            return Fail(Error, TEXT("LayerInfo object name must match its package name."));
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> VectorValues(const FVector& Value)
    {
        return { MakeShared<FJsonValueNumber>(Value.X), MakeShared<FJsonValueNumber>(Value.Y), MakeShared<FJsonValueNumber>(Value.Z) };
    }

    bool SaveJsonNew(const TSharedRef<FJsonObject>& Object, const FString& Absolute, FString& Error)
    {
        FString Text;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
        if (!FJsonSerializer::Serialize(Object, Writer) || !FFileHelper::SaveStringToFile(Text, *Absolute,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_NoReplaceExisting))
            return Fail(Error, TEXT("Could not create source metadata; existing files are never replaced."));
        return true;
    }

    bool SaveBytesNew(TArrayView64<const uint8> Bytes, const FString& Absolute, FString& Error)
    {
        return FFileHelper::SaveArrayToFile(Bytes, *Absolute, &IFileManager::Get(), FILEWRITE_NoReplaceExisting)
            || Fail(Error, TEXT("Could not create a raw export; existing files are never replaced."));
    }

    TArray<uint8> EncodeHeightLE(const TArray<uint16>& Height)
    {
        TArray<uint8> Bytes;
        Bytes.SetNumUninitialized(Height.Num() * 2);
        for (int32 Index = 0; Index < Height.Num(); ++Index)
        {
            Bytes[Index * 2] = static_cast<uint8>(Height[Index] & 255);
            Bytes[Index * 2 + 1] = static_cast<uint8>(Height[Index] >> 8);
        }
        return Bytes;
    }

    struct FPendingLayer
    {
        FName Name;
        FString PackagePath;
        FString ObjectPath;
        TArray<uint8> Weights;
        ULandscapeLayerInfoObject* ExistingInfo = nullptr;
    };
}
#endif

AActor* UDSLandscapeTools::ImportTerrainLandscape(UObject* WorldContextObject, const FString& SpecRelativePath, FString& OutError)
{
    OutError.Reset();
#if WITH_EDITOR
    using namespace DSLandscapeAuthoring;
    UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
    if (!World || World->WorldType != EWorldType::Editor || World->IsPartitionedWorld() || !FApp::CanEverRender())
    {
        Fail(OutError, TEXT("Import requires a rendering-enabled editor and a non-partitioned persistent world."));
        return nullptr;
    }
    FString SpecPath, SpecText;
    TSharedPtr<FJsonObject> Spec;
    if (!ResolveProjectPath(SpecRelativePath, false, SpecPath, OutError)) return nullptr;
    if (!FFileHelper::LoadFileToString(SpecText, *SpecPath) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(SpecText), Spec) || !Spec.IsValid())
    {
        Fail(OutError, TEXT("Could not read a JSON object from the import specification."));
        return nullptr;
    }
    int32 Width = 0, Height = 0, Sections = 0, Quads = 0;
    FVector Location, Scale;
    FString HeightPath, MaterialPath, Label;
    if (!ReadInteger(Spec, TEXT("width"), 2, 8193, Width, OutError)
        || !ReadInteger(Spec, TEXT("height"), 2, 8193, Height, OutError)
        || !ReadInteger(Spec, TEXT("sections"), 1, 2, Sections, OutError)
        || !ReadInteger(Spec, TEXT("quads"), 7, 255, Quads, OutError)
        || !ReadVector(Spec, TEXT("locationCm"), Location, OutError)
        || !ReadVector(Spec, TEXT("scaleCm"), Scale, OutError)
        || !ReadRequiredString(Spec, TEXT("heightR16LE"), HeightPath, OutError)
        || !ReadRequiredString(Spec, TEXT("materialPath"), MaterialPath, OutError)
        || !ReadRequiredString(Spec, TEXT("label"), Label, OutError)) return nullptr;
    const int32 ComponentQuads = Sections * Quads;
    const int64 SampleCount = int64(Width) * Height;
    const int32 ComponentsX = (Width - 1) / ComponentQuads;
    const int32 ComponentsY = (Height - 1) / ComponentQuads;
    if (!FMath::IsPowerOfTwo(Quads + 1) || (Width - 1) % ComponentQuads || (Height - 1) % ComponentQuads
        || ComponentsX < 1 || ComponentsY < 1 || int64(ComponentsX) * ComponentsY > 1024 || SampleCount > MaxSamples)
    {
        Fail(OutError, TEXT("Landscape dimensions must fit complete legal components, with at most 1024 components."));
        return nullptr;
    }
    if (Scale.GetMin() <= 0 || Scale.GetMax() > 1000000 || Location.GetAbsMax() > 1000000000 || Label.Len() > 128
        || Label.Contains(TEXT("\n")) || Label.Contains(TEXT("\r")))
    {
        Fail(OutError, TEXT("Scale must be positive and bounded; location and label must be valid."));
        return nullptr;
    }
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        if (It->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase) || It->IsA<ALandscapeProxy>())
        {
            Fail(OutError, TEXT("The world already contains a Landscape or an actor with the requested label. No actors were replaced."));
            return nullptr;
        }
    }
    UMaterialInterface* Material = nullptr;
    if (MaterialPath.StartsWith(TEXT("/Game/")) || MaterialPath.StartsWith(TEXT("/Engine/")))
        Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
    if (!Material)
    {
        Fail(OutError, TEXT("materialPath must identify an existing material interface."));
        return nullptr;
    }
    TArray<uint8> HeightBytes;
    if (!ReadRaw(HeightPath, SampleCount * 2, HeightBytes, OutError)) return nullptr;
    TArray<FPendingLayer> PendingLayers;
    const TArray<TSharedPtr<FJsonValue>>* Layers = nullptr;
    if (Spec->HasField(TEXT("weightLayers")) && !Spec->TryGetArrayField(TEXT("weightLayers"), Layers))
    {
        Fail(OutError, TEXT("weightLayers must be an array when supplied."));
        return nullptr;
    }
    TSet<FName> Names;
    TSet<FName> Packages;
    TArray<FName> MaterialLayers = ALandscapeProxy::GetLayersFromMaterial(Material);
    if (ALandscapeProxy::VisibilityLayer)
    {
        MaterialLayers.Remove(ALandscapeProxy::VisibilityLayer->GetLayerName());
    }
    if (Layers)
    {
        if (Layers->Num() > 16)
        {
            Fail(OutError, TEXT("At most 16 paint layers are supported by this import bridge."));
            return nullptr;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Layers)
        {
            const TSharedPtr<FJsonObject>* Layer = nullptr;
            FString Name, WeightsPath, InfoPath;
            if (!Value.IsValid() || !Value->TryGetObject(Layer) || !Layer || !Layer->IsValid())
            {
                Fail(OutError, TEXT("Each paint layer must be an object."));
                return nullptr;
            }
            if (!ReadRequiredString(*Layer, TEXT("name"), Name, OutError)
                || !ReadRequiredString(*Layer, TEXT("weightR8"), WeightsPath, OutError)
                || !ReadRequiredString(*Layer, TEXT("layerInfoPath"), InfoPath, OutError)) return nullptr;
            if (Name.Len() >= NAME_SIZE || Name.Contains(TEXT("\n")) || Name.Contains(TEXT("\r")))
            {
                Fail(OutError, TEXT("Paint layer name is too long or contains a line break."));
                return nullptr;
            }
            FPendingLayer Pending;
            Pending.Name = FName(*Name);
            if (!MaterialLayers.Contains(Pending.Name))
            {
                Fail(OutError, TEXT("A paint layer name is not present in the specified Landscape material."));
                return nullptr;
            }
            if (!ResolveAssetPath(InfoPath, Pending.PackagePath, Pending.ObjectPath, OutError)
                || !ReadRaw(WeightsPath, SampleCount, Pending.Weights, OutError)) return nullptr;
            if (Pending.Name.IsNone() || Names.Contains(Pending.Name) || Packages.Contains(FName(*Pending.PackagePath)))
            {
                Fail(OutError, TEXT("Paint layer names and LayerInfo packages must be unique."));
                return nullptr;
            }
            Names.Add(Pending.Name);
            Packages.Add(FName(*Pending.PackagePath));
            const bool bPackageExists = FPackageName::DoesPackageExist(Pending.PackagePath) || FindPackage(nullptr, *Pending.PackagePath);
            if (bPackageExists)
            {
                Pending.ExistingInfo = LoadObject<ULandscapeLayerInfoObject>(nullptr, *Pending.ObjectPath);
                if (!Pending.ExistingInfo || Pending.ExistingInfo->GetLayerName() != Pending.Name
                    || Pending.ExistingInfo->GetBlendMethod() != ELandscapeTargetLayerBlendMethod::FinalWeightBlending)
                {
                    Fail(OutError, TEXT("An existing LayerInfo must match the requested name and final weight blending. Existing assets are not changed."));
                    return nullptr;
                }
            }
            PendingLayers.Add(MoveTemp(Pending));
        }
    }
    if (!PendingLayers.IsEmpty())
    {
        for (int32 Index = 0; Index < SampleCount; ++Index)
        {
            int32 Sum = 0;
            for (const FPendingLayer& Layer : PendingLayers) Sum += Layer.Weights[Index];
            if (Sum != 255)
            {
                Fail(OutError, FString::Printf(TEXT("Paint weights must sum to 255 at every sample; sample %d sums to %d."), Index, Sum));
                return nullptr;
            }
        }
    }

    // All source files, dimensions and destinations are validated before authoring starts.
    TMap<FGuid, TArray<uint16>> Heights;
    TArray<uint16>& HeightValues = Heights.Add(FGuid());
    HeightValues.SetNumUninitialized(static_cast<int32>(SampleCount));
    for (int32 Index = 0; Index < SampleCount; ++Index)
        HeightValues[Index] = uint16(HeightBytes[Index * 2]) | (uint16(HeightBytes[Index * 2 + 1]) << 8);
    FActorSpawnParameters SpawnParameters;
    SpawnParameters.OverrideLevel = World->PersistentLevel;
    SpawnParameters.ObjectFlags |= RF_Transactional;
    ALandscape* Landscape = World->SpawnActor<ALandscape>(ALandscape::StaticClass(), Location, FRotator::ZeroRotator, SpawnParameters);
    if (!Landscape)
    {
        Fail(OutError, TEXT("Could not create the Landscape actor."));
        return nullptr;
    }
    Landscape->SetActorScale3D(Scale);
    Landscape->SetActorLabel(Label);
    Landscape->Tags.AddUnique(FName(TEXT("DSEnvironmentTerrain")));
    Landscape->CollisionMipLevel = 0;
    Landscape->SimpleCollisionMipLevel = 0;
    Landscape->BodyInstance.SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
    // Import creates the component material instances from this property. The editor setter is not exported.
    Landscape->LandscapeMaterial = Material;
    TMap<FGuid, TArray<FLandscapeImportLayerInfo>> Paint;
    TArray<FLandscapeImportLayerInfo>& PaintLayers = Paint.Add(FGuid());
    for (FPendingLayer& Pending : PendingLayers)
    {
        ULandscapeLayerInfoObject* Info = Pending.ExistingInfo;
        if (!Info)
        {
            UPackage* Package = CreatePackage(*Pending.PackagePath);
            Info = NewObject<ULandscapeLayerInfoObject>(Package, *FPackageName::GetLongPackageAssetName(Pending.PackagePath), RF_Public | RF_Standalone | RF_Transactional);
            Info->SetLayerName(Pending.Name, false);
            Info->SetBlendMethod(ELandscapeTargetLayerBlendMethod::FinalWeightBlending, false);
            Info->MarkPackageDirty();
            FAssetRegistryModule::AssetCreated(Info);
        }
        FLandscapeImportLayerInfo& ImportLayer = PaintLayers.Emplace_GetRef(Pending.Name);
        ImportLayer.LayerInfo = Info;
        ImportLayer.LayerData = MoveTemp(Pending.Weights);
    }
    Landscape->Import(FGuid::NewGuid(), 0, 0, Width - 1, Height - 1, Sections, Quads, Heights,
        *HeightPath, Paint, ELandscapeImportAlphamapType::Additive, TArrayView<const FLandscapeLayer>());
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 7
    // 5.7 Import writes its default edit layer with legacy weight adjustment:
    // each later layer redistributes earlier layers even for Additive input.
    // These source layers are already normalized together. Restore their exact
    // values without that redistribution; retain the final-weight blend mode.
    if (!PaintLayers.IsEmpty())
    {
        ULandscapeInfo* ImportedInfo = Landscape->GetLandscapeInfo();
        const ULandscapeEditLayerBase* DefaultEditLayer = Landscape->GetEditLayerConst(0);
        if (!ImportedInfo || !DefaultEditLayer)
        {
            Fail(OutError, TEXT("The imported Landscape has no default edit layer for exact paint restoration."));
            return nullptr;
        }
        FLandscapeEditDataInterface Edit(ImportedInfo, DefaultEditLayer->GetGuid(), true);
        for (const FLandscapeImportLayerInfo& Layer : PaintLayers)
        {
PRAGMA_DISABLE_DEPRECATION_WARNINGS
            Edit.SetAlphaData(Layer.LayerInfo, 0, 0, Width - 1, Height - 1, Layer.LayerData.GetData(), Width,
                ELandscapeLayerPaintingRestriction::None, /*bWeightAdjust=*/false, /*bTotalWeightAdjust=*/false);
PRAGMA_ENABLE_DEPRECATION_WARNINGS
        }
        TArray<uint8> RestoredWeights;
        RestoredWeights.SetNumUninitialized(static_cast<int32>(SampleCount));
        for (const FLandscapeImportLayerInfo& Layer : PaintLayers)
        {
            Edit.GetWeightDataFast(Layer.LayerInfo, 0, 0, Width - 1, Height - 1, RestoredWeights.GetData(), Width);
            if (RestoredWeights != Layer.LayerData)
            {
                Fail(OutError, FString::Printf(TEXT("Exact edit-layer paint restoration failed for %s."), *Layer.LayerInfo->GetLayerName().ToString()));
                return nullptr;
            }
        }
        Edit.Flush();
        Landscape->RequestLayersContentUpdateForceAll();
    }
#endif
    Landscape->MarkPackageDirty();
    return Landscape;
#else
    OutError = TEXT("Landscape authoring is only available in an editor build.");
    return nullptr;
#endif
}

bool UDSLandscapeTools::ExportLandscapeSource(AActor* SourceActor, const FString& OutputRelativeDirectory, FString& OutError)
{
    OutError.Reset();
#if WITH_EDITOR
    using namespace DSLandscapeAuthoring;
    ALandscapeProxy* Proxy = Cast<ALandscapeProxy>(SourceActor);
    ULandscapeInfo* Info = Proxy ? Proxy->GetLandscapeInfo() : nullptr;
    ALandscape* Landscape = Proxy ? Proxy->GetLandscapeActor() : nullptr;
    UWorld* SourceWorld = Proxy ? Proxy->GetWorld() : nullptr;
    if (!Info || !Landscape || !SourceWorld || SourceWorld->WorldType != EWorldType::Editor
        || !FApp::CanEverRender() || !Landscape->IsUpToDate())
        return Fail(OutError, TEXT("Export requires a loaded Landscape with completed final layers in a rendering-enabled editor."));
    // Non-partitioned streamed levels are not covered by the Landscape actor-desc extent query.
    for (const ULevelStreaming* Streaming : SourceWorld->GetStreamingLevels())
        if (Streaming && (!Streaming->IsLevelLoaded() || !Streaming->IsLevelVisible()))
            return Fail(OutError, TEXT("Load and show all source streaming levels before exporting the complete Landscape."));
    FIntRect Extent;
    if (!Info->GetLandscapeExtent(Extent) || Extent != Info->GetCompleteLandscapeExtent()
        || Info->HasUnloadedComponentsInRegion(Extent.Min.X, Extent.Min.Y, Extent.Max.X, Extent.Max.Y))
        return Fail(OutError, TEXT("Load every source Landscape component before exporting; incomplete regions are not exported."));
    const int64 Width = int64(Extent.Max.X) - Extent.Min.X + 1;
    const int64 Height = int64(Extent.Max.Y) - Extent.Min.Y + 1;
    if (Width < 2 || Height < 2 || Width > 8193 || Height > 8193 || Width * Height > MaxSamples)
        return Fail(OutError, TEXT("The source heightfield dimensions are invalid or exceed 8193 samples per axis."));
    const int32 ComponentQuads = Info->ComponentSizeQuads;
    if (ComponentQuads <= 0 || Extent.Min.X % ComponentQuads || Extent.Min.Y % ComponentQuads
        || (Width - 1) % ComponentQuads || (Height - 1) % ComponentQuads)
        return Fail(OutError, TEXT("The source component grid is not a complete aligned heightfield rectangle."));
    for (int32 Y = Extent.Min.Y; Y < Extent.Max.Y; Y += ComponentQuads)
    {
        for (int32 X = Extent.Min.X; X < Extent.Max.X; X += ComponentQuads)
        {
            const ULandscapeComponent* Component = Info->XYtoComponentMap.FindRef(FIntPoint(X / ComponentQuads, Y / ComponentQuads));
            if (!Component || Component->GetSectionBase() != FIntPoint(X, Y) || Component->ComponentSizeQuads != ComponentQuads
                || !Component->GetHeightmap(FGuid()) || !Component->GetHeightmap(FGuid())->Source.IsValid())
                return Fail(OutError, TEXT("The source rectangle contains a missing component or unavailable final height texture."));
        }
    }
    FString Output;
    if (!ResolveProjectPath(OutputRelativeDirectory, true, Output, OutError)) return false;
    if (IFileManager::Get().DirectoryExists(*Output) || IFileManager::Get().FileExists(*Output))
        return Fail(OutError, TEXT("The export directory must not already exist."));

    FLandscapeEditDataInterface Edit(Info, FGuid(), false);
    Edit.SetShouldDirtyPackage(false);
    TArray<uint16> HeightValues;
    HeightValues.SetNumZeroed(static_cast<int32>(Width * Height));
    Edit.GetHeightDataFast(Extent.Min.X, Extent.Min.Y, Extent.Max.X, Extent.Max.Y, HeightValues.GetData(), static_cast<int32>(Width));
    TArray<uint8> HeightBytes = EncodeHeightLE(HeightValues);
    TArray<TSharedPtr<FJsonValue>> LayerMetadata;
    TArray<TArray<uint8>> WeightBytes;
    TSet<FName> SeenLayers;
    for (const FLandscapeInfoLayerSettings& Settings : Info->Layers)
    {
        ULandscapeLayerInfoObject* Layer = Settings.LayerInfoObj;
        if (!Layer || SeenLayers.Contains(Layer->GetLayerName())) continue;
        SeenLayers.Add(Layer->GetLayerName());
        const int32 Index = WeightBytes.Num();
        TArray<uint8>& Weights = WeightBytes.Emplace_GetRef();
        Weights.SetNumZeroed(static_cast<int32>(Width * Height));
        Edit.GetWeightDataFast(Layer, Extent.Min.X, Extent.Min.Y, Extent.Max.X, Extent.Max.Y, Weights.GetData(), static_cast<int32>(Width));
        TSharedRef<FJsonObject> LayerJson = MakeShared<FJsonObject>();
        LayerJson->SetStringField(TEXT("name"), Layer->GetLayerName().ToString());
        LayerJson->SetStringField(TEXT("sourceAsset"), Layer->GetPathName());
        LayerJson->SetStringField(TEXT("file"), FString::Printf(TEXT("weight_%02d.r8"), Index));
        LayerJson->SetNumberField(TEXT("byteCount"), Weights.Num());
        LayerJson->SetNumberField(TEXT("blendMethod"), static_cast<int32>(Layer->GetBlendMethod()));
        LayerMetadata.Add(MakeShared<FJsonValueObject>(LayerJson));
    }
    const FTransform Transform = Proxy->LandscapeActorToWorld();
    TSharedRef<FJsonObject> Metadata = MakeShared<FJsonObject>();
    Metadata->SetStringField(TEXT("schema"), TEXT("terrain-source/v1"));
    Metadata->SetStringField(TEXT("sourceActor"), Proxy->GetPathName());
    Metadata->SetStringField(TEXT("sourceMap"), Proxy->GetWorld()->GetOutermost()->GetName());
    Metadata->SetStringField(TEXT("heightR16LE"), TEXT("height.r16"));
    Metadata->SetStringField(TEXT("sampleOrder"), TEXT("row-major, X increases within each row, Y increases between rows"));
    Metadata->SetNumberField(TEXT("width"), static_cast<double>(Width));
    Metadata->SetNumberField(TEXT("height"), static_cast<double>(Height));
    Metadata->SetNumberField(TEXT("heightByteCount"), HeightBytes.Num());
    Metadata->SetNumberField(TEXT("localHeightOffset"), 32768);
    Metadata->SetNumberField(TEXT("localHeightDivisor"), 128);
    Metadata->SetArrayField(TEXT("extentMin"), { MakeShared<FJsonValueNumber>(Extent.Min.X), MakeShared<FJsonValueNumber>(Extent.Min.Y) });
    Metadata->SetArrayField(TEXT("extentMaxInclusive"), { MakeShared<FJsonValueNumber>(Extent.Max.X), MakeShared<FJsonValueNumber>(Extent.Max.Y) });
    Metadata->SetArrayField(TEXT("locationCm"), VectorValues(Transform.GetLocation()));
    Metadata->SetArrayField(TEXT("scaleCm"), VectorValues(Transform.GetScale3D()));
    const FQuat Rotation = Transform.GetRotation();
    Metadata->SetArrayField(TEXT("rotationQuaternionXYZW"), { MakeShared<FJsonValueNumber>(Rotation.X), MakeShared<FJsonValueNumber>(Rotation.Y), MakeShared<FJsonValueNumber>(Rotation.Z), MakeShared<FJsonValueNumber>(Rotation.W) });
    Metadata->SetArrayField(TEXT("weightLayers"), LayerMetadata);
    if (!IFileManager::Get().MakeDirectory(*Output, true)) return Fail(OutError, TEXT("Could not create the source export directory."));
    if (!SaveBytesNew(TConstArrayView64<uint8>(HeightBytes.GetData(), HeightBytes.Num()), Output / TEXT("height.r16"), OutError)) return false;
    for (int32 Index = 0; Index < WeightBytes.Num(); ++Index)
        if (!SaveBytesNew(TConstArrayView64<uint8>(WeightBytes[Index].GetData(), WeightBytes[Index].Num()), Output / FString::Printf(TEXT("weight_%02d.r8"), Index), OutError)) return false;
    return SaveJsonNew(Metadata, Output / TEXT("source.json"), OutError);
#else
    OutError = TEXT("Landscape source export is only available in an editor build.");
    return false;
#endif
}

bool UDSLandscapeTools::ExportTextureSourceMip0(UTexture2D* Texture, const FString& OutputRelativePath, FString& OutError)
{
    OutError.Reset();
#if WITH_EDITOR
    using namespace DSLandscapeAuthoring;
    if (!Texture || !Texture->Source.IsValid() || Texture->Source.GetNumBlocks() != 1
        || Texture->Source.GetNumLayers() != 1 || Texture->Source.GetNumSlices() != 1)
        return Fail(OutError, TEXT("Texture export requires one valid 2D source block, layer and slice."));
    FString Output;
    if (!ResolveProjectPath(OutputRelativePath, true, Output, OutError)) return false;
    const FString MetadataPath = Output + TEXT(".json");
    if (IFileManager::Get().FileExists(*Output) || IFileManager::Get().DirectoryExists(*Output)
        || IFileManager::Get().FileExists(*MetadataPath) || IFileManager::Get().DirectoryExists(*MetadataPath))
        return Fail(OutError, TEXT("Texture output and metadata paths must not already exist."));
    const ETextureSourceFormat Format = Texture->Source.GetFormat();
    if (Format <= TSF_Invalid || Format >= TSF_MAX)
        return Fail(OutError, TEXT("Texture source format is invalid."));
    TArray64<uint8> Bytes;
    if (!Texture->Source.GetMipData(Bytes, 0) || Bytes.IsEmpty() || Bytes.Num() != Texture->Source.CalcMipSize(0))
        return Fail(OutError, TEXT("Could not read the complete source mip zero payload."));
    TSharedRef<FJsonObject> Metadata = MakeShared<FJsonObject>();
    Metadata->SetStringField(TEXT("schema"), TEXT("texture-source/v1"));
    Metadata->SetStringField(TEXT("sourceAsset"), Texture->GetPathName());
    Metadata->SetStringField(TEXT("file"), FPaths::GetCleanFilename(Output));
    Metadata->SetStringField(TEXT("sourceFormat"), GTextureSourceFormats[Format].Name);
    Metadata->SetNumberField(TEXT("sourceFormatId"), static_cast<int32>(Format));
    Metadata->SetNumberField(TEXT("width"), Texture->Source.GetSizeX());
    Metadata->SetNumberField(TEXT("height"), Texture->Source.GetSizeY());
    Metadata->SetNumberField(TEXT("bytesPerPixel"), GTextureSourceFormats[Format].BytesPerPixel);
    Metadata->SetNumberField(TEXT("byteCount"), static_cast<double>(Bytes.Num()));
    Metadata->SetNumberField(TEXT("mip"), 0);
    Metadata->SetBoolField(TEXT("sourceSRGB"), Texture->SRGB);
    Metadata->SetBoolField(TEXT("channelDecodingApplied"), false);
    Metadata->SetBoolField(TEXT("gammaConversionApplied"), false);
    Metadata->SetStringField(TEXT("byteOrder"), PLATFORM_LITTLE_ENDIAN ? TEXT("little-endian") : TEXT("big-endian"));
    if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Output), true))
        return Fail(OutError, TEXT("Could not create the texture export parent directory."));
    return SaveBytesNew(TConstArrayView64<uint8>(Bytes.GetData(), Bytes.Num()), Output, OutError) && SaveJsonNew(Metadata, MetadataPath, OutError);
#else
    OutError = TEXT("Texture source export is only available in an editor build.");
    return false;
#endif
}
