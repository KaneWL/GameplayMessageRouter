// Copyright Epic Games, Inc. All Rights Reserved.

#include "GameFramework/AsyncAction_ListenForGameplayMessage.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameplayMessageSubsystem.h"
#include "UObject/ScriptMacros.h"
#include "UObject/Stack.h"
#include "UObject/UnrealType.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(AsyncAction_ListenForGameplayMessage)

UAsyncAction_ListenForGameplayMessage* UAsyncAction_ListenForGameplayMessage::ListenForGameplayMessages(UObject* WorldContextObject, FGameplayTag Channel, UScriptStruct* PayloadType, EGameplayMessageMatch MatchType, bool bOnlyTriggerOnce)
{
	UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	if (!World)
	{
		return nullptr;
	}

	UAsyncAction_ListenForGameplayMessage* Action = NewObject<UAsyncAction_ListenForGameplayMessage>();
	Action->WorldPtr = World;
	Action->ChannelToRegister = Channel;
	Action->MessageStructType = PayloadType;
	Action->MessageMatchType = MatchType;
	Action->bOnlyTriggerOnce = bOnlyTriggerOnce;
	Action->RegisterWithGameInstance(World);

	return Action;
}

UAsyncAction_ListenForGameplayMessage* UAsyncAction_ListenForGameplayMessage::ListenForGameplayMessagesOn(UObject* WorldContextObject, AActor* Target, FGameplayTag Channel, UScriptStruct* PayloadType, EGameplayMessageMatch MatchType, bool bOnlyTriggerOnce)
{
	UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	if (!World)
	{
		return nullptr;
	}
	if (!IsValid(Target))
	{
		UE_LOG(LogGameplayMessageSubsystem, Warning, TEXT("ListenForGameplayMessagesOn(%s): Target is invalid, node will not activate"), *Channel.ToString());
		return nullptr;
	}

	UAsyncAction_ListenForGameplayMessage* Action = NewObject<UAsyncAction_ListenForGameplayMessage>();
	Action->WorldPtr = World;
	Action->ChannelToRegister = Channel;
	Action->MessageStructType = PayloadType;
	Action->MessageMatchType = MatchType;
	Action->TargetPtr = Target;
	Action->bTargeted = true;
	Action->bOnlyTriggerOnce = bOnlyTriggerOnce;
	Action->RegisterWithGameInstance(World);

	return Action;
}

UAsyncAction_ListenForGameplayMessage* UAsyncAction_ListenForGameplayMessage::ListenWithFunction(UObject* WorldContextObject, UObject* Owner, FName FunctionName, FGameplayTag Channel, UScriptStruct* PayloadType, EGameplayMessageMatch MatchType)
{
	UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	if (!World || !UGameplayMessageSubsystem::HasInstance(World))
	{
		return nullptr;
	}
	if (!IsValid(Owner) || Owner->FindFunction(FunctionName) == nullptr)
	{
		UE_LOG(LogGameplayMessageSubsystem, Warning, TEXT("ListenWithFunction(%s): Owner 无效或找不到函数 %s"), *Channel.ToString(), *FunctionName.ToString());
		return nullptr;
	}

	UAsyncAction_ListenForGameplayMessage* Action = NewObject<UAsyncAction_ListenForGameplayMessage>();
	Action->WorldPtr = World;
	Action->ChannelToRegister = Channel;
	Action->MessageStructType = PayloadType;
	Action->MessageMatchType = MatchType;
	Action->OwnerPtr = Owner;
	Action->OwnerFunctionName = FunctionName;
	Action->RegisterWithGameInstance(World);

	TWeakObjectPtr<UAsyncAction_ListenForGameplayMessage> WeakThis(Action);
	Action->ListenerHandle = UGameplayMessageSubsystem::Get(World).RegisterListenerInternal(Channel,
		[WeakThis](FGameplayTag Tag, const UScriptStruct* StructType, const void* Payload)
		{
			if (UAsyncAction_ListenForGameplayMessage* StrongThis = WeakThis.Get())
			{
				StrongThis->InvokeOwnerFunction(Tag, StructType, Payload);
			}
		}, PayloadType, MatchType);

	return Action;
}

