// Copyright Epic Games, Inc. All Rights Reserved.

#include "GameFramework/GameplayMessageSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/ScriptMacros.h"
#include "UObject/Stack.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GameplayMessageSubsystem)

DEFINE_LOG_CATEGORY(LogGameplayMessageSubsystem);

namespace UE
{
	namespace GameplayMessageSubsystem
	{
		static int32 ShouldLogMessages = 0;
		static FAutoConsoleVariableRef CVarShouldLogMessages(TEXT("GameplayMessageSubsystem.LogMessages"),
			ShouldLogMessages,
			TEXT("Should messages broadcast through the gameplay message subsystem be logged?"));
	}
}

//////////////////////////////////////////////////////////////////////
// FGameplayMessageListenerHandle

void FGameplayMessageListenerHandle::Unregister()
{
	if (UGameplayMessageSubsystem* StrongSubsystem = Subsystem.Get())
	{
		StrongSubsystem->UnregisterListener(*this);
	}
	Subsystem.Reset();
	Channel = FGameplayTag();
	ID = 0;
	Target.Reset();
	bTargeted = false;
}

//////////////////////////////////////////////////////////////////////
// UGameplayMessageSubsystem

UGameplayMessageSubsystem& UGameplayMessageSubsystem::Get(const UObject* WorldContextObject)
{
	UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::Assert);
	check(World);
	UGameplayMessageSubsystem* Router = UGameInstance::GetSubsystem<UGameplayMessageSubsystem>(World->GetGameInstance());
	check(Router);
	return *Router;
}

bool UGameplayMessageSubsystem::HasInstance(const UObject* WorldContextObject)
{
	UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::Assert);
	UGameplayMessageSubsystem* Router = World != nullptr ? UGameInstance::GetSubsystem<UGameplayMessageSubsystem>(World->GetGameInstance()) : nullptr;
	return Router != nullptr;
}

void UGameplayMessageSubsystem::Deinitialize()
{
	ListenerMap.Reset();

	for (auto& Pair : TargetedListenerMap)
	{
		if (AActor* Target = Pair.Key.ResolveObjectPtr())
		{
			Target->OnEndPlay.RemoveDynamic(this, &UGameplayMessageSubsystem::HandleTargetEndPlay);
		}
	}
	TargetedListenerMap.Reset();

	Super::Deinitialize();
}

void UGameplayMessageSubsystem::LogBroadcast(const AActor* Target, FGameplayTag Channel, const UScriptStruct* StructType, const void* MessageBytes) const
{
	if (UE::GameplayMessageSubsystem::ShouldLogMessages == 0)
	{
		return;
	}

	FString* pContextString = nullptr;
#if WITH_EDITOR
	if (GIsEditor)
	{
		extern ENGINE_API FString GPlayInEditorContextString;
		pContextString = &GPlayInEditorContextString;
	}
#endif

	FString HumanReadableMessage;
	StructType->ExportText(/*out*/ HumanReadableMessage, MessageBytes, /*Defaults=*/ nullptr, /*OwnerObject=*/ nullptr, PPF_None, /*ExportRootScope=*/ nullptr);
	if (Target != nullptr)
	{
		UE_LOG(LogGameplayMessageSubsystem, Log, TEXT("BroadcastMessageTo(%s, Target=%s, %s, %s)"), pContextString ? **pContextString : *GetPathNameSafe(this), *GetNameSafe(Target), *Channel.ToString(), *HumanReadableMessage);
	}
	else
	{
		UE_LOG(LogGameplayMessageSubsystem, Log, TEXT("BroadcastMessage(%s, %s, %s)"), pContextString ? **pContextString : *GetPathNameSafe(this), *Channel.ToString(), *HumanReadableMessage);
	}
}

void UGameplayMessageSubsystem::DispatchMessage(TFunctionRef<FChannelListenerList*(FGameplayTag)> FindList, TFunctionRef<void(FGameplayTag, int32)> RemoveInvalidListener,
	FGameplayTag Channel, const UScriptStruct* StructType, const void* MessageBytes)
{
	bool bOnInitialTag = true;
	for (FGameplayTag Tag = Channel; Tag.IsValid(); Tag = Tag.RequestDirectParent())
	{
		if (const FChannelListenerList* pList = FindList(Tag))
		{
			// Copy in case there are removals while handling callbacks
			TArray<FGameplayMessageListenerData> ListenerArray(pList->Listeners);

			for (const FGameplayMessageListenerData& Listener : ListenerArray)
			{
				if (bOnInitialTag || (Listener.MatchType == EGameplayMessageMatch::PartialMatch))
				{
					if (Listener.bHadValidType && !Listener.ListenerStructType.IsValid())
					{
						UE_LOG(LogGameplayMessageSubsystem, Warning, TEXT("Listener struct type has gone invalid on Channel %s. Removing listener from list"), *Channel.ToString());
						RemoveInvalidListener(Tag, Listener.HandleID);
						continue;
					}

					// The receiving type must be either a parent of the sending type or completely ambiguous (for internal use)
					if (!Listener.bHadValidType || StructType->IsChildOf(Listener.ListenerStructType.Get()))
					{
						Listener.ReceivedCallback(Channel, StructType, MessageBytes);
					}
					else
					{
						UE_LOG(LogGameplayMessageSubsystem, Error, TEXT("Struct type mismatch on channel %s (broadcast type %s, listener at %s was expecting type %s)"),
							*Channel.ToString(),
							*StructType->GetPathName(),
							*Tag.ToString(),
							*Listener.ListenerStructType->GetPathName());
					}
				}
			}
		}
		bOnInitialTag = false;
	}
}

