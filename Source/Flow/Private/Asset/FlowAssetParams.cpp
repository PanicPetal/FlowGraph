// Copyright https://github.com/MothCocoon/FlowGraph/graphs/contributors

#include "Asset/FlowAssetParams.h"
#include "FlowAsset.h"
#include "FlowLogChannels.h"
#include "Asset/FlowAssetParamsUtils.h"
#include "Types/FlowDataPinValuesStandard.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#include "UObject/ObjectSaveContext.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(FlowAssetParams)

#if WITH_EDITOR
void UFlowAssetParams::PostLoad()
{
	Super::PostLoad();

	if (!HasAnyFlags(RF_ArchetypeObject | RF_ClassDefaultObject))
	{
		// Migrate the named properties over to the new structs

		bool bMadeAnyChanges = false;
		for (FFlowNamedDataPinProperty& NamedProperty : Properties)
		{
			bMadeAnyChanges |= NamedProperty.FixupDataPinProperty();
		}
		
		if (bMadeAnyChanges)
		{
			ModifyAndRebuildPropertiesMap();
		}
	}

	const EFlowReconcilePropertiesResult ReconcileResult = ReconcilePropertiesWithParentParams();
	if (EFlowReconcilePropertiesResult_Classifiers::IsErrorResult(ReconcileResult))
	{
		UE_LOG(LogFlow, Error, TEXT("Failed to reconcile ParentParams during PostLoad() for %s: %s"),
			*GetPathName(), *UEnum::GetDisplayValueAsText(ReconcileResult).ToString());
	}
}

void UFlowAssetParams::PreSaveRoot(FObjectPreSaveRootContext ObjectSaveContext)
{
	Super::PreSaveRoot(ObjectSaveContext);

	const EFlowReconcilePropertiesResult ReconcileResult = ReconcilePropertiesWithParentParams();
	if (EFlowReconcilePropertiesResult_Classifiers::IsErrorResult(ReconcileResult))
	{
		UE_LOG(LogFlow, Error, TEXT("Failed to reconcile ParentParams during PreSaveRoot() for %s: %s"),
			*GetPathName(), *UEnum::GetDisplayValueAsText(ReconcileResult).ToString());
	}
}
#endif

void UFlowAssetParams::Serialize(FArchive& Ar)
{
#if WITH_EDITOR
	if (Ar.IsCooking())
	{
		const EFlowReconcilePropertiesResult ReconcileResult = ReconcilePropertiesWithParentParams();
		if (EFlowReconcilePropertiesResult_Classifiers::IsErrorResult(ReconcileResult))
		{
			UE_LOG(LogFlow, Error, TEXT("Failed to reconcile ParentParams during cooking for %s: %s"),
				*GetPathName(), *UEnum::GetDisplayValueAsText(ReconcileResult).ToString());
		}
	}

#endif
	Super::Serialize(Ar);
}

UFlowAsset* UFlowAssetParams::ProvideFlowAsset() const
{
#if WITH_EDITOR
	return OwnerFlowAsset.LoadSynchronous();
#else
	// We don't have knowledge of the OwnerFlowAsset in non-editor builds
	checkNoEntry();
	return nullptr;
#endif
}

#if WITH_EDITOR
EDataValidationResult UFlowAssetParams::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);

	if (OwnerFlowAsset.IsNull())
	{
		Context.AddError(FText::FromString(TEXT("OwnerFlowAsset is null")));
		Result = CombineDataValidationResults(Result, EDataValidationResult::Invalid);
	}
	else if (!OwnerFlowAsset.IsValid() && !OwnerFlowAsset.LoadSynchronous())
	{
		Context.AddError(FText::FromString(FString::Printf(TEXT("Failed to load OwnerFlowAsset: %s"), *OwnerFlowAsset.ToString())));
		Result = CombineDataValidationResults(Result, EDataValidationResult::Invalid);
	}

	const EFlowReconcilePropertiesResult CycleResult = CheckForParentCycle();
	if (EFlowReconcilePropertiesResult_Classifiers::IsErrorResult(CycleResult))
	{
		Context.AddError(FText::FromString(TEXT("Cyclic inheritance detected")));
		Result = CombineDataValidationResults(Result, EDataValidationResult::Invalid);
	}

	TSet<FGuid> SeenGuids;
	for (int32 Index = 0; Index < Properties.Num(); ++Index)
	{
		const FFlowNamedDataPinProperty& Property = Properties[Index];
		if (Property.Name == NAME_None)
		{
			Context.AddError(FText::FromString(FString::Printf(TEXT("Property at index %d has invalid name"), Index)));
			Result = CombineDataValidationResults(Result, EDataValidationResult::Invalid);
		}

		if (!Property.DataPinValue.IsValid())
		{
			Context.AddError(FText::FromString(FString::Printf(TEXT("Property at index %d has invalid DataPinValue"), Index)));
			Result = CombineDataValidationResults(Result, EDataValidationResult::Invalid);
		}

		if (!Property.Guid.IsValid())
		{
			Context.AddError(FText::FromString(FString::Printf(TEXT("Property at index %d has invalid Guid"), Index)));
			Result = CombineDataValidationResults(Result, EDataValidationResult::Invalid);
		}
		else if (SeenGuids.Contains(Property.Guid))
		{
			Context.AddError(FText::FromString(FString::Printf(TEXT("Duplicate Guid found for property at index %d"), Index)));
			Result = CombineDataValidationResults(Result, EDataValidationResult::Invalid);
		}
		else
		{
			SeenGuids.Add(Property.Guid);
		}

		if (Property.bMayChangeNameAndType)
		{
			Context.AddError(FText::FromString(FString::Printf(TEXT("Property at index %d has bMayChangeNameAndType = true in UFlowAssetParams"), Index)));
			Result = CombineDataValidationResults(Result, EDataValidationResult::Invalid);
		}
	}

	return Result;
}

