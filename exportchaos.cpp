// Copyright 2019 Lipeng Zha, Inc. All Rights Reserved.

#include "ExportChaosEditor.h"
#include "ExportChaosStyle.h"
#include "ExportChaosCommands.h"

#include "Misc/MessageDialog.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "DesktopPlatformModule.h"
#include "LevelEditor.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include <vector>
#include <unordered_map>

#include "Misc/Paths.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
//#include "Resources/Version.h"
#include "UObject/ArchiveCookContext.h"
#include "UObject/SavePackage.h"

#include "EngineUtils.h"
#include "PhysicsEngine/BodySetup.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"

#include "JsonObjectConverter.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonTypes.h"
#include "Json.h"

#include "Field/FieldSystemActor.h"
#include "Field/FieldSystemComponent.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "LandscapeHeightfieldCollisionComponent.h"

static const FName ExportChaosTabName("ExportChaos");

#define LOCTEXT_NAMESPACE "FExportChaosEditorModule"

void FExportChaosEditorModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
	
	FExportChaosStyle::Initialize();
	FExportChaosStyle::ReloadTextures();

	FExportChaosCommands::Register();
	
	PluginCommands = MakeShareable(new FUICommandList);

	PluginCommands->MapAction(
		FExportChaosCommands::Get().PluginAction,
		FExecuteAction::CreateRaw(this, &FExportChaosEditorModule::PluginButtonClicked),
		FCanExecuteAction());
		
	FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");
	
	{
		TSharedPtr<FExtender> MenuExtender = MakeShareable(new FExtender());
		MenuExtender->AddMenuExtension("WindowLayout", EExtensionHook::After, PluginCommands, FMenuExtensionDelegate::CreateRaw(this, &FExportChaosEditorModule::AddMenuExtension));

		LevelEditorModule.GetMenuExtensibilityManager()->AddExtender(MenuExtender);
	}
	
	{
		TSharedPtr<FExtender> ToolbarExtender = MakeShareable(new FExtender);
		ToolbarExtender->AddToolBarExtension("Settings", EExtensionHook::After, PluginCommands, FToolBarExtensionDelegate::CreateRaw(this, &FExportChaosEditorModule::AddToolbarExtension));
		
		LevelEditorModule.GetToolBarExtensibilityManager()->AddExtender(ToolbarExtender);
	}
}

void FExportChaosEditorModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
	FExportChaosStyle::Shutdown();

	FExportChaosCommands::Unregister();
}

void FExportChaosEditorModule::PluginButtonClicked()
{
	ExportPhysicData();	
}

void FExportChaosEditorModule::NotFountAnyValidChaosDataMsg()
{
	UWorld* World = GEditor->GetEditorWorldContext().World();
	FText DialogText = FText::Format(
		LOCTEXT("NotFoundValidChaosDialogText", "Not found any valid Chaosigation data in {0} Map!"),
		FText::FromString(World->GetMapName())
	);
	FMessageDialog::Open(EAppMsgType::Ok, DialogText);
}

void FExportChaosEditorModule::CreateSaveFileNotify(const FText& InMsg, const FString& InSavedFile)
{
	auto Message = InMsg;
	FNotificationInfo Info(Message);
	Info.bFireAndForget = true;
	Info.ExpireDuration = 5.0f;
	Info.bUseSuccessFailIcons = false;
	Info.bUseLargeFont = false;

	const FString HyperLinkText = InSavedFile;
	Info.Hyperlink = FSimpleDelegate::CreateStatic(
		[](FString SourceFilePath)
	{
		FPlatformProcess::ExploreFolder(*SourceFilePath);
	},
		HyperLinkText
		);
	Info.HyperlinkText = FText::FromString(HyperLinkText);

	FSlateNotificationManager::Get().AddNotification(Info)->SetCompletionState(SNotificationItem::CS_Success);
}




TSharedPtr<FJsonObject> JsonStrToJsonObj(FString JSONStr)
{
	auto JsonReader = TJsonReaderFactory<>::Create(JSONStr);
	TSharedPtr<FJsonObject> JsonObject;
	FJsonSerializer::Deserialize(JsonReader, JsonObject);
	return JsonObject;
}

FString JsonObjToJsonStr(TSharedPtr<FJsonObject> JsonObject)
{
	FString JsonStr;
	TSharedRef<TJsonWriter<>> JsonWriter = TJsonWriterFactory<>::Create(&JsonStr);
	FJsonSerializer::Serialize(JsonObject.ToSharedRef(), JsonWriter);
	return JsonStr;
}

template<typename EnumType>
auto enum_to_int(EnumType e)
{
	return static_cast<std::underlying_type_t<EnumType>>(e);
}

constexpr float DEFAULT_CAPSULE_RADIUS = 50;
constexpr float DEFAULT_CAPSULE_HALFHEIGHT = 90;

