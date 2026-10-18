using Avalonia;
using Avalonia.Controls;
using System.Text.Json.Nodes;

namespace VRPorts;

public partial class MainWindow
{
    FeatureToggleButton? homeRedmodToggle;
    FeatureToggleButton? homeEnhancementToggle;

    bool QuickOptionBlocked => busy || loading || !portStateCurrent || B(state, "gameRunning") || B(state, "vortexRunning")
        || gameProfileSelection.RecoveryPending || gameProfileSelection.PendingId != null;

    FeatureToggleButton BuildHomeRedmodToggle()
    {
        var binding = new FeatureToggleBinding(
            () => new FeatureToggleState("REDmod", null, B(state?["redmod"], "enabled"), B(state?["redmod"], "enabled"), B(state?["redmod"], "available")),
            enabled => _ = Run("SetRedmodEnabled", new JsonObject { ["enabled"] = enabled }));
        homeRedmodToggle = new FeatureToggleButton(binding, () => QuickOptionBlocked,
            "Использование REDmod в текущем профиле. При включении недостающий инструмент восстанавливается из библиотеки центра.",
            "Штатный инструмент REDmod отсутствует и не сохранён в библиотеке центра.",
            () => "Закрой игру и дождись завершения текущей операции.")
        { Name = "HomeRedmodToggle", MinHeight = 0, MinWidth = 0, Padding = new Thickness(9, 6) };
        return homeRedmodToggle;
    }

    FeatureToggleButton BuildHomeEnhancementToggle()
    {
        homeEnhancementToggle = CreateEnhancementPackageToggle(currentLaunch: true);
        homeEnhancementToggle.MinHeight = 0; homeEnhancementToggle.MinWidth = 0;
        homeEnhancementToggle.Padding = new Thickness(9, 6);
        return homeEnhancementToggle;
    }

    void RefreshCurrentLaunchControls()
    {
        homeRedmodToggle?.Refresh();
        homeEnhancementToggle?.Refresh();
    }
}
