#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbilityTypes.h"
#include "GASNetTraceTrace.h"

class UAbilitySystemComponent;
class UGameplayAbility;

/** Optional project adapter. It supplements public-delegate observation; it never mutates GAS state. */
class GASNETTRACERUNTIME_API IGASNetTraceAdapter
{
public:
#if UE_BUILD_SHIPPING
    static FORCEINLINE void RegisterASC(UAbilitySystemComponent*) {}
    static FORCEINLINE void UnregisterASC(UAbilitySystemComponent*) {}
    static FORCEINLINE void AbilityAttempt(UAbilitySystemComponent*, const UGameplayAbility*, FGameplayAbilitySpecHandle, const FPredictionKey&, FStringView) {}
    static FORCEINLINE void AbilityAttemptResult(UAbilitySystemComponent*, const UGameplayAbility*, FGameplayAbilitySpecHandle, const FPredictionKey&, bool, FStringView) {}
    static FORCEINLINE void PredictionWindow(UAbilitySystemComponent*, const UGameplayAbility*, FGameplayAbilitySpecHandle, const FPredictionKey&, bool) {}
    static FORCEINLINE void TargetData(UAbilitySystemComponent*, const UGameplayAbility*, FGameplayAbilitySpecHandle, const FPredictionKey&, int32, FStringView, FStringView, bool) {}
    static FORCEINLINE void ActorInfo(UAbilitySystemComponent*, FStringView) {}
    static FORCEINLINE void AbilitySpec(UAbilitySystemComponent*, const UGameplayAbility*, FGameplayAbilitySpecHandle, FStringView) {}
#else
    static void RegisterASC(UAbilitySystemComponent* ASC);
    static void UnregisterASC(UAbilitySystemComponent* ASC);

    static void AbilityAttempt(UAbilitySystemComponent* ASC, const UGameplayAbility* Ability,
        FGameplayAbilitySpecHandle SpecHandle, const FPredictionKey& PredictionKey, FStringView Source);
    static void AbilityAttemptResult(UAbilitySystemComponent* ASC, const UGameplayAbility* Ability,
        FGameplayAbilitySpecHandle SpecHandle, const FPredictionKey& PredictionKey, bool bActivated, FStringView Reason);
    static void PredictionWindow(UAbilitySystemComponent* ASC, const UGameplayAbility* Ability,
        FGameplayAbilitySpecHandle SpecHandle, const FPredictionKey& PredictionKey, bool bOpened);
    static void TargetData(UAbilitySystemComponent* ASC, const UGameplayAbility* Ability,
        FGameplayAbilitySpecHandle SpecHandle, const FPredictionKey& PredictionKey,
        int32 ItemCount, FStringView Stage, FStringView ResultCode, bool bAccepted);
    static void ActorInfo(UAbilitySystemComponent* ASC, FStringView Reason);
    static void AbilitySpec(UAbilitySystemComponent* ASC, const UGameplayAbility* Ability,
        FGameplayAbilitySpecHandle SpecHandle, FStringView Operation);
#endif
};