EFlowReconcilePropertiesResult UFlowAssetParams::ReconcilePropertiesWithStartNode(
	const FDateTime& FlowAssetLastSaveTimeStamp,
	const TSoftObjectPtr<UFlowAsset>& InOwnerFlowAsset,
	TArray<FFlowNamedDataPinProperty>& MutablePropertiesFromStartNode)
{
	if (InOwnerFlowAsset.IsNull())
	{
		return EFlowReconcilePropertiesResult::Error_InvalidAsset;
	}

	// LOCAL MOD: OwnerFlowAsset is (re)assigned below, inside the change detection, not unconditionally here.
	const EFlowReconcilePropertiesResult PropertiesMatchResult = FFlowAssetParamsUtils::CheckPropertiesMatch(Properties, MutablePropertiesFromStartNode);
	const FDateTime ParamsTimestamp = FFlowAssetParamsUtils::GetLastSavedTimestampForObject(this);

	if (FlowAssetLastSaveTimeStamp >= ParamsTimestamp ||
		EFlowReconcilePropertiesResult_Classifiers::IsErrorResult(PropertiesMatchResult))
	{
		// LOCAL MOD: the Start node is authoritative here, but this runs on every PostLoad of the owning
		// Flow asset, so only touch the transaction buffer / dirty the package when the result actually
		// differs from what is already stored. Otherwise merely opening the graph dirties the params asset.
		TArray<FFlowNamedDataPinProperty> NewProperties = MutablePropertiesFromStartNode;
		FFlowNamedDataPinProperty::ConfigurePropertiesForFlowAssetParams(NewProperties);

		const bool bReferencesChanged = !ParentParams.AssetPtr.IsNull() || !(OwnerFlowAsset == InOwnerFlowAsset);
		if (bReferencesChanged)
		{
			Modify();
			ParentParams.AssetPtr = nullptr;
			OwnerFlowAsset = InOwnerFlowAsset;
		}

		const bool bPropertiesChanged = ApplyPropertiesIfChanged(MoveTemp(NewProperties));
		if (bReferencesChanged && !bPropertiesChanged)
		{
			MarkPackageDirty();
		}

		return (bReferencesChanged || bPropertiesChanged)
			? EFlowReconcilePropertiesResult::ParamsPropertiesUpdated
			: EFlowReconcilePropertiesResult::NoChanges;
	}

	if (!(OwnerFlowAsset == InOwnerFlowAsset))
	{
		Modify();
		OwnerFlowAsset = InOwnerFlowAsset;
		MarkPackageDirty();
	}

	MutablePropertiesFromStartNode = Properties;

	// LOCAL MOD: these properties live on the Start node, so they take the Start node configuration
	// (bMayChangeNameAndType = true); the params configuration would lock renaming in the graph.
	FFlowNamedDataPinProperty::ConfigurePropertiesForFlowAssetStartNode(MutablePropertiesFromStartNode);

	return EFlowReconcilePropertiesResult::AssetPropertyValuesUpdated;
}

