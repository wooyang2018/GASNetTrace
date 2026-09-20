#include "GASNetTraceSubsystem.h"

#include "AbilitySystemComponent.h"
#include "Abilities/GameplayAbility.h"
#include "EngineUtils.h"
#include "GameplayEffect.h"
#include "GameplayEffectTypes.h"
#include "AbilitySystemGlobals.h"
#include "GameplayCueManager.h"
#include "GASNetTraceTrace.h"
#include "GASNetTraceClockSyncActor.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

bool UGASNetTraceSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
    const UWorld* World = Cast<UWorld>(Outer);
    return Super::ShouldCreateSubsystem(Outer) && World && World->IsGameWorld();
}

void UGASNetTraceSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
    Super::OnWorldBeginPlay(InWorld);
    if (!FGASNetTrace::IsEnabled()) return;

    FGASNetTrace::EmitNetworkConfig(&InWorld);
    if (UGameplayCueManager* CueManager = UAbilitySystemGlobals::Get().GetGameplayCueManager())
    {
        GameplayCueHandle = CueManager->OnGameplayCueRouted().AddUObject(this, &ThisClass::OnGameplayCueRouted);
    }

    for (TActorIterator<AActor> It(&InWorld); It; ++It) DiscoverActor(*It);
    ActorSpawnedHandle = InWorld.AddOnActorSpawnedHandler(
        FOnActorSpawned::FDelegate::CreateUObject(this, &ThisClass::OnActorSpawned));
#if !UE_BUILD_SHIPPING
    float AutoQuitSeconds = 0.0f;
    if (FParse::Value(FCommandLine::Get(), TEXT("GASNetTraceAutoQuitAfter="), AutoQuitSeconds) && AutoQuitSeconds > 0.0f)
    {
        InWorld.GetTimerManager().SetTimer(AutoQuitTimer, []
        {
            RequestEngineExit(TEXT("GASNetTraceAutoQuitAfter elapsed; graceful trace shutdown"));
        }, AutoQuitSeconds, false);
    }
#endif
}

void UGASNetTraceSubsystem::Deinitialize()
{
    if (UWorld* World = GetWorld(); World && ActorSpawnedHandle.IsValid())
    {
        World->RemoveOnActorSpawnedHandler(ActorSpawnedHandle);
        World->GetTimerManager().ClearTimer(AutoQuitTimer);
    }
    TArray<TWeakObjectPtr<UAbilitySystemComponent>> Components;
    Registered.GetKeys(Components);
    for (const TWeakObjectPtr<UAbilitySystemComponent>& ASC : Components) UnregisterASC(ASC.Get());
    if (GameplayCueHandle.IsValid())
    {
        if (UGameplayCueManager* CueManager = UAbilitySystemGlobals::Get().GetGameplayCueManager())
        {
            CueManager->OnGameplayCueRouted().Remove(GameplayCueHandle);
        }
        GameplayCueHandle.Reset();
    }
    Super::Deinitialize();
}

void UGASNetTraceSubsystem::DiscoverActor(AActor* Actor)
{
    if (!Actor) return;
    if (APlayerController* Controller = Cast<APlayerController>(Actor)) EnsureClockSync(Controller);
    TArray<UAbilitySystemComponent*> Components;
    Actor->GetComponents<UAbilitySystemComponent>(Components);
    for (UAbilitySystemComponent* ASC : Components) RegisterASC(ASC);
}

void UGASNetTraceSubsystem::EnsureClockSync(APlayerController* Controller)
{
#if UE_TRACE_ENABLED && !UE_BUILD_SHIPPING
    if (!Controller || !Controller->HasAuthority() || ClockSyncActors.Contains(Controller)) return;
    FActorSpawnParameters Params;
    Params.Owner = Controller;
    Params.ObjectFlags |= RF_Transient;
    if (AGASNetTraceClockSyncActor* SyncActor = GetWorld()->SpawnActor<AGASNetTraceClockSyncActor>(Params))
    {
        ClockSyncActors.Add(Controller, SyncActor);
    }
#endif
}

void UGASNetTraceSubsystem::OnActorSpawned(AActor* Actor) { DiscoverActor(Actor); }

