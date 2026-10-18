using System;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;

namespace VRPorts;

public sealed class FeatureToggleButton : Button {
    protected override Type StyleKeyOverride => typeof(Button);
    readonly FeatureToggleBinding binding;
    readonly Func<bool> blocked;
    readonly string tip;
    readonly string unavailableTip;
    readonly Func<string>? blockedTip;
    readonly TextBlock title = new() { FontSize = 13, FontWeight = FontWeight.SemiBold };

    public FeatureToggleButton(FeatureToggleBinding binding, Func<bool> blocked, string tip, string unavailableTip, Func<string>? blockedTip = null) {
        this.binding = binding;
        this.blocked = blocked;
        this.tip = tip;
        this.unavailableTip = unavailableTip;
        this.blockedTip = blockedTip;
        Classes.Add("FeatureToggle");
        Content = title;
        HorizontalAlignment = HorizontalAlignment.Left;
        VerticalAlignment = VerticalAlignment.Center;
        ToolTip.SetShowOnDisabled(this, true);
        Click += (_, _) => {
            binding.RequestToggle(blocked());
            Refresh();
        };
        Refresh();
    }

    public void Refresh() {
        var state = binding.State;
        foreach (var name in Enum.GetNames<FeatureVisualState>()) Classes.Remove(name);
        Classes.Add(state.VisualState.ToString());
        title.Text = state.DisplayName;
        bool temporarilyBlocked = blocked();
        IsEnabled = state.Available && !temporarilyBlocked;
        AutomationProperties.SetName(this, state.DisplayName + ", " + state.StatusText);
        ToolTip.SetTip(this, !state.Available ? unavailableTip
            : temporarilyBlocked && blockedTip != null ? state.StatusText + ". " + blockedTip()
            : state.StatusText + ". " + tip);
    }
}