void SaveBodyInstanceDetail(TSharedPtr<FJsonObject> detail_info, FBodyInstance* BodyInstance)
{
	UBodySetup* BodySetup = BodyInstance->GetBodySetup();
	if (BodySetup == nullptr)
		return;
	const auto& default_instance = &BodySetup->DefaultInstance;
	if (default_instance->SleepFamily != BodyInstance->SleepFamily)
		detail_info->SetNumberField("SleepFamily", enum_to_int(BodyInstance->SleepFamily));
	if (default_instance->bUseCCD != BodyInstance->bUseCCD)
		detail_info->SetBoolField("bUseCCD", BodyInstance->bUseCCD);
	if (default_instance->bIgnoreAnalyticCollisions != BodyInstance->bIgnoreAnalyticCollisions)
		detail_info->SetBoolField("bIgnoreAnalyticCollisions", BodyInstance->bIgnoreAnalyticCollisions);
	if (default_instance->bNotifyRigidBodyCollision != BodyInstance->bNotifyRigidBodyCollision)
		detail_info->SetBoolField("bNotifyRigidBodyCollision", BodyInstance->bNotifyRigidBodyCollision);
	if (default_instance->bSmoothEdgeCollisions != BodyInstance->bSmoothEdgeCollisions)
		detail_info->SetBoolField("bSmoothEdgeCollisions", BodyInstance->bSmoothEdgeCollisions);
	if (default_instance->bLockTranslation != BodyInstance->bLockTranslation)
		detail_info->SetBoolField("bLockTranslation", BodyInstance->bLockTranslation);
	if (default_instance->bLockRotation != BodyInstance->bLockRotation)
		detail_info->SetBoolField("bLockRotation", BodyInstance->bLockRotation);
	if (default_instance->bLockXTranslation != BodyInstance->bLockXTranslation)
		detail_info->SetBoolField("bLockXTranslation", BodyInstance->bLockXTranslation);
	if (default_instance->bLockYTranslation != BodyInstance->bLockYTranslation)
		detail_info->SetBoolField("bLockYTranslation", BodyInstance->bLockYTranslation);
	if (default_instance->bLockZTranslation != BodyInstance->bLockZTranslation)
		detail_info->SetBoolField("bLockZTranslation", BodyInstance->bLockZTranslation);
	if (default_instance->bLockXRotation != BodyInstance->bLockXRotation)
		detail_info->SetBoolField("bLockXRotation", BodyInstance->bLockXRotation);
	if (default_instance->bLockYRotation != BodyInstance->bLockYRotation)
		detail_info->SetBoolField("bLockYRotation", BodyInstance->bLockYRotation);
	if (default_instance->bLockZRotation != BodyInstance->bLockZRotation)
		detail_info->SetBoolField("bLockZRotation", BodyInstance->bLockZRotation);
	if (default_instance->bOverrideMaxAngularVelocity != BodyInstance->bOverrideMaxAngularVelocity)
		detail_info->SetBoolField("bOverrideMaxAngularVelocity", BodyInstance->bOverrideMaxAngularVelocity);
	if (default_instance->GetMassOverride() != BodyInstance->GetMassOverride())
		detail_info->SetNumberField("MassOverride", BodyInstance->GetMassOverride());
	if (default_instance->LinearDamping != BodyInstance->LinearDamping)
		detail_info->SetNumberField("LinearDamping", BodyInstance->LinearDamping);
	if (default_instance->AngularDamping != BodyInstance->AngularDamping)
		detail_info->SetNumberField("AngularDamping", BodyInstance->AngularDamping);
	if (default_instance->CustomDOFPlaneNormal != BodyInstance->CustomDOFPlaneNormal)
		detail_info->SetStringField("CustomDOFPlaneNormal", BodyInstance->CustomDOFPlaneNormal.ToString());
	if (default_instance->COMNudge != BodyInstance->COMNudge)
		detail_info->SetStringField("COMNudge", BodyInstance->COMNudge.ToString());
	if (default_instance->MassScale != BodyInstance->MassScale)
		detail_info->SetNumberField("MassScale", BodyInstance->MassScale);
	if (default_instance->InertiaTensorScale != BodyInstance->InertiaTensorScale)
		detail_info->SetStringField("InertiaTensorScale", BodyInstance->InertiaTensorScale.ToString());
	if (default_instance->MaxAngularVelocity != BodyInstance->MaxAngularVelocity)
		detail_info->SetNumberField("MaxAngularVelocity", BodyInstance->MaxAngularVelocity);
	if (default_instance->CustomSleepThresholdMultiplier != BodyInstance->CustomSleepThresholdMultiplier)
		detail_info->SetNumberField("CustomSleepThresholdMultiplier", BodyInstance->CustomSleepThresholdMultiplier);
	if (default_instance->StabilizationThresholdMultiplier != BodyInstance->StabilizationThresholdMultiplier)
		detail_info->SetNumberField("StabilizationThresholdMultiplier", BodyInstance->StabilizationThresholdMultiplier);

}

