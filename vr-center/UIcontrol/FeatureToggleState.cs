using System;

namespace VRPorts {
    public enum FeatureVisualState { On, Off, Pending, Unavailable }

    public static class FeatureTogglePalette {
        public const string On = "#86EFAC";
        public const string Off = "#E88D85";
        public const string Pending = "#78B7FF";
        public const string Unavailable = "#9BA9A1";
    }

    public record FeatureToggleState(string Name, string? AlternateName, bool Selected, bool Actual, bool Available) {
        public string DisplayName => AlternateName != null && !Selected ? AlternateName : Name;
        public FeatureVisualState VisualState => !Available ? FeatureVisualState.Unavailable
            : Selected != Actual ? FeatureVisualState.Pending
            : AlternateName != null || Actual ? FeatureVisualState.On : FeatureVisualState.Off;
        public string StatusText => VisualState switch {
            FeatureVisualState.Unavailable => "Недоступно",
            FeatureVisualState.Pending => AlternateName != null ? "После применения" : Selected ? "Будет включено" : "Будет выключено",
            _ when AlternateName != null => "Активный режим",
            FeatureVisualState.On => "Включено",
            _ => "Выключено"
        };
        public string Accent => VisualState switch {
            FeatureVisualState.On => FeatureTogglePalette.On,
            FeatureVisualState.Off => FeatureTogglePalette.Off,
            FeatureVisualState.Pending => FeatureTogglePalette.Pending,
            _ => FeatureTogglePalette.Unavailable
        };
    }

    // The binding owns no optimistic state: immediate actions remain factual
    // until the backend supplies a fresh snapshot; staged setters own their draft.
    public sealed class FeatureToggleBinding {
        readonly Func<FeatureToggleState> read;
        readonly Action<bool> write;
        public FeatureToggleBinding(Func<FeatureToggleState> read, Action<bool> write) { this.read = read; this.write = write; }
        public FeatureToggleState State => read();
        public bool RequestToggle(bool blocked = false) {
            var state = read();
            if (blocked || !state.Available) return false;
            write(!state.Selected);
            return true;
        }
    }
}
