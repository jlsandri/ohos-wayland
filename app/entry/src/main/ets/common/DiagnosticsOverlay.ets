import { displaySync } from '@kit.ArkGraphics2D';
import runtime, { Diagnostics } from 'libentry.so';
import { DOT_IDLE, DOT_LIVE, GROUP_GAP, GROUP_PADDING, GROUP_RADIUS, HAIRLINE, HOVER, INK, INK_MUTED, PANEL_INSET,
  PANEL_RADIUS, TABULAR, TYPE_BODY, TYPE_CAPTION, TYPE_MICRO, TYPE_SUBTITLE, WARN, WEIGHT_SEMIBOLD } from './Design';

// The numbers cover the last second; half of that keeps them readable, and a bar glides one
// sample's worth of frames in this time.
const SAMPLE_MS = 500;
// The numeral reads the rate every display frame and eases toward it with this time constant,
// so a change shows within a few frames and passes through each whole number on the way.
const NUMERAL_TAU_MS = 60;
// Multiples of 5 vp, whole device pixels at the Fold's density of 1.8, so the 1 px glass edge
// lands on one pixel row.
const PANEL_WIDTH = 260;
const CHART_WIDTH = PANEL_WIDTH - 2 * PANEL_INSET - 2 * GROUP_PADDING;
const CHART_HEIGHT = 36;
// The chart's top is three display periods; the guide line marks one.
const CHART_PERIODS = 3;
// A frame this many periods after the last one held the picture for at least one extra refresh.
const LATE_PERIODS = 1.5;
// Within this of 1, every client pixel lands on one view pixel.
const PIXEL_EXACT = 0.005;

// One presented frame, in chart units: x and width over a chart one unit wide, height over one
// unit tall. `id` is the frame's number, so the strip can glide by the frames that arrived.
export interface Bar {
  id: number;
  x: number;
  width: number;
  height: number;
  late: boolean;
  opacity: number;
}

// One stretch of the facts line; a warning stretch is drawn in the warning colour.
export interface Fact {
  text: string;
  warn: boolean;
}

export interface Stat {
  value: string;
  caption: string;
}

export interface Summary {
  // Frames per second while the app draws, else one word for what it is doing.
  hero: string;
  live: boolean;
  state: string;
  stateColor: string;
  bars: Bar[];
  // How the frames arrive, then what the buffer is.
  note: string;
  noteWarn: boolean;
  facts: Fact[];
  stats: Stat[];
}

// Whole numbers when the rate sits on one, so a steady 89.9 reads as the 90 it is.
function fpsText(fps: number): string {
  return Math.abs(fps - Math.round(fps)) < 0.3 ? `${Math.round(fps)}` : fps.toFixed(1);
}

function scaleText(scale: number): string {
  return `${Math.round(scale * 100) / 100}×`;
}

// The last second of presented frames, newest at the right; a full chart is one frame per
// refresh. Older bars fade.
function timeline(intervals: number[], lastId: number, periodMs: number): Bar[] {
  const slots = Math.max(30, Math.round(1000 / periodMs));
  const step = 1 / slots;
  const width = step * 0.58;
  const recent = intervals.slice(-slots);
  return recent.map((interval: number, index: number): Bar => {
    const age = (recent.length - 1 - index) / slots;
    return {
      id: lastId - (recent.length - 1 - index),
      x: 1 - (recent.length - index) * step + (step - width) / 2,
      width: width,
      height: Math.max(0.04, Math.min(interval / (periodMs * CHART_PERIODS), 1)),
      late: interval > periodMs * LATE_PERIODS,
      opacity: 1 - age * 0.55,
    };
  });
}

