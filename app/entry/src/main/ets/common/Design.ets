import { curves } from '@kit.ArkUI';

// The runtime's own surfaces (the diagnostics overlay) share the DP monitor's design
// (dp-monitor/app/entry/src/main/ets/common/FrameMeter.ets): Fluid Functionalism's system
// (fluidfunctionalism.com/docs) translated to ArkUI, in its dark theme, as an instrument over
// whatever the app draws.

// Two text colours, never an opacity step of them; warnings in one amber.
export const INK = '#FAFAFA';
export const INK_MUTED = '#A1A1A1';
export const WARN = '#FF9F0A';
export const DOT_LIVE = '#30D158';
export const DOT_IDLE = '#8E8E93';
// Hover and selected are overlays on whatever surface is under them, not fixed colours.
export const HOVER = '#0FFFFFFF';
export const SELECTED = '#1AFFFFFF';
export const HAIRLINE = '#1FFFFFFF';
// Six type roles. The fps numeral is the one size outside them: it is the instrument.
export const TYPE_TITLE = 16;
export const TYPE_SUBTITLE = 14;
export const TYPE_BODY = 13;
export const TYPE_CAPTION = 12;
export const TYPE_MICRO = 11;
// Three weights; ArkUI steps weights by 100, so semibold is 500.
export const WEIGHT_NORMAL = 400;
export const WEIGHT_SEMIBOLD = 500;
export const WEIGHT_BOLD = 700;
export const TABULAR = '"tnum" on';
// Three spring tiers, the bigger the thing that moves the slower. Exits are plain tweens a
// tier quicker, so a dismissal reads final instead of replaying the entrance backwards.
export const SPRING_FAST: ICurve = curves.springMotion(0.08, 1);
export const SPRING_MODERATE: ICurve = curves.springMotion(0.16, 1);
export const SPRING_SLOW: ICurve = curves.springMotion(0.24, 0.88);
export const EXIT_FAST_MS = 60;
export const EXIT_MODERATE_MS = 120;
export const EXIT_SLOW_MS = 160;
// Groups sit one step above the glass and inset from it, with concentric corners: the
// group's radius is the panel's less the inset and the 1 px edge.
export const PANEL_RADIUS = 28;
export const PANEL_INSET = 10;
export const GROUP_RADIUS = 17;
export const GROUP_PADDING = 15;
export const GROUP_GAP = 10;