void UGameplayMessageSubsystem::BroadcastMessageInternal(FGameplayTag Channel, const UScriptStruct* StructType, const void* MessageBytes)
{
	LogBroadcast(nullptr, Channel, StructType, MessageBytes);

	DispatchMessage(
		[this](FGameplayTag Tag) { return ListenerMap.Find(Tag); },
		[this](FGameplayTag Tag, int32 HandleID) { UnregisterListenerInternal(Tag, HandleID); },
		Channel, StructType, MessageBytes);
}

void UGameplayMessageSubsystem::BroadcastMessageToInternal(AActor* Target, FGameplayTag Channel, const UScriptStruct* StructType, const void* MessageBytes)
{
	if (Target == nullptr)
	{
		UE_LOG(LogGameplayMessageSubsystem, Warning, TEXT("BroadcastMessageTo on channel %s with null Target, message dropped"), *Channel.ToString());
		return;
	}

	LogBroadcast(Target, Channel, StructType, MessageBytes);

	const TObjectKey<AActor> Key(Target);
	if (!TargetedListenerMap.Contains(Key))
	{
		return;
	}

	DispatchMessage(
		[this, Key](FGameplayTag Tag) -> FChannelListenerList*
		{
			// 回调期间可能增删 Target 条目，每次重新查找
			FChannelMap* ChannelMap = TargetedListenerMap.Find(Key);
			return ChannelMap ? ChannelMap->Find(Tag) : nullptr;
		},
		[this, Target](FGameplayTag Tag, int32 HandleID) { UnregisterTargetedListenerInternal(Target, Tag, HandleID); },
		Channel, StructType, MessageBytes);
}

void UGameplayMessageSubsystem::K2_BroadcastMessage(FGameplayTag Channel, const int32& Message)
{
	// This will never be called, the exec version below will be hit instead
	checkNoEntry();
}

DEFINE_FUNCTION(UGameplayMessageSubsystem::execK2_BroadcastMessage)
{
	P_GET_STRUCT(FGameplayTag, Channel);

	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FStructProperty>(nullptr);
	void* MessagePtr = Stack.MostRecentPropertyAddress;
	FStructProperty* StructProp = CastField<FStructProperty>(Stack.MostRecentProperty);

	P_FINISH;

	if (ensure((StructProp != nullptr) && (StructProp->Struct != nullptr) && (MessagePtr != nullptr)))
	{
		P_THIS->BroadcastMessageInternal(Channel, StructProp->Struct, MessagePtr);
	}
}

void UGameplayMessageSubsystem::K2_BroadcastMessageTo(AActor* Target, FGameplayTag Channel, const int32& Message)
{
	// This will never be called, the exec version below will be hit instead
	checkNoEntry();
}

DEFINE_FUNCTION(UGameplayMessageSubsystem::execK2_BroadcastMessageTo)
{
	P_GET_OBJECT(AActor, Target);
	P_GET_STRUCT(FGameplayTag, Channel);

	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FStructProperty>(nullptr);
	void* MessagePtr = Stack.MostRecentPropertyAddress;
	FStructProperty* StructProp = CastField<FStructProperty>(Stack.MostRecentProperty);

	P_FINISH;

	if (ensure((StructProp != nullptr) && (StructProp->Struct != nullptr) && (MessagePtr != nullptr)))
	{
		P_THIS->BroadcastMessageToInternal(Target, Channel, StructProp->Struct, MessagePtr);
	}
}

FGameplayMessageListenerHandle UGameplayMessageSubsystem::RegisterListenerInternal(FGameplayTag Channel, TFunction<void(FGameplayTag, const UScriptStruct*, const void*)>&& Callback, const UScriptStruct* StructType, EGameplayMessageMatch MatchType)
{
	FChannelListenerList& List = ListenerMap.FindOrAdd(Channel);

	FGameplayMessageListenerData& Entry = List.Listeners.AddDefaulted_GetRef();
	Entry.ReceivedCallback = MoveTemp(Callback);
	Entry.ListenerStructType = StructType;
	Entry.bHadValidType = StructType != nullptr;
	Entry.HandleID = ++List.HandleID;
	Entry.MatchType = MatchType;

	return FGameplayMessageListenerHandle(this, Channel, Entry.HandleID);
}