export function summarize(d: Diagnostics): Summary {
  const periodMs = d.refreshHz > 0 ? 1000 / d.refreshHz : 1000 / 60;
  const started = d.path.length > 0;
  const live = started && d.fps >= 2;
  const slots = Math.max(30, Math.round(1000 / periodMs));
  const recent = d.intervalsMs.slice(-slots);
  const late = recent.filter((interval: number): boolean => interval > periodMs * LATE_PERIODS).length;
  const facts: Fact[] = [];
  if (d.bufferWidth > 0) {
    facts.push({ text: `${d.bufferWidth} × ${d.bufferHeight} ${d.path === 'gpu' ? 'GPU' : 'shm'}`, warn: false });
  }
  if (d.pixelRatio > 0) {
    facts.push(Math.abs(d.pixelRatio - 1) < PIXEL_EXACT ? { text: 'pixel exact', warn: false } :
      { text: `stretched ${scaleText(d.pixelRatio)}`, warn: true });
  }
  return {
    hero: live ? fpsText(d.fps) : started ? 'Idle' : 'Waiting',
    live: live,
    state: d.refreshHz > 0 ? `${Math.round(d.refreshHz)} Hz` : '',
    stateColor: live ? DOT_LIVE : DOT_IDLE,
    bars: timeline(d.intervalsMs, d.presented, periodMs),
    note: !started ? 'Waiting for the first frame' : !live ? 'No new frames, the app is idle' :
      late === 0 ? 'Every frame on time' : `${late} held a refresh · ${d.worstFrameMs.toFixed(0)} ms worst`,
    noteWarn: live && late > 0,
    facts: facts,
    stats: [
      { value: d.inputMs >= 0 ? `${Math.round(d.inputMs)} ms` : '–', caption: 'Input to frame' },
      { value: d.rssBytes >= 0 ? `${Math.round(d.rssBytes / 1048576)} MiB` : '–', caption: 'Memory' },
    ],
  };
}

function logLine(d: Diagnostics): string {
  return `fps=${d.fps} refresh=${d.refreshHz.toFixed(1)} frame=${d.frameMs.toFixed(2)} ` +
    `worst=${d.worstFrameMs.toFixed(2)} input=${d.inputMs.toFixed(1)} path=${d.path} ` +
    `buffer=${d.bufferWidth}x${d.bufferHeight} scale=${d.clientScale.toFixed(2)} ` +
    `ratio=${d.pixelRatio.toFixed(3)} rss=${d.rssBytes}`;
}

// Reads the numbers while the overlay is on: the panel twice a second, the numeral every
// display frame. It stops the moment the user hides the panel, not when the panel's exit
// animation ends.
export class DiagnosticsSampler {
  private timer: number = -1;
  private last: Diagnostics | undefined = undefined;
  private sync: displaySync.DisplaySync | undefined = undefined;
  private shown: number = -1;
  private lastFrameNs: number = 0;

  start(pid: number, show: (summary: Summary) => void, showFps: (fps: number) => void): void {
    console.info('DIAG_OVERLAY on');
    const sample = () => {
      this.last = runtime.getDiagnostics(pid);
      console.info(`DIAG_OVERLAY ${logLine(this.last)}`);
      show(summarize(this.last));
    };
    sample();
    this.timer = setInterval(sample, SAMPLE_MS);
    this.sync = displaySync.create();
    this.sync.setExpectedFrameRateRange({ expected: 120, min: 30, max: 120 });
    this.sync.on('frame', (info: displaySync.IntervalInfo) => {
      const target = runtime.presentedFps();
      const elapsedMs = this.lastFrameNs > 0 ? (info.timestamp - this.lastFrameNs) / 1e6 : 0;
      this.lastFrameNs = info.timestamp;
      this.shown = this.shown < 0 ? target :
        this.shown + (target - this.shown) * (1 - Math.exp(-elapsedMs / NUMERAL_TAU_MS));
      showFps(Math.round(this.shown));
    });
    this.sync.start();
  }

  stop(): void {
    if (this.timer < 0) return;
    clearInterval(this.timer);
    this.timer = -1;
    this.sync?.stop();
    this.sync?.off('frame');
    this.sync = undefined;
    this.shown = -1;
    this.lastFrameNs = 0;
    console.info(`DIAG_OVERLAY off ${this.last ? logLine(this.last) : ''}`);
  }
}

// Frosted glass: a custom blur kept on when the window is not focused, a hairline edge and a
// floating shadow. The radius is set here and only here: a radius set after a blur style
// reshapes the edge but not the blur, which leaves two curves. The DP monitor's glass is 65%
// dark over video; apps can be light, so this one is 85% dark and stays a dark instrument.
@Extend(Column)
function glass(radius: number) {
  .backgroundEffect({
    radius: 80,
    saturation: 1.4,
    color: '#D9111113',
    policy: BlurStyleActivePolicy.ALWAYS_ACTIVE,
  })
  .border({ width: '1px', color: '#2EFFFFFF' })
  .borderRadius(radius)
  .shadow(ShadowStyle.OUTER_FLOATING_MD)
}

// Groups sit on surfaces, not rules: one step above the glass, corners concentric with it.
@Styles
function surfaceGroup() {
  .width('100%')
  .padding(GROUP_PADDING)
  .borderRadius(GROUP_RADIUS)
  .backgroundColor(HOVER)
}

const NO_READING: Diagnostics = {
  fps: 0, presented: 0, refreshHz: 0, frameMs: 0, worstFrameMs: 0, intervalsMs: [], inputMs: -1, path: '',
  bufferWidth: 0, bufferHeight: 0, clientScale: 0, pixelRatio: 0, rssBytes: -1,
};

