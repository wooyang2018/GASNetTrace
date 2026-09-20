#include "GASNetTraceAdapter.h"

#include "AbilitySystemComponent.h"
#include "Abilities/GameplayAbility.h"
#include "GASNetTraceSubsystem.h"

#if !UE_BUILD_SHIPPING

namespace
{
    FGASNetTraceContext MakeContext(FGameplayAbilitySpecHandle Handle, const FPredictionKey& Key)
    {
        FGASNetTraceContext Context;
        Context.AbilitySpecHandle = static_cast<int32>(GetTypeHash(Handle));
        Context.PredictionCurrent = Key.Current;
        Context.PredictionBase = Key.Base;
        Context.PredictiveConnectionKey = static_cast<int64>(Key.GetPredictiveConnectionKey());
        return Context;
    }

    UGASNetTraceSubsystem* FindSubsystem(UAbilitySystemComponent* ASC)
    {
        return ASC && ASC->GetWorld() ? ASC->GetWorld()->GetSubsystem<UGASNetTraceSubsystem>() : nullptr;
    }
}

void IGASNetTraceAdapter::RegisterASC(UAbilitySystemComponent* ASC)
{
    if (!FGASNetTrace::IsEnabled()) return;
    if (UGASNetTraceSubsystem* Subsystem = FindSubsystem(ASC)) Subsystem->RegisterASC(ASC);
}

void IGASNetTraceAdapter::UnregisterASC(UAbilitySystemComponent* ASC)
{
    if (!FGASNetTrace::IsEnabled()) return;
    if (UGASNetTraceSubsystem* Subsystem = FindSubsystem(ASC)) Subsystem->UnregisterASC(ASC);
}

void IGASNetTraceAdapter::AbilityAttempt(UAbilitySystemComponent* ASC, const UGameplayAbility* Ability,
    FGameplayAbilitySpecHandle SpecHandle, const FPredictionKey& PredictionKey, FStringView Source)
{
    if (!FGASNetTrace::IsEnabled()) return;
    FGASNetTrace::EmitEvent(EGASNetTraceEventType::AbilityAttempt, ASC, Ability, MakeContext(SpecHandle, PredictionKey), Source);
    FGASNetTrace::EmitCoverage(ASC, EGASNetTraceCoverage::AdapterAttempt | EGASNetTraceCoverage::AdapterPrediction);
}

void IGASNetTraceAdapter::AbilityAttemptResult(UAbilitySystemComponent* ASC, const UGameplayAbility* Ability,
    FGameplayAbilitySpecHandle SpecHandle, const FPredictionKey& PredictionKey, bool bActivated, FStringView Reason)
{
    if (!FGASNetTrace::IsEnabled()) return;
    FGASNetTrace::EmitEvent(bActivated ? EGASNetTraceEventType::AbilityActivated : EGASNetTraceEventType::AbilityFailed,
        ASC, Ability, MakeContext(SpecHandle, PredictionKey), Reason, 0.0, 0.0, bActivated ? 1 : 0);
}

void IGASNetTraceAdapter::PredictionWindow(UAbilitySystemComponent* ASC, const UGameplayAbility* Ability,
    FGameplayAbilitySpecHandle SpecHandle, const FPredictionKey& PredictionKey, bool bOpened)
{
    if (!FGASNetTrace::IsEnabled() || !PredictionKey.IsValidKey()) return;
    FGASNetTrace::EmitEvent(EGASNetTraceEventType::PredictionWindow, ASC, Ability,
        MakeContext(SpecHandle, PredictionKey), bOpened ? TEXTVIEW("Opened") : TEXTVIEW("Closed"), 0.0, 0.0, bOpened ? 1 : 0);
    FGASNetTrace::EmitCoverage(ASC, EGASNetTraceCoverage::AdapterPrediction);
}

void IGASNetTraceAdapter::TargetData(UAbilitySystemComponent* ASC, const UGameplayAbility* Ability,
    FGameplayAbilitySpecHandle SpecHandle, const FPredictionKey& PredictionKey, int32 ItemCount,
    FStringView Stage, FStringView ResultCode, bool bAccepted)
{
    if (!FGASNetTrace::IsEnabled()) return;
    const FString Detail = FString::Printf(TEXT("%.*s|%.*s"), Stage.Len(), Stage.GetData(), ResultCode.Len(), ResultCode.GetData());
    FGASNetTrace::EmitEvent(EGASNetTraceEventType::TargetData, ASC, Ability, MakeContext(SpecHandle, PredictionKey),
        Detail, static_cast<double>(ItemCount), 0.0, bAccepted ? 1 : 0);
    FGASNetTrace::EmitCoverage(ASC, EGASNetTraceCoverage::AdapterTargetData);
}

void IGASNetTraceAdapter::ActorInfo(UAbilitySystemComponent* ASC, FStringView Reason)
{
    if (!FGASNetTrace::IsEnabled()) return;
    FGASNetTrace::EmitEvent(EGASNetTraceEventType::ActorInfo, ASC, ASC ? ASC->GetAvatarActor() : nullptr, {}, Reason);
    FGASNetTrace::EmitCoverage(ASC, EGASNetTraceCoverage::AdapterActorInfo);
}

void IGASNetTraceAdapter::AbilitySpec(UAbilitySystemComponent* ASC, const UGameplayAbility* Ability,
    FGameplayAbilitySpecHandle SpecHandle, FStringView Operation)
{
    if (!FGASNetTrace::IsEnabled()) return;
    FGASNetTrace::EmitEvent(EGASNetTraceEventType::AbilitySpec, ASC, Ability, MakeContext(SpecHandle, {}), Operation);
    FGASNetTrace::EmitCoverage(ASC, EGASNetTraceCoverage::AdapterSpec);
}

#endif