enum class EPhysicFieldType : uint8
{
	None,
	DirectionalForce, //向一个方向施加一个力
};





void ExportLandscape(FString SavePath, ALandscape* landscape, TArray<TSharedPtr<FJsonValue>>& LandInfoArray)
{
	ULandscapeInfo* Info = landscape->GetLandscapeInfo();
	if (Info == nullptr)
		return;
	TSharedPtr<FJsonObject> land_info = MakeShareable(new FJsonObject);
	land_info->SetNumberField("LandID", landscape->GetUniqueID());
	land_info->SetStringField("LandGuid", landscape->GetLandscapeGuid().ToString());
	land_info->SetNumberField("LandscapeSectionOffsetX", landscape->LandscapeSectionOffset.X);
	land_info->SetNumberField("LandscapeSectionOffsetY", landscape->LandscapeSectionOffset.Y);
	land_info->SetStringField("ActorToWorld", landscape->ActorToWorld().ToString());
	land_info->SetStringField("LandscapeActorToWorld", landscape->LandscapeActorToWorld().ToString());
	
	TArray<TSharedPtr<FJsonValue >> CollisionInfoArray;
	for (const auto& [key, CollisionComponent] : Info->XYtoCollisionComponentMap)
	{
		if (CollisionComponent->CookedCollisionData.Num() == 0)
			continue;
		
		FString PackageFileName = SavePath / CollisionComponent->GetName() + ".data";
		TUniquePtr<FArchive> FileAr(IFileManager::Get().CreateFileWriter(*PackageFileName));
		if (FileAr == NULL)
			continue;
		CollisionComponent->CookedCollisionData.BulkSerialize(*FileAr);
		FileAr->Close();
			
		TSharedPtr<FJsonObject> coll_info = MakeShareable(new FJsonObject);
		coll_info->SetNumberField("OwnerID", landscape->GetUniqueID());
		coll_info->SetNumberField("CompID", CollisionComponent->GetUniqueID());
		coll_info->SetStringField("Package", CollisionComponent->GetName());
		coll_info->SetNumberField("SectionBaseX", CollisionComponent->SectionBaseX);
		coll_info->SetNumberField("SectionBaseY", CollisionComponent->SectionBaseY);
		coll_info->SetNumberField("SimpleCollisionSizeQuads", CollisionComponent->SimpleCollisionSizeQuads);
		coll_info->SetNumberField("CollisionScale", CollisionComponent->CollisionScale);
		coll_info->SetNumberField("CollisionSizeQuads", CollisionComponent->CollisionSizeQuads);
		coll_info->SetStringField("HeightfieldGuid", CollisionComponent->HeightfieldGuid.ToString());
		coll_info->SetStringField("Transform", CollisionComponent->GetComponentTransform().ToString());
		CollisionInfoArray.Add(MakeShareable(new FJsonValueObject(coll_info)));
		
	}
	land_info->SetArrayField("collions", CollisionInfoArray);

	LandInfoArray.Add(MakeShareable(new FJsonValueObject(land_info)));
}