// The live numbers Ctrl+Alt+Shift+D shows over the app, the DP monitor's meter on its glass:
// frames per second over a bar per presented frame, a verdict and the buffer, then the two
// quieter numbers. It passes every touch and click through to the app and never takes focus.
@Component
export struct DiagnosticsOverlay {
  @Prop @Watch('onSummary') summary: Summary = summarize(NO_READING);
  // The numeral, read every display frame; -1 shows the summary's.
  @Prop fps: number = -1;
  // Off for offscreen renders (the gallery), which have no frames to animate on.
  @Prop animated: boolean = true;
  // The strip's offset while it glides; the newest frame number it last drew.
  @State private glide: number = 0;
  private newestId: number = -1;

  // New frames arrive on the right: start the strip that many slots right of where it now
  // draws, then slide it home over one sample, so the timeline moves at the rate frames
  // arrive instead of jumping twice a second.
  onSummary(): void {
    const bars = this.summary.bars;
    const newest = bars.length > 0 ? bars[bars.length - 1].id : -1;
    const added = this.newestId >= 0 && newest > this.newestId ? newest - this.newestId : 0;
    this.newestId = newest;
    if (!this.animated || added === 0 || bars.length < 2) return;
    const step = (bars[1].x - bars[0].x) * CHART_WIDTH;
    this.glide = Math.min(added * step, CHART_WIDTH);
    animateTo({ duration: SAMPLE_MS, curve: Curve.Linear }, () => {
      this.glide = 0;
    });
  }

  @Builder
  Chart() {
    Stack({ alignContent: Alignment.TopStart }) {
      // The guide: one display period. A bar above it held the previous picture an extra refresh.
      Column()
        .width(CHART_WIDTH)
        .height('1px')
        .backgroundColor(HAIRLINE)
        .position({ x: 0, y: this.summary.live ? CHART_HEIGHT * (1 - 1 / CHART_PERIODS) : CHART_HEIGHT - 1 })
      if (this.summary.live) {
        Stack({ alignContent: Alignment.TopStart }) {
          ForEach(this.summary.bars, (bar: Bar) => {
            Column()
              .width(bar.width * CHART_WIDTH)
              .height(bar.height * CHART_HEIGHT)
              .borderRadius({ topLeft: 1, topRight: 1 })
              .backgroundColor(bar.late ? WARN : INK)
              .opacity(bar.late ? 1 : bar.opacity * 0.5)
              .position({ x: bar.x * CHART_WIDTH, y: CHART_HEIGHT * (1 - bar.height) })
          }, (bar: Bar) => `${bar.id}|${bar.x.toFixed(4)}|${bar.height.toFixed(3)}|${bar.late}`)
        }
        .width(CHART_WIDTH)
        .height(CHART_HEIGHT)
        .translate({ x: this.glide })
      }
    }
    .width(CHART_WIDTH)
    .height(CHART_HEIGHT)
    .clip(true)
  }

  @Builder
  StatView(stat: Stat) {
    Column({ space: 2 }) {
      Text(stat.value)
        .fontSize(TYPE_SUBTITLE)
        .fontWeight(WEIGHT_SEMIBOLD)
        .fontColor(stat.value === '–' ? INK_MUTED : INK)
        .fontFeature(TABULAR)
        .maxLines(1)
      Text(stat.caption)
        .fontSize(TYPE_MICRO)
        .fontColor(INK_MUTED)
        .maxLines(1)
    }
    .layoutWeight(1)
    .alignItems(HorizontalAlign.Start)
  }

