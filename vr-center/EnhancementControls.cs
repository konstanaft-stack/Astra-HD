using Avalonia.Controls;
using System.Text.Json.Nodes;

namespace VRPorts;

public partial class MainWindow
{
    FeatureToggleButton CreateEnhancementPackageToggle(bool currentLaunch = false)
    {
        bool Flat() => S((currentLaunch ? ActualPort : selected)?.Node, "kind") == "flat";
        bool Shared() => S(state?["enhancementPackage"], "scope") == "game-shared";
        var binding = new FeatureToggleBinding(
            () => new FeatureToggleState("NR + MFG", null,
                B(state?["enhancementPackage"], "enabled"),
                B(state?["enhancementPackage"], "enabled"),
                (Flat() || Shared()) && B(state?["enhancementPackage"], "available") && B(state?["enhancementPackage"], "compatible")),
            enabled => _ = Run("SetEnhancementPackage", new JsonObject { ["enabled"] = enabled }));
        var button = new FeatureToggleButton(binding,
            () => busy || loading || B(state, "gameRunning") || B(state, "vortexRunning")
                || B(state, "mo2Running") || B(state, "modOrganizerRunning"),
            Shared() ? "Общий комплект NR + MFG для всех профилей игры. Настройки пользователя сохраняются."
                : "Настроенный комплект NR + MFG для обычной игры. Настройки пользователя сохраняются.",
            Flat() || Shared() ? S(state?["enhancementPackage"], "reason") is { Length: > 0 } reason ? reason : "Комплект NR + MFG не добавлен в библиотеку." : "Это дополнение для плоской игры. У выбранного VR-порта собственные настройки генерации.",
            () => B(state, "gameRunning") ? "Изменение доступно после выхода из игры."
                : B(state, "vortexRunning") || B(state, "mo2Running") || B(state, "modOrganizerRunning")
                    ? "Закрой менеджер модов перед изменением."
                : "Дождись завершения операции.");
        button.Name = currentLaunch ? "HomeEnhancementToggle" : "EnhancementPackageToggle";
        featureButtons.Add(button);
        return button;
    }

    Control EnhancementPackageCard()
    {
        var package = state?["enhancementPackage"];
        bool flat = S(selected?.Node, "kind") == "flat";
        bool shared = S(package, "scope") == "game-shared";
        var button = CreateEnhancementPackageToggle();
        string unavailable = "У выбранного VR-порта собственные настройки генерации.";
        var status = Text(flat || shared || B(package, "suspended") ? S(package, "status") : unavailable, 11, "#9BB0A3");
        status.Name = "EnhancementPackageStatus";
        var source = Text(S(package, "sourceLabel"), 10, "#9BB0A3");
        source.Name = "EnhancementPackageSource";
        return new StackPanel { Spacing = 4, Children = { button, status, source } };
    }
}