void UGASNetTraceSubsystem::RegisterASC(UAbilitySystemComponent* ASC)
{
    if (!FGASNetTrace::IsEnabled() || !IsValid(ASC) || Registered.Contains(ASC)) return;

    FBindings& Bindings = Registered.Add(ASC);
    const TWeakObjectPtr<UAbilitySystemComponent> WeakASC(ASC);
    Bindings.Activated = ASC->AbilityActivatedCallbacks.AddUObject(this, &ThisClass::OnAbilityActivated, WeakASC);
    Bindings.Failed = ASC->AbilityFailedCallbacks.AddUObject(this, &ThisClass::OnAbilityFailed, WeakASC);
    Bindings.Ended = ASC->OnAbilityEnded.AddUObject(this, &ThisClass::OnAbilityEnded, WeakASC);
    Bindings.EffectAdded = ASC->OnActiveGameplayEffectAddedDelegateToSelf.AddUObject(this, &ThisClass::OnEffectAdded);
    Bindings.EffectRemoved = ASC->OnAnyGameplayEffectRemovedDelegate().AddUObject(this, &ThisClass::OnEffectRemoved, WeakASC);
    Bindings.TagChanged = ASC->RegisterGenericGameplayTagEvent().AddUObject(this, &ThisClass::OnTagChanged, WeakASC);

    TArray<FGameplayAttribute> Attributes;
    ASC->GetAllAttributes(Attributes);
    for (const FGameplayAttribute& Attribute : Attributes)
    {
        const FString Name = Attribute.GetName();
        Bindings.Attributes.Add(Name,
            ASC->GetGameplayAttributeValueChangeDelegate(Attribute).AddUObject(this, &ThisClass::OnAttributeChanged, Name, WeakASC));
    }

    constexpr EGASNetTraceCoverage ObserverCoverage = EGASNetTraceCoverage::ObserverAbility |
        EGASNetTraceCoverage::ObserverEffect | EGASNetTraceCoverage::ObserverTag | EGASNetTraceCoverage::ObserverAttribute;
    FGASNetTrace::EmitCoverage(ASC, ObserverCoverage);
}

void UGASNetTraceSubsystem::UnregisterASC(UAbilitySystemComponent* ASC)
{
    if (!ASC) return;
    FBindings Bindings;
    if (!Registered.RemoveAndCopyValue(ASC, Bindings)) return;
    ASC->AbilityActivatedCallbacks.Remove(Bindings.Activated);
    ASC->AbilityFailedCallbacks.Remove(Bindings.Failed);
    ASC->OnAbilityEnded.Remove(Bindings.Ended);
    ASC->OnActiveGameplayEffectAddedDelegateToSelf.Remove(Bindings.EffectAdded);
    ASC->OnAnyGameplayEffectRemovedDelegate().Remove(Bindings.EffectRemoved);
    ASC->RegisterGenericGameplayTagEvent().Remove(Bindings.TagChanged);
    TArray<FGameplayAttribute> Attributes;
    ASC->GetAllAttributes(Attributes);
    for (const FGameplayAttribute& Attribute : Attributes)
    {
        if (const FDelegateHandle* Handle = Bindings.Attributes.Find(Attribute.GetName()))
            ASC->GetGameplayAttributeValueChangeDelegate(Attribute).Remove(*Handle);
    }
    for (const TPair<FActiveGameplayEffectHandle, FDelegateHandle>& Pair : Bindings.EffectStacks)
    {
        if (FOnActiveGameplayEffectStackChange* Delegate = ASC->OnGameplayEffectStackChangeDelegate(Pair.Key))
            Delegate->Remove(Pair.Value);
    }
}

void UGASNetTraceSubsystem::OnAbilityActivated(UGameplayAbility* Ability, TWeakObjectPtr<UAbilitySystemComponent> ASC)
{
    FGASNetTraceContext Context;
    if (Ability)
    {
        Context.AbilitySpecHandle = static_cast<int32>(GetTypeHash(Ability->GetCurrentAbilitySpecHandle()));
        const FPredictionKey Key = Ability->GetCurrentActivationInfo().GetActivationPredictionKey();
        Context.PredictionCurrent = Key.Current;
        Context.PredictionBase = Key.Base;
        Context.PredictiveConnectionKey = Key.GetPredictiveConnectionKey();
        if (Key.IsValidKey())
        {
            FGASNetTrace::EmitEvent(EGASNetTraceEventType::PredictionWindow, ASC.Get(), Ability, Context, TEXTVIEW("Opened"), 0.0, 0.0, 1);
        }
    }
    FGASNetTrace::EmitEvent(EGASNetTraceEventType::AbilityActivated, ASC.Get(), Ability, Context);
}

void UGASNetTraceSubsystem::OnAbilityFailed(const UGameplayAbility* Ability, const FGameplayTagContainer& FailureTags,
    TWeakObjectPtr<UAbilitySystemComponent> ASC)
{
    FGASNetTraceContext Context;
    if (Ability)
    {
        Context.AbilitySpecHandle = static_cast<int32>(GetTypeHash(Ability->GetCurrentAbilitySpecHandle()));
        const FPredictionKey Key = Ability->GetCurrentActivationInfo().GetActivationPredictionKey();
        Context.PredictionCurrent = Key.Current;
        Context.PredictionBase = Key.Base;
        Context.PredictiveConnectionKey = Key.GetPredictiveConnectionKey();
    }
    FGASNetTrace::EmitEvent(EGASNetTraceEventType::AbilityFailed, ASC.Get(), Ability, Context, FailureTags.ToStringSimple());
}