EFlowReconcilePropertiesResult UFlowAssetParams::ReconcilePropertiesWithParentParams()
{
	const EFlowReconcilePropertiesResult CycleResult = CheckForParentCycle();
	if (EFlowReconcilePropertiesResult_Classifiers::IsErrorResult(CycleResult))
	{
		return CycleResult;
	}

	if (ParentParams.AssetPtr.IsNull())
	{
		return EFlowReconcilePropertiesResult::NoChanges;
	}

	UFlowAssetParams* Parent = ParentParams.AssetPtr.LoadSynchronous();
	if (!Parent)
	{
		UE_LOG(LogFlow, Warning, TEXT("Failed to load ParentParams: %s"), *ParentParams.AssetPtr.ToString());

		return EFlowReconcilePropertiesResult::Error_UnloadableParent;
	}

	const EFlowReconcilePropertiesResult ParentResult = Parent->ReconcilePropertiesWithParentParams();
	if (EFlowReconcilePropertiesResult_Classifiers::IsErrorResult(ParentResult))
	{
		return ParentResult;
	}

	const TArray<FFlowNamedDataPinProperty>& ParentProps = Parent->Properties;
	TArray<FFlowNamedDataPinProperty> NewProperties;

	for (const FFlowNamedDataPinProperty& ParentProp : ParentProps)
	{
		FFlowNamedDataPinProperty* LocalProp = FFlowAssetParamsUtils::FindPropertyByGuid(Properties, ParentProp.Guid);
		if (LocalProp != nullptr)
		{
			// We have a version of ParentProp locally.
			// Determine if our local property has been modified since our last reconcile.
			// A local property is considered modified if we've never added it to PropertyMap or is different from what currently exists in PropertyMap.
			bool bLocalPropHasChanged = true;
			if (PropertyMap.Contains(LocalProp->Name))
			{
				FFlowNamedDataPinProperty PreviousLocalProp = *LocalProp;
				PreviousLocalProp.DataPinValue = PropertyMap[LocalProp->Name];
				bLocalPropHasChanged = !FFlowAssetParamsUtils::ArePropertiesEqual(*LocalProp, PreviousLocalProp);
			}

			if (bLocalPropHasChanged)
			{
				// If the local property has been changed then compare it to the parent value to determine if it is an override or not.
				if (FFlowAssetParamsUtils::ArePropertiesEqual(*LocalProp, ParentProp))
				{
					FFlowNamedDataPinProperty& NewProp = NewProperties.Add_GetRef(ParentProp);
					NewProp.bIsOverride = false;
				}
				else
				{
					FFlowNamedDataPinProperty& NewProp = NewProperties.Add_GetRef(*LocalProp);
					NewProp.Name = ParentProp.Name;
					NewProp.bIsOverride = true;
				}
			}
			else
			{
				// If the local property has not been changed then check whether it is an override.
				// Overrides will get copied over while non-overrides will be updated to match the parent.
				if (LocalProp->bIsOverride)
				{
					FFlowNamedDataPinProperty& NewProp = NewProperties.Add_GetRef(*LocalProp);
					NewProp.Name = ParentProp.Name;
					NewProp.bIsOverride = true;
				}
				else
				{
					FFlowNamedDataPinProperty& NewProp = NewProperties.Add_GetRef(ParentProp);
					NewProp.bIsOverride = false;
				}
			}
		}
		else
		{
			// We do not have a version of ParentProp. Just make a non-override copy.
			FFlowNamedDataPinProperty& NewProp = NewProperties.Add_GetRef(ParentProp);
			NewProp.bIsOverride = false;
		}
	}

	for (const FFlowNamedDataPinProperty& LocalProp : Properties)
	{
		if (!FFlowAssetParamsUtils::FindPropertyByGuid(ParentProps, LocalProp.Guid))
		{
			FFlowNamedDataPinProperty& NewProp = NewProperties.Add_GetRef(LocalProp);
			NewProp.bIsOverride = true;
		}
	}

	// LOCAL MOD: this runs from PostLoad/PreSaveRoot/cook; only dirty the package when the flattened
	// result actually differs from what is stored, otherwise loading a child params asset dirties it.
	if (!ApplyPropertiesIfChanged(MoveTemp(NewProperties)))
	{
		return EFlowReconcilePropertiesResult::NoChanges;
	}

	return EFlowReconcilePropertiesResult::ParamsPropertiesUpdated;
}

void UFlowAssetParams::ConfigureFlowAssetParams(TSoftObjectPtr<UFlowAsset> OwnerAsset, TSoftObjectPtr<UFlowAssetParams> InParentParams, const TArray<FFlowNamedDataPinProperty>& InProperties)
{
	ParentParams.AssetPtr = InParentParams;
	OwnerFlowAsset = OwnerAsset;
	Properties = InProperties;
	FFlowNamedDataPinProperty::ConfigurePropertiesForFlowAssetParams(Properties);

	ModifyAndRebuildPropertiesMap();
}

bool UFlowAssetParams::CanModifyFlowDataPinType() const
{
	// These are set by the Flow asset, which is authoritative
	return false;
}

