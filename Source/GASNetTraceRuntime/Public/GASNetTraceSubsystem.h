#pragma once

#include "CoreMinimal.h"
#include "ActiveGameplayEffectHandle.h"
#include "GameplayTagContainer.h"
#include "GameplayCueManager.h"
#include "Subsystems/WorldSubsystem.h"
#include "TimerManager.h"
#include "GASNetTraceSubsystem.generated.h"

class UAbilitySystemComponent;
class UGameplayAbility;
class APlayerController;
class AGASNetTraceClockSyncActor;
struct FAbilityEndedData;
struct FActiveGameplayEffect;
struct FGameplayEffectSpec;
struct FOnAttributeChangeData;

UCLASS()
class GASNETTRACERUNTIME_API UGASNetTraceSubsystem final : public UWorldSubsystem
{
    GENERATED_BODY()

public:
    virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
    virtual void OnWorldBeginPlay(UWorld& InWorld) override;
    virtual void Deinitialize() override;

    void RegisterASC(UAbilitySystemComponent* ASC);
    void UnregisterASC(UAbilitySystemComponent* ASC);

private:
    struct FBindings
    {
        FDelegateHandle Activated;
        FDelegateHandle Failed;
        FDelegateHandle Ended;
        FDelegateHandle EffectAdded;
        FDelegateHandle EffectRemoved;
        FDelegateHandle TagChanged;
        TMap<FString, FDelegateHandle> Attributes;
        TMap<FActiveGameplayEffectHandle, FDelegateHandle> EffectStacks;
    };

    void DiscoverActor(AActor* Actor);
    void EnsureClockSync(APlayerController* Controller);
    void OnActorSpawned(AActor* Actor);
    void OnAbilityActivated(UGameplayAbility* Ability, TWeakObjectPtr<UAbilitySystemComponent> ASC);
    void OnAbilityFailed(const UGameplayAbility* Ability, const FGameplayTagContainer& FailureTags, TWeakObjectPtr<UAbilitySystemComponent> ASC);
    void OnAbilityEnded(const FAbilityEndedData& Data, TWeakObjectPtr<UAbilitySystemComponent> ASC);
    void OnEffectAdded(UAbilitySystemComponent* Target, const FGameplayEffectSpec& Spec, FActiveGameplayEffectHandle Handle);
    void OnEffectRemoved(const FActiveGameplayEffect& Effect, TWeakObjectPtr<UAbilitySystemComponent> ASC);
    void OnEffectStackChanged(FActiveGameplayEffectHandle Handle, int32 NewCount, int32 PreviousCount, TWeakObjectPtr<UAbilitySystemComponent> ASC);
    void OnTagChanged(const FGameplayTag Tag, int32 NewCount, TWeakObjectPtr<UAbilitySystemComponent> ASC);
    void OnAttributeChanged(const FOnAttributeChangeData& Data, FString AttributeName, TWeakObjectPtr<UAbilitySystemComponent> ASC);
    void OnGameplayCueRouted(AActor* TargetActor, FGameplayTag CueTag, EGameplayCueEvent::Type EventType,
        const FGameplayCueParameters& Parameters, EGameplayCueExecutionOptions Options);

    TMap<TWeakObjectPtr<UAbilitySystemComponent>, FBindings> Registered;
    FDelegateHandle ActorSpawnedHandle;
    FDelegateHandle GameplayCueHandle;
    FTimerHandle AutoQuitTimer;
    TMap<TWeakObjectPtr<APlayerController>, TWeakObjectPtr<AGASNetTraceClockSyncActor>> ClockSyncActors;
};