void UAsyncAction_ListenForGameplayMessage::Activate()
{
	if (UWorld* World = WorldPtr.Get())
	{
		if (UGameplayMessageSubsystem::HasInstance(World))
		{
			UGameplayMessageSubsystem& Router = UGameplayMessageSubsystem::Get(World);

			TWeakObjectPtr<UAsyncAction_ListenForGameplayMessage> WeakThis(this);
			auto Callback = [WeakThis](FGameplayTag Channel, const UScriptStruct* StructType, const void* Payload)
				{
					if (UAsyncAction_ListenForGameplayMessage* StrongThis = WeakThis.Get())
					{
						StrongThis->HandleMessageReceived(Channel, StructType, Payload);
					}
				};

			if (bTargeted)
			{
				AActor* Target = TargetPtr.Get();
				if (Target == nullptr)
				{
					SetReadyToDestroy();
					return;
				}
				ListenerHandle = Router.RegisterTargetedListenerInternal(Target, ChannelToRegister, MoveTemp(Callback), MessageStructType.Get(), MessageMatchType);
			}
			else
			{
				ListenerHandle = Router.RegisterListenerInternal(ChannelToRegister, MoveTemp(Callback), MessageStructType.Get(), MessageMatchType);
			}

			return;
		}
	}

	SetReadyToDestroy();
}

void UAsyncAction_ListenForGameplayMessage::SetReadyToDestroy()
{
	ListenerHandle.Unregister();

	Super::SetReadyToDestroy();
}

bool UAsyncAction_ListenForGameplayMessage::GetPayload(int32& OutPayload)
{
	checkNoEntry();
	return false;
}

DEFINE_FUNCTION(UAsyncAction_ListenForGameplayMessage::execGetPayload)
{
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FStructProperty>(nullptr);
	void* MessagePtr = Stack.MostRecentPropertyAddress;
	FStructProperty* StructProp = CastField<FStructProperty>(Stack.MostRecentProperty);
	P_FINISH;

	bool bSuccess = false;

	// Make sure the type we are trying to get through the blueprint node matches the type of the message payload received.
	if ((StructProp != nullptr) && (StructProp->Struct != nullptr) && (MessagePtr != nullptr) && (StructProp->Struct == P_THIS->MessageStructType.Get()) && (P_THIS->ReceivedMessagePayloadPtr != nullptr))
	{
		StructProp->Struct->CopyScriptStruct(MessagePtr, P_THIS->ReceivedMessagePayloadPtr);
		bSuccess = true;
	}

	*(bool*)RESULT_PARAM = bSuccess;
}

void UAsyncAction_ListenForGameplayMessage::HandleMessageReceived(FGameplayTag Channel, const UScriptStruct* StructType, const void* Payload)
{
	bool bTriggered = false;
	if (!MessageStructType.Get() || (MessageStructType.Get() == StructType))
	{
		ReceivedMessagePayloadPtr = Payload;

		OnMessageReceived.Broadcast(this, Channel);

		ReceivedMessagePayloadPtr = nullptr;
		bTriggered = true;
	}

	if (!OnMessageReceived.IsBound())
	{
		// If the BP object that created the async node is destroyed, OnMessageReceived will be unbound after calling the broadcast.
		// In this case we can safely mark this receiver as ready for destruction.
		// Need to support a more proactive mechanism for cleanup FORT-340994
		SetReadyToDestroy();
	}
	else if (bTriggered && bOnlyTriggerOnce)
	{
		// 单次触发后自动注销
		SetReadyToDestroy();
	}
}

void UAsyncAction_ListenForGameplayMessage::InvokeOwnerFunction(FGameplayTag Channel, const UScriptStruct* StructType, const void* Payload)
{
	UObject* Owner = OwnerPtr.Get();
	if (Owner == nullptr)
	{
		SetReadyToDestroy();
		return;
	}
	UFunction* Func = Owner->FindFunction(OwnerFunctionName);
	if (Func == nullptr)
	{
		return;
	}

	TArray<uint8> Params;
	Params.SetNumZeroed(Func->ParmsSize);
	for (TFieldIterator<FProperty> It(Func); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		It->InitializeValue_InContainer(Params.GetData());
		if (const FStructProperty* StructProp = CastField<FStructProperty>(*It))
		{
			void* Dest = StructProp->ContainerPtrToValuePtr<void>(Params.GetData());
			if (StructProp->Struct == FGameplayTag::StaticStruct())
			{
				StructProp->CopyCompleteValue(Dest, &Channel);
			}
			else if (StructProp->Struct == StructType && Payload != nullptr)
			{
				StructProp->CopyCompleteValue(Dest, Payload);
			}
		}
	}

	Owner->ProcessEvent(Func, Params.GetData());

	for (TFieldIterator<FProperty> It(Func); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		It->DestroyValue_InContainer(Params.GetData());
	}
}