  build() {
    Column({ space: GROUP_GAP }) {
      Column() {
        Row() {
          Text(this.summary.live && this.fps >= 0 ? `${this.fps}` : this.summary.hero)
            .fontSize(this.summary.live ? 34 : 22)
            .fontWeight(WEIGHT_SEMIBOLD)
            .fontColor(this.summary.live ? INK : INK_MUTED)
            .fontFeature(TABULAR)
            .letterSpacing(this.summary.live ? -0.6 : 0)
          if (this.summary.live) {
            Text('fps')
              .fontSize(TYPE_BODY)
              .fontColor(INK_MUTED)
              .margin({ left: 4, bottom: 6 })
          }
          Blank()
          if (this.summary.state.length > 0) {
            Row({ space: 6 }) {
              Circle()
                .width(6)
                .height(6)
                .fill(this.summary.stateColor)
              Text(this.summary.state)
                .fontSize(TYPE_MICRO)
                .fontColor(INK_MUTED)
                .fontFeature(TABULAR)
                .maxLines(1)
            }
            .margin({ bottom: 8 })
          }
        }
        .width('100%')
        .height(40)
        .alignItems(VerticalAlign.Bottom)

        Column() {
          this.Chart()
        }
        .margin({ top: 8 })

        Text(this.summary.note)
          .fontSize(TYPE_CAPTION)
          .lineHeight(16)
          .fontColor(this.summary.noteWarn ? WARN : INK)
          .fontFeature(TABULAR)
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
          .margin({ top: 8 })
        Text() {
          ForEach(this.summary.facts, (fact: Fact, index: number) => {
            Span(index === 0 ? fact.text : ` · ${fact.text}`)
              .fontColor(fact.warn ? WARN : INK_MUTED)
          }, (fact: Fact, index: number) => `${index}|${fact.text}|${fact.warn}`)
        }
        .fontSize(TYPE_MICRO)
        .lineHeight(14)
        .fontFeature(TABULAR)
        .maxLines(1)
        .textOverflow({ overflow: TextOverflow.Ellipsis })
        .visibility(this.summary.facts.length > 0 ? Visibility.Visible : Visibility.Hidden)
      }
      .surfaceGroup()
      .alignItems(HorizontalAlign.Start)

      Row({ space: GROUP_PADDING }) {
        this.StatView(this.summary.stats[0])
        this.StatView(this.summary.stats[1])
      }
      .surfaceGroup()
    }
    .width(PANEL_WIDTH)
    .padding(PANEL_INSET)
    .glass(PANEL_RADIUS)
    .focusable(false)
    .hitTestBehavior(HitTestMode.None)
  }
}

// Visual QA: the panel in its named states over a dark and a light app,
// rendered offscreen (Index.ets `overlay-gallery`). These readings are made
// up and never reach the live panel.
export interface GalleryState {
  name: string;
  reading: Diagnostics;
}

function paced(count: number, periodMs: number, lateEvery: number): number[] {
  const intervals: number[] = [];
  for (let i = 0; i < count; ++i) {
    intervals.push(lateEvery > 0 && i % lateEvery === lateEvery - 1 ? periodMs * (2 + (i % 3)) : periodMs);
  }
  return intervals;
}

export const GALLERY_STATES: GalleryState[] = [
  { name: 'smooth', reading: { fps: 89, presented: 4000, refreshHz: 90, frameMs: 11.2,
    worstFrameMs: 12.4, intervalsMs: paced(88, 11.1, 0), inputMs: 9, path: 'gpu', bufferWidth: 2102,
    bufferHeight: 1332, clientScale: 2, pixelRatio: 1, rssBytes: 167772160 } },
  { name: 'stutter', reading: { fps: 65, presented: 4000, refreshHz: 90, frameMs: 15.4,
    worstFrameMs: 48.9, intervalsMs: paced(64, 11.1, 7), inputMs: 18, path: 'gpu', bufferWidth: 2102,
    bufferHeight: 1332, clientScale: 2, pixelRatio: 1, rssBytes: 167772160 } },
  { name: 'idle-stretched', reading: { fps: 0, presented: 4000, refreshHz: 90, frameMs: 0,
    worstFrameMs: 0, intervalsMs: [], inputMs: -1, path: 'gpu', bufferWidth: 1001, bufferHeight: 633,
    clientScale: 1, pixelRatio: 2, rssBytes: 46137344 } },
  { name: 'waiting', reading: NO_READING },
];

@Builder
export function DiagnosticsGallery(state: GalleryState, mode: ThemeColorMode) {
  WithTheme({ colorMode: mode }) {
    Stack({ alignContent: Alignment.BottomEnd }) {
      Column({ space: 6 }) {
        ForEach(['$ cargo build --release', '   Compiling wayland-client v0.31', '   Compiling smithay v0.3',
          '   Finished release in 41.2 s', '$ ./target/release/app --verbose', 'listening on wayland-0'],
          (line: string) => {
            Text(line)
              .fontSize(13)
              .fontFamily('monospace')
              .fontColor(mode === ThemeColorMode.DARK ? '#C8CCD4' : '#2B2D31')
          }, (line: string) => line)
      }
      .width('100%')
      .height('100%')
      .padding(16)
      .alignItems(HorizontalAlign.Start)
      .backgroundColor(mode === ThemeColorMode.DARK ? '#16181C' : '#F4F4F2')

      DiagnosticsOverlay({ summary: summarize(state.reading), animated: false })
        .margin(12)
    }
    .width(360)
    .height(320)
  }
}