bool UFlowAssetParams::ShowFlowDataPinValueInputPinCheckbox() const
{
	// These are set by the Flow asset, which is authoritative
	return false;
}

bool UFlowAssetParams::ShowFlowDataPinValueClassFilter(const FFlowDataPinValue* Value) const
{
	return true;
}

bool UFlowAssetParams::CanEditFlowDataPinValueClassFilter(const FFlowDataPinValue* Value) const
{
	// These are set by the Flow asset, which is authoritative
	return false;
}

EFlowReconcilePropertiesResult UFlowAssetParams::CheckForParentCycle() const
{
	TSet<TSoftObjectPtr<UFlowAssetParams>> Visited;
	TSoftObjectPtr<UFlowAssetParams> Current = ParentParams.AssetPtr;

	while (!Current.IsNull())
	{
		if (Visited.Contains(Current))
		{
			UE_LOG(LogFlow, Error, TEXT("Cyclic inheritance detected at: %s"), *Current.ToString());
			return EFlowReconcilePropertiesResult::Error_CyclicInheritance;
		}

		Visited.Add(Current);
		const UFlowAssetParams* CurrentParams = Current.LoadSynchronous();
		if (!CurrentParams)
		{
			UE_LOG(LogFlow, Warning, TEXT("Failed to load ParentParams: %s"), *Current.ToString());
			return EFlowReconcilePropertiesResult::Error_UnloadableParent;
		}

		Current = CurrentParams->ParentParams.AssetPtr;
	}

	return EFlowReconcilePropertiesResult::NoChanges;
}

void UFlowAssetParams::ModifyAndRebuildPropertiesMap()
{
	Modify();

	RebuildPropertiesMap();

	MarkPackageDirty();
}

void UFlowAssetParams::RebuildPropertiesMap()
{
	PropertyMap = BuildPropertiesMap(Properties);
}

// LOCAL MOD: change-detecting variants used by the reconcile paths, which run on every asset load.

TMap<FName, TInstancedStruct<FFlowDataPinValue>> UFlowAssetParams::BuildPropertiesMap(const TArray<FFlowNamedDataPinProperty>& InProperties) const
{
	TMap<FName, TInstancedStruct<FFlowDataPinValue>> NewMap;
	NewMap.Reserve(InProperties.Num());

	for (const FFlowNamedDataPinProperty& Prop : InProperties)
	{
		if (Prop.IsValid())
		{
			NewMap.Add(Prop.Name, Prop.DataPinValue);
		}
		else
		{
			UE_LOG(LogFlow, Warning, TEXT("Skipping invalid property %s during rebuild for %s"), *Prop.Name.ToString(), *GetPathName());
		}
	}

	return NewMap;
}

bool UFlowAssetParams::AreNamedPropertiesIdentical(const TArray<FFlowNamedDataPinProperty>& A, const TArray<FFlowNamedDataPinProperty>& B)
{
	// ArePropertyArraysEqual compares name, guid, struct type and value; the editor flags are
	// part of the stored state too, so a flag flip alone must still count as a change.
	if (!FFlowAssetParamsUtils::ArePropertyArraysEqual(A, B))
	{
		return false;
	}

	for (int32 Index = 0; Index < A.Num(); ++Index)
	{
		if (A[Index].bIsOverride != B[Index].bIsOverride ||
			A[Index].bMayChangeNameAndType != B[Index].bMayChangeNameAndType)
		{
			return false;
		}
	}

	return true;
}

bool UFlowAssetParams::ApplyPropertiesIfChanged(TArray<FFlowNamedDataPinProperty>&& NewProperties)
{
	TMap<FName, TInstancedStruct<FFlowDataPinValue>> NewPropertyMap = BuildPropertiesMap(NewProperties);

	const bool bChanged =
		!AreNamedPropertiesIdentical(Properties, NewProperties) ||
		!PropertyMap.OrderIndependentCompareEqual(NewPropertyMap);

	if (!bChanged)
	{
		return false;
	}

	Modify();

	Properties = MoveTemp(NewProperties);
	PropertyMap = MoveTemp(NewPropertyMap);

	MarkPackageDirty();

	return true;
}
#endif

bool UFlowAssetParams::CanSupplyDataPinValues() const
{
	return !PropertyMap.IsEmpty();
}

FFlowDataPinResult UFlowAssetParams::TrySupplyDataPin(const FName PinName) const
{
	if (const TInstancedStruct<FFlowDataPinValue>* Found = PropertyMap.Find(PinName))
	{
		FFlowDataPinResult DataPinResult(EFlowDataPinResolveResult::Success);
		DataPinResult.ResultValue = (*Found);

		return DataPinResult;
	}

	return FFlowDataPinResult(EFlowDataPinResolveResult::FailedUnknownPin);
}