bool FExportChaosEditorModule::ExportPhysicData()
{
	// UWorld* World = GEditor->GetEditorWorldContext(false).World();
	UWorld* World = NULL;

	auto WorldList = GEngine->GetWorldContexts();
	for (int32 i = 0; i < WorldList.Num(); ++i)
	{
		UWorld* local_World = WorldList[i].World();
		if (local_World && UKismetSystemLibrary::IsValid(local_World))
		{
			World = local_World;
			break;
		}
	}
	if (!World) return false;

	FString MapName = World->GetMapName();
	//World->bDebugDrawAllTraceTags = true;
	//FVector Start = FVector(1680, 1240, 60);
	//FVector End = FVector(1680, 1740, 60);
	//FQuat   Rot = FQuat(0, 0, 0, 1);

	//for (TObjectIterator<ALandscape> It; It; ++It)
	//{
	//	if (It->GetWorld() == World)
	//	{
	//		auto Landscape = *It;
	//		auto height_1 = Landscape->GetHeightAtLocation({ -5130.000000,-6500.000000,1360.000000 }, EHeightfieldSource::Complex);
	//		auto height_2 = Landscape->GetHeightAtLocation({ 1600.000000,-6500.000000,0.000000 }, EHeightfieldSource::Complex);
	//		UE_LOG(LogTemp, Warning, TEXT("ID:%d %f %f"), Landscape->GetUniqueID(), height_1.GetValue(), height_2.GetValue());
	//	}
	//}
	
	//if(true)
	//{
	//	static FCollisionShape CollisionShape = FCollisionShape::MakeCapsule(DEFAULT_CAPSULE_RADIUS, DEFAULT_CAPSULE_HALFHEIGHT);

	//	ECollisionChannel                         TraceChannel = ECC_WorldStatic;
	//	const struct FCollisionQueryParams& Params = FCollisionQueryParams::DefaultQueryParam;
	//	const struct FCollisionResponseParams& ResponseParams = FCollisionResponseParams::DefaultResponseParam;
	//	const struct FCollisionObjectQueryParams& ObjectParams = FCollisionObjectQueryParams::DefaultObjectQueryParam;
	//	struct FHitResult                         OutHit;
	//	if (FPhysicsInterface::GeomSweepSingle(World,
	//		CollisionShape,
	//		Rot,
	//		OutHit,
	//		Start,
	//		End,
	//		TraceChannel,
	//		Params,
	//		ResponseParams,
	//		ObjectParams) == true)
	//	{
	//		UE_LOG(LogTemp, Warning, TEXT("XXXXXXX"));
	//	}
	//	else
	//	{
	//		UE_LOG(LogTemp, Warning, TEXT("YYYYYYY"));
	//	}

	//}

	struct BodySetupData
	{
		std::vector<UStaticMeshComponent*> static_mesh;
		std::vector<UInstancedStaticMeshComponent*> instanced_static_mesh;
	};
	std::unordered_map<UBodySetup*, BodySetupData> BodySetupMap;
	
	struct SaveConstraintData
	{
		uint32_t OwnerID = 0;
		uint32_t CompID = 0;
		uint32_t ActorID1 = 0;
		uint32_t ActorID2 = 0;
		FTransform Transform;
		FConstraintProfileProperties profile;
	};
	
	std::vector<SaveConstraintData> ConstraintDataSet;

	struct SavePhysicFieldData
	{
		uint32_t OwnerID = 0;
		uint32_t CompID = 0;
		FTransform Transform;
		FVector Direction;
		float Magnitude = 1.0f;
		bool bEnable = false;
		uint8_t FieldType = 0;
	};
	std::vector<SavePhysicFieldData> PhysicFieldDataSet;
	
	TArray<TSharedPtr<FJsonValue>> LandInfoArray;
	
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* actor = *It;
		if (actor == nullptr)
			continue;
		if (actor->GetClass()->GetName() == "DirectionalForce_C")
		{
			auto pComponent = actor->FindComponentByClass(UFieldSystemComponent::StaticClass());
			SavePhysicFieldData data;
			data.OwnerID = actor->GetUniqueID();
			data.CompID = (pComponent) ? pComponent->GetUniqueID() : actor->GetUniqueID();
			data.Transform = actor->GetTransform();
			data.bEnable = true;
			data.FieldType = static_cast<uint8_t>(EPhysicFieldType::DirectionalForce);
			//auto FieldSysComp = actor->FindComponentByClass(UFieldSystemComponent::StaticClass());
			PhysicFieldDataSet.emplace_back(std::move(data));
				
			
			continue;
		}
		 
		if (actor->GetClass() == ALandscape::StaticClass())
		{
			FString SavePath = FPaths::ProjectSavedDir() / "Cooked/LinuxServer" / FApp::GetProjectName() / "Content/Landscape" / MapName;
			ALandscape* landscape = Cast<ALandscape>(actor);
			ExportLandscape(SavePath, landscape, LandInfoArray);


			continue;
		}
		

		for (UActorComponent* ActorComponent : actor->GetComponents())
		{
			if (ActorComponent->IsEditorOnly())
				continue;
			
			if (UPrimitiveComponent* PriComponent = Cast<UPrimitiveComponent>(ActorComponent))
			{
				if (PriComponent->IsCollisionEnabled() == false)
					continue;
			}
			
			if (UInstancedStaticMeshComponent* InstancedStaticMeshComponent = Cast<UInstancedStaticMeshComponent>(ActorComponent))
			{
				auto BodySetup = InstancedStaticMeshComponent->GetBodySetup();
				if (BodySetup->AggGeom.GetElementCount() <= 0)
					continue;

				auto& data = BodySetupMap[BodySetup];
				data.instanced_static_mesh.push_back(InstancedStaticMeshComponent);
			}
			else if (UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(ActorComponent))
			{
				auto BodySetup = StaticMeshComponent->GetBodySetup();
				if (BodySetup->AggGeom.GetElementCount() <= 0)
					continue;

				auto& data = BodySetupMap[BodySetup];
				data.static_mesh.push_back(StaticMeshComponent);
			}
			else if (UPhysicsConstraintComponent* ConstraintComponent = Cast<UPhysicsConstraintComponent>(ActorComponent))
			{
				SaveConstraintData cs_data;
				if (ConstraintComponent->ConstraintActor1)
					cs_data.ActorID1 = ConstraintComponent->ConstraintActor1->GetUniqueID();
				else
					cs_data.ActorID1 = actor->GetUniqueID();
				if (ConstraintComponent->ConstraintActor2)
					cs_data.ActorID2 = ConstraintComponent->ConstraintActor2->GetUniqueID();
				else
					cs_data.ActorID2 = actor->GetUniqueID();
				cs_data.OwnerID = actor->GetUniqueID();
				cs_data.CompID = ConstraintComponent->GetUniqueID();
				cs_data.Transform = ConstraintComponent->GetRelativeTransform();
				cs_data.profile = ConstraintComponent->ConstraintInstance.ProfileInstance;
				ConstraintDataSet.emplace_back(std::move(cs_data));
			}
			

		}

	}

	ITargetPlatform* TargetPlatform = GetTargetPlatformManager()->FindTargetPlatform(TEXT("LinuxServer"));
	if (TargetPlatform == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("Should Add TargetPlatfrom LinuxServer"));
		return false;
	}
	/*{
		auto loadPkg = LoadPackage(nullptr, ANSI_TO_TCHAR("/Game/Physic/SM_Cube"), LOAD_None);
		if (loadPkg)
		{
			auto NewBodySetup = LoadObject<UBodySetup>(loadPkg, ANSI_TO_TCHAR("BodySetup_1"));
			check(NewBodySetup);
			if (NewBodySetup)
			{
				check(NewBodySetup->IsAsset());
				check(NewBodySetup->IsCachedCookedPlatformDataLoaded(TargetPlatform));
			}

		}
	}*/

	//save
	{

		//TOptional<FArchiveCookData> CookData;
		//
		//FString ResolvedRootPath = FPaths::ProjectContentDir() / "Physic";
		//FString ResolvedMetadataPath = FPaths::ProjectContentDir() / "Physic"/ "Metadata";
		//
		//auto ZenStoreWriter = new FZenStoreWriter(ResolvedRootPath, ResolvedMetadataPath, TargetPlatform);

		//ICookedPackageWriter::FCookInfo CookInfo;
		//CookInfo.bFullBuild = true;
		//ZenStoreWriter->Initialize(CookInfo);
		//ZenStoreWriter->BeginCook();
		//
		//FSavePackageContext SaveContext(TargetPlatform, ZenStoreWriter);


		FSavePackageArgs SaveArgs;
		//SaveArgs.SavePackageContext = &SaveContext;

		SaveArgs.TopLevelFlags = EObjectFlags::RF_Public;
		//SaveArgs.SaveFlags = ESaveFlags::SAVE_NoError | ESaveFlags::SAVE_FromAutosave;
		SaveArgs.Error = GWarn;

		TArray<FString> PackageNameArray;
		TArray<TSharedPtr<FJsonValue>> JsonBodySetupInfoArray;
		for (const auto& [BodySetup, Data] : BodySetupMap)
		{
			if (BodySetup == nullptr)
				continue;
			FString MeshName = BodySetup->GetOuter()->GetName();
			FString BodySetupName = BodySetup->GetName();
			UE_LOG(LogTemp, Warning, TEXT("BodySetup:%s Guid:%u"), *MeshName, *BodySetup->BodySetupGuid.ToString());

			FString PackageName = TEXT("/Game/Physic/") + MeshName;
			UPackage* SavePkg = CreatePackage(*PackageName);
			SavePkg->ClearFlags(RF_Transient);
			//SavePkg->SetFlags(RF_Standalone);
			//SavePkg->SetPackageFlags(PKG_FilterEditorOnly);
			auto NewBodySetup = FindObject<UBodySetup>(SavePkg, *BodySetup->GetName());
			if (NewBodySetup == nullptr)
			{
				NewBodySetup = DuplicateObject(BodySetup, SavePkg);
			}
			else
			{
				NewBodySetup->CopyBodyPropertiesFrom(BodySetup);
			}

			NewBodySetup->SetFlags(EObjectFlags::RF_Public);
			NewBodySetup->ClearFlags(EObjectFlags::RF_Transient);
			BodySetup->CookedFormatDataOverride = &BodySetup->CookedFormatData;
			check(BodySetup->IsCachedCookedPlatformDataLoaded(TargetPlatform));
			NewBodySetup->bSharedCookedData = true;
			NewBodySetup->CookedFormatData = BodySetup->CookedFormatData;
			NewBodySetup->bUseSavedCookData = true;
			NewBodySetup->AddToRoot();

			FAssetRegistryModule::AssetCreated(NewBodySetup);
			SavePkg->SetDirtyFlag(true);

			/*UPackage::Save(SavePkg, NewBodySetup, *NewBodySetup->GetName(), SaveArgs); */



			FString PackageFileName = FPaths::ProjectContentDir() / "Physic" / MeshName + ".uasset";
			if (IFileManager::Get().FileExists(*PackageFileName))
			{
				IFileManager::Get().Delete(*PackageFileName);
			}
			PackageNameArray.Add(MeshName);
			//FArchiveCookContext CookContext(SavePkg, FArchiveCookContext::ECookTypeUnknown);
			//if (TargetPlatform != nullptr)
			//{
			//	CookData.Emplace(*TargetPlatform, CookContext);
			//}

			//SaveArgs.ArchiveCookData = CookData.GetPtrOrNull();

			//ICookedPackageWriter::FBeginPackageInfo Info;
			//Info.PackageName = SavePkg->GetFName();
			//Info.LooseFilePath = PackageFileName;
			//ZenStoreWriter->BeginPackage(Info);

			UPackage::SavePackage(SavePkg, nullptr, *PackageFileName, SaveArgs);
			/*GIsCookerLoadingPackage = true;
			uint32 SaveFlags = SAVE_KeepGUID | SAVE_Async | SAVE_ComputeHash | SAVE_Unversioned;
			EObjectFlags CookedFlags = RF_Public;

			FSavePackageResultStruct Result = GEditor->Save(SavePkg, nullptr, *PackageFileName, SaveArgs);
			GIsCookerLoadingPackage = false;*/

			UPackage::WaitForAsyncFileWrites();
			/*ICookedPackageWriter::FCommitPackageInfo CommitInfo;
			CommitInfo.Status = IPackageWriter::ECommitStatus::Success;
			CommitInfo.PackageName = SavePkg->GetFName();
			CommitInfo.PackageGuid = FGuid();
			CommitInfo.WriteOptions = IPackageWriter::EWriteOptions::Write | IPackageWriter::EWriteOptions::ComputeHash;

			ZenStoreWriter->CommitPackage(MoveTemp(CommitInfo));

			check(IFileManager::Get().FileExists(*PackageFileName));*/


			TSharedPtr<FJsonObject> bs_info = MakeShareable(new FJsonObject);
			bs_info->SetStringField("Package", MeshName);
			bs_info->SetStringField("Name", BodySetupName);
			bs_info->SetStringField("Path", BodySetup->GetPathName());
			bs_info->SetStringField("Guid", BodySetup->BodySetupGuid.ToString());

			//static mesh
			TArray<TSharedPtr<FJsonValue>> StaticMeshArray;
			for (const auto& StaticMeshComponent : Data.static_mesh)
			{
				auto BodyInstance = StaticMeshComponent->GetBodyInstance();

				TSharedPtr<FJsonObject>  bi_info = MakeShareable(new FJsonObject);
				bi_info->SetNumberField("ActorID", StaticMeshComponent->GetOwner()->GetUniqueID());
				bi_info->SetNumberField("CompID", StaticMeshComponent->GetUniqueID());
				bi_info->SetStringField("Name", StaticMeshComponent->GetOwner()->GetActorLabel(false));
				auto Transform = BodyInstance->GetUnrealWorldTransform_AssumesLocked();
				Transform.SetScale3D(BodyInstance->Scale3D);
				bi_info->SetStringField("Transform", Transform.ToString());
				bi_info->SetBoolField("SimulatePhysics", BodyInstance->bSimulatePhysics);
				TSharedPtr<FJsonObject>  detail_info = MakeShareable(new FJsonObject);
				SaveBodyInstanceDetail(detail_info, BodyInstance);
				bi_info->SetObjectField("Detail", detail_info);


				bi_info->SetBoolField("EnableGravity", BodyInstance->bEnableGravity);
				bi_info->SetBoolField("StartAwake", BodyInstance->bStartAwake);

				bi_info->SetBoolField("Movable", Cast<UPrimitiveComponent>(StaticMeshComponent)->Mobility == EComponentMobility::Movable);
				StaticMeshArray.Add(MakeShareable(new FJsonValueObject(bi_info)));
			}


			//static mesh instance
			TArray<TSharedPtr<FJsonValue>> InstancedStaticMeshArray;
			for (const auto& InstancedStaticMeshComponent : Data.instanced_static_mesh)
			{
				auto BodyInstance = InstancedStaticMeshComponent->GetBodyInstance();

				TSharedPtr<FJsonObject>  bi_info = MakeShareable(new FJsonObject);
				bi_info->SetNumberField("ActorID", InstancedStaticMeshComponent->GetOwner()->GetUniqueID());
				bi_info->SetNumberField("CompID", InstancedStaticMeshComponent->GetUniqueID());
				bi_info->SetStringField("Name", InstancedStaticMeshComponent->GetOwner()->GetActorLabel(false));
				auto Transform = BodyInstance->GetUnrealWorldTransform_AssumesLocked();
				Transform.SetScale3D(BodyInstance->Scale3D);
				bi_info->SetStringField("Transform", Transform.ToString());
				bi_info->SetBoolField("SimulatePhysics", BodyInstance->bSimulatePhysics);
				TSharedPtr<FJsonObject>  detail_info = MakeShareable(new FJsonObject);
				SaveBodyInstanceDetail(detail_info, BodyInstance);
				bi_info->SetObjectField("Detail", detail_info);

				bi_info->SetBoolField("EnableGravity", BodyInstance->bEnableGravity);
				bi_info->SetBoolField("StartAwake", BodyInstance->bStartAwake);

				bi_info->SetBoolField("Movable", Cast<UPrimitiveComponent>(InstancedStaticMeshComponent)->Mobility == EComponentMobility::Movable);
				TArray<TSharedPtr<FJsonValue>> InstancedTransformArray;
				const auto& instanc_array = InstancedStaticMeshComponent->InstanceBodies;
				for (const auto& inst : instanc_array)
				{
					TSharedPtr<FJsonObject>  inst_tm_json = MakeShareable(new FJsonObject);
					//bi_info->SetStringField("Name", BodyInstance->GetName());
					auto inst_Transform = inst->GetUnrealWorldTransform_AssumesLocked();
					inst_Transform.SetScale3D(inst->Scale3D);
					inst_tm_json->SetStringField("Transform", inst_Transform.ToString());

					InstancedTransformArray.Add(MakeShareable(new FJsonValueObject(inst_tm_json)));
				}
				bi_info->SetArrayField("InstancesTM", InstancedTransformArray);
				InstancedStaticMeshArray.Add(MakeShareable(new FJsonValueObject(bi_info)));
			}

			if (StaticMeshArray.Num() > 0)
			{
				bs_info->SetArrayField("StaticMesh", StaticMeshArray);
			}
			if (InstancedStaticMeshArray.Num() > 0)
			{
				bs_info->SetArrayField("StaticMeshInstance", InstancedStaticMeshArray);
			}

			JsonBodySetupInfoArray.Add(MakeShareable(new FJsonValueObject(bs_info)));


		}

		if (PackageNameArray.Num())
		{
			FString PackageAllName = FString::JoinBy(PackageNameArray, TEXT("+"), [](auto v) {return "/Game/Physic/" + v; });

			const FString EditorBinary = FPlatformProcess::ExecutablePath();
			const FString Project = FPaths::SetExtension(FPaths::Combine(FPaths::ProjectDir(), FApp::GetProjectName()), ".uproject");
			const FString CmdParams = "\"" + Project + "\"" + " -run=Cook  -TargetPlatform=LinuxServer -iterate -ddc=DerivedDataBackendGraph -unversioned -fileopenlog -stdout -CrashForUAT -unattended -NoLogTimes  -UTF8Output -cooksinglepackagenorefs -NoGameAlwaysCook -PACKAGE=" + PackageAllName;
			UE_LOG(LogTemp, Warning, TEXT("RUN CMD:%s %s"), *EditorBinary, *CmdParams);

			void* PipeRead = nullptr;
			void* PipeWrite = nullptr;
			verify(FPlatformProcess::CreatePipe(PipeRead, PipeWrite));

			FProcHandle WorkerHandle = FPlatformProcess::CreateProc(*EditorBinary, *CmdParams, false, false, false, nullptr, 0, nullptr, PipeWrite, nullptr);

			TArray<uint8> BinaryFileContent;
			while (FPlatformProcess::IsProcRunning(WorkerHandle))
			{
				FPlatformProcess::Sleep(0);

				TArray<uint8> BinaryData;
				FPlatformProcess::ReadPipeToArray(PipeRead, BinaryData);
				if (BinaryData.Num() > 0)
				{
					BinaryFileContent.Append(MoveTemp(BinaryData));
				}
			}

			TArray<uint8> BinaryData;
			FPlatformProcess::ReadPipeToArray(PipeRead, BinaryData);
			if (BinaryData.Num() > 0)
			{
				BinaryFileContent.Append(MoveTemp(BinaryData));
			}
			BinaryFileContent.Add(0);

			int32 ExitCode = 0xffffffff;
			bool GotReturnCode = FPlatformProcess::GetProcReturnCode(WorkerHandle, &ExitCode);
			FPlatformProcess::CloseProc(WorkerHandle);

			if(ExitCode == 0)
			{
				UE_LOG(LogTemp, Log, TEXT("%s"), ANSI_TO_TCHAR((char*)BinaryFileContent.GetData()));
			}
			else
			{
				UE_LOG(LogTemp, Error, TEXT("exit code: %d\n%s"), ExitCode, ANSI_TO_TCHAR((char*)BinaryFileContent.GetData()));
			}

			FPlatformProcess::ClosePipe(PipeRead, PipeWrite);

			if (ExitCode == 0)
			{
				for (const auto& PackageName : PackageNameArray)
				{
					FString PackageFileName = FPaths::ProjectContentDir() / "Physic" / PackageName + ".uasset";
					if (IFileManager::Get().FileExists(*PackageFileName))
					{
						IFileManager::Get().Delete(*PackageFileName);
					}
				}
			}
		}

		TArray<TSharedPtr<FJsonValue>> JsonConstraintsInfoArray;
		for (const auto& data : ConstraintDataSet)
		{
			TSharedPtr<FJsonObject> cs_info = MakeShareable(new FJsonObject);
			cs_info->SetNumberField("OwnerID", data.OwnerID);
			cs_info->SetNumberField("CompID", data.CompID);
			cs_info->SetNumberField("ActorID1", data.ActorID1);
			cs_info->SetNumberField("ActorID2", data.ActorID2);
			cs_info->SetStringField("Transform", data.Transform.ToString());
			
			TSharedRef<FJsonObject> JsonConstraintProfile = MakeShareable(new FJsonObject);
			if (FJsonObjectConverter::UStructToJsonObject(FConstraintProfileProperties::StaticStruct(), &data.profile, JsonConstraintProfile, 0, 0))
			{
				cs_info->SetObjectField("Profile", JsonConstraintProfile);
			}
			
			JsonConstraintsInfoArray.Add(MakeShareable(new FJsonValueObject(cs_info)));
		}

		TArray<TSharedPtr<FJsonValue>> JsonPhysicFieldInfoArray;
		for (const auto& data : PhysicFieldDataSet)
		{
			TSharedPtr<FJsonObject> cs_info = MakeShareable(new FJsonObject);
			cs_info->SetNumberField("OwnerID", data.OwnerID);
			cs_info->SetNumberField("CompID", data.CompID);
			cs_info->SetBoolField("Enable", data.bEnable);
			cs_info->SetNumberField("PhysicFieldType", data.FieldType);
			
			cs_info->SetStringField("Transform", data.Transform.ToString());
			cs_info->SetStringField("Direction", data.Direction.ToString());
			cs_info->SetNumberField("Magnitude", data.Magnitude);
			JsonPhysicFieldInfoArray.Add(MakeShareable(new FJsonValueObject(cs_info)));
		}
		
		//ZenStoreWriter->EndCook();
		TSharedRef<FJsonObject> JsonRootObject = MakeShareable(new FJsonObject());
		//JsonRootObject->SetStringField("PackageFile", PackageName);
		JsonRootObject->SetArrayField("BodySetups", JsonBodySetupInfoArray);
		JsonRootObject->SetArrayField("Constraints", JsonConstraintsInfoArray);
		JsonRootObject->SetArrayField("PhysicFields", JsonPhysicFieldInfoArray);
		JsonRootObject->SetArrayField("Landscapes", LandInfoArray);
		
		JsonRootObject->SetStringField("MapName", MapName);
		auto JsonTxt = JsonObjToJsonStr(JsonRootObject);

		FString JsonFilePath = FPaths::ProjectSavedDir() / "Cooked/LinuxServer" / FApp::GetProjectName() / "Content/PhysicScene/" + MapName +".json";
		//保存json
		FFileHelper::SaveStringToFile(JsonTxt, *JsonFilePath);

		FText ChaosSuccMsg = LOCTEXT("SaveChaosMeshMesh", "Successd to Export the ChaosMesh.");
		CreateSaveFileNotify(ChaosSuccMsg, JsonFilePath);

		//FString PackageFileName = FPaths::ProjectContentDir() / "DumpBodySetup.uasset";
		////FString PackageFileName = "/Game/DumpBodySetup";
		//UPackage::SavePackage(SavePkg, nullptr, *PackageFileName, SaveArgs);
		//UPackage::WaitForAsyncFileWrites();
		//check(IFileManager::Get().FileExists(*PackageFileName));



		//TArray<ITargetPlatform*> TargetPlatforms;
		//TargetPlatforms.Add(TargetPlatform);
		//TArray<FString> CookedMaps;
		//TArray<FString> CookDirectories;
		//TArray<FString> CookCultures;
		//TArray<FString> IniMapSections;
		//GEditor->StartCookByTheBookInEditor(TargetPlatforms, CookedMaps, CookDirectories, CookCultures, IniMapSections);
	}


	return true;
}


void FExportChaosEditorModule::AddMenuExtension(FMenuBuilder& Builder)
{
	Builder.AddMenuEntry(FExportChaosCommands::Get().PluginAction);
}



void FExportChaosEditorModule::AddToolbarExtension(FToolBarBuilder& Builder)
{
	Builder.AddToolBarButton(FExportChaosCommands::Get().PluginAction);
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FExportChaosEditorModule, ExportChaosEditor)