void UGASNetTraceSubsystem::OnAbilityEnded(const FAbilityEndedData& Data, TWeakObjectPtr<UAbilitySystemComponent> ASC)
{
    FGASNetTraceContext Context;
    Context.AbilitySpecHandle = static_cast<int32>(GetTypeHash(Data.AbilitySpecHandle));
    if (Data.AbilityThatEnded)
    {
        const FPredictionKey Key = Data.AbilityThatEnded->GetCurrentActivationInfo().GetActivationPredictionKey();
        Context.PredictionCurrent = Key.Current;
        Context.PredictionBase = Key.Base;
        Context.PredictiveConnectionKey = Key.GetPredictiveConnectionKey();
        if (Key.IsValidKey())
        {
            FGASNetTrace::EmitEvent(EGASNetTraceEventType::PredictionWindow, ASC.Get(), Data.AbilityThatEnded,
                Context, TEXTVIEW("Closed"), 0.0, 0.0, 0);
        }
    }
    FGASNetTrace::EmitEvent(EGASNetTraceEventType::AbilityEnded, ASC.Get(), Data.AbilityThatEnded, Context,
        Data.bWasCancelled ? TEXTVIEW("Cancelled") : TEXTVIEW("Completed"), 0.0, 0.0, Data.bWasCancelled ? 1 : 0);
}

void UGASNetTraceSubsystem::OnEffectAdded(UAbilitySystemComponent* Target, const FGameplayEffectSpec& Spec, FActiveGameplayEffectHandle Handle)
{
    FGASNetTraceContext Context;
    Context.PredictionCurrent = Spec.GetContext().GetAbilityInstance_NotReplicated() ?
        Spec.GetContext().GetAbilityInstance_NotReplicated()->GetCurrentActivationInfo().GetActivationPredictionKey().Current : 0;
    FGASNetTrace::EmitEvent(EGASNetTraceEventType::EffectAdded, Target, Spec.Def, Context, Handle.ToString());
    if (FBindings* Bindings = Registered.Find(Target))
    {
        if (FOnActiveGameplayEffectStackChange* Delegate = Target->OnGameplayEffectStackChangeDelegate(Handle))
        {
            Bindings->EffectStacks.Add(Handle, Delegate->AddUObject(this, &ThisClass::OnEffectStackChanged,
                TWeakObjectPtr<UAbilitySystemComponent>(Target)));
        }
    }
}

void UGASNetTraceSubsystem::OnEffectRemoved(const FActiveGameplayEffect& Effect, TWeakObjectPtr<UAbilitySystemComponent> ASC)
{
    FGASNetTrace::EmitEvent(EGASNetTraceEventType::EffectRemoved, ASC.Get(), Effect.Spec.Def, {}, Effect.Handle.ToString());
    if (FBindings* Bindings = Registered.Find(ASC)) Bindings->EffectStacks.Remove(Effect.Handle);
}

void UGASNetTraceSubsystem::OnEffectStackChanged(FActiveGameplayEffectHandle Handle, int32 NewCount, int32 PreviousCount,
    TWeakObjectPtr<UAbilitySystemComponent> ASC)
{
    FGASNetTrace::EmitEvent(EGASNetTraceEventType::EffectStack, ASC.Get(), nullptr, {}, Handle.ToString(), PreviousCount, NewCount);
}

void UGASNetTraceSubsystem::OnTagChanged(const FGameplayTag Tag, int32 NewCount, TWeakObjectPtr<UAbilitySystemComponent> ASC)
{
    FGASNetTrace::EmitEvent(EGASNetTraceEventType::TagChanged, ASC.Get(), nullptr, {}, Tag.ToString(), NewCount);
}

void UGASNetTraceSubsystem::OnAttributeChanged(const FOnAttributeChangeData& Data, FString AttributeName,
    TWeakObjectPtr<UAbilitySystemComponent> ASC)
{
    FGASNetTrace::EmitEvent(EGASNetTraceEventType::AttributeChanged, ASC.Get(), nullptr, {}, AttributeName,
        Data.OldValue, Data.NewValue);
}

void UGASNetTraceSubsystem::OnGameplayCueRouted(AActor* TargetActor, FGameplayTag CueTag,
    EGameplayCueEvent::Type EventType, const FGameplayCueParameters& Parameters, EGameplayCueExecutionOptions)
{
    (void)Parameters;
    UAbilitySystemComponent* ASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(TargetActor);
    if (!ASC) return;
    FGASNetTraceContext Context;
    const FString Detail = FString::Printf(TEXT("%s|%d"), *CueTag.ToString(), static_cast<int32>(EventType));
    FGASNetTrace::EmitEvent(EGASNetTraceEventType::GameplayCue, ASC, TargetActor, Context, Detail,
        static_cast<double>(EventType));
}