FGameplayMessageListenerHandle UGameplayMessageSubsystem::RegisterTargetedListenerInternal(AActor* Target, FGameplayTag Channel, TFunction<void(FGameplayTag, const UScriptStruct*, const void*)>&& Callback, const UScriptStruct* StructType, EGameplayMessageMatch MatchType)
{
	if (!IsValid(Target))
	{
		UE_LOG(LogGameplayMessageSubsystem, Warning, TEXT("RegisterTargetedListener on channel %s with invalid Target, listener not registered"), *Channel.ToString());
		return FGameplayMessageListenerHandle();
	}

	const TObjectKey<AActor> Key(Target);
	FChannelMap* ChannelMap = TargetedListenerMap.Find(Key);
	if (ChannelMap == nullptr)
	{
		ChannelMap = &TargetedListenerMap.Add(Key);
		// 首次为该 Target 注册时挂 EndPlay，Actor 结束时整表清理
		Target->OnEndPlay.AddUniqueDynamic(this, &UGameplayMessageSubsystem::HandleTargetEndPlay);
	}

	FChannelListenerList& List = ChannelMap->FindOrAdd(Channel);

	FGameplayMessageListenerData& Entry = List.Listeners.AddDefaulted_GetRef();
	Entry.ReceivedCallback = MoveTemp(Callback);
	Entry.ListenerStructType = StructType;
	Entry.bHadValidType = StructType != nullptr;
	Entry.HandleID = ++List.HandleID;
	Entry.MatchType = MatchType;

	return FGameplayMessageListenerHandle(this, Target, Channel, Entry.HandleID);
}

void UGameplayMessageSubsystem::UnregisterListener(FGameplayMessageListenerHandle Handle)
{
	if (Handle.IsValid())
	{
		check(Handle.Subsystem == this);

		if (Handle.bTargeted)
		{
			// Target 已销毁时其条目已随 EndPlay 一并清理
			if (AActor* Target = Handle.Target.Get())
			{
				UnregisterTargetedListenerInternal(Target, Handle.Channel, Handle.ID);
			}
		}
		else
		{
			UnregisterListenerInternal(Handle.Channel, Handle.ID);
		}
	}
	else
	{
		UE_LOG(LogGameplayMessageSubsystem, Warning, TEXT("Trying to unregister an invalid Handle."));
	}
}

void UGameplayMessageSubsystem::RemoveListenerFromChannelMap(FChannelMap& Map, FGameplayTag Channel, int32 HandleID)
{
	if (FChannelListenerList* pList = Map.Find(Channel))
	{
		int32 MatchIndex = pList->Listeners.IndexOfByPredicate([ID = HandleID](const FGameplayMessageListenerData& Other) { return Other.HandleID == ID; });
		if (MatchIndex != INDEX_NONE)
		{
			pList->Listeners.RemoveAtSwap(MatchIndex);
		}

		if (pList->Listeners.Num() == 0)
		{
			Map.Remove(Channel);
		}
	}
}

void UGameplayMessageSubsystem::UnregisterListenerInternal(FGameplayTag Channel, int32 HandleID)
{
	RemoveListenerFromChannelMap(ListenerMap, Channel, HandleID);
}

void UGameplayMessageSubsystem::UnregisterTargetedListenerInternal(AActor* Target, FGameplayTag Channel, int32 HandleID)
{
	const TObjectKey<AActor> Key(Target);
	if (FChannelMap* ChannelMap = TargetedListenerMap.Find(Key))
	{
		RemoveListenerFromChannelMap(*ChannelMap, Channel, HandleID);
		if (ChannelMap->Num() == 0)
		{
			RemoveTargetEntry(Target);
		}
	}
}

void UGameplayMessageSubsystem::RemoveTargetEntry(AActor* Target)
{
	if (TargetedListenerMap.Remove(TObjectKey<AActor>(Target)) > 0 && IsValid(Target))
	{
		Target->OnEndPlay.RemoveDynamic(this, &UGameplayMessageSubsystem::HandleTargetEndPlay);
	}
}

void UGameplayMessageSubsystem::UnregisterAllListenersForTarget(AActor* Target)
{
	if (Target == nullptr)
	{
		return;
	}
	RemoveTargetEntry(Target);
}

void UGameplayMessageSubsystem::HandleTargetEndPlay(AActor* Actor, EEndPlayReason::Type EndPlayReason)
{
	RemoveTargetEntry(Actor);
}
