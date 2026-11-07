import runtime from 'libentry.so';
import { fileIo } from '@kit.CoreFileKit';

// ArkUI_CoastingAxisEventPhase (arkui/ui_input_event.h).
export const COAST_BEGIN = 1;
export const COAST_UPDATE = 2;
export const COAST_END = 3;

const SOURCE_TOUCHPAD = 1;
// ArkUI decides to coast while handling the lift and sends the first
// coasting event on a following frame, so the stop waits this long for it.
const COAST_WAIT_MS = 50;
// HarmonyOS reports finger travel in libinput's units, and native ArkUI
// moves content 1:1 with it. Wayland clients multiply it for their own feel,
// as they do on Linux: GTK lists 2.5, Alacritty 2.7, Zed 3, Ghostty 9.5
// (tools/scroll-measure). One gain for every client is the inverse of the
// median, which brings most apps to native speed.
const FINGER_GAIN = 0.35;

// One two-finger touchpad scroll, as HarmonyOS reports it to a native
// component: finger travel while the fingers move, then, after a fling,
// ArkUI's coasting events, the platform's own momentum. A Wayland client
// sees both as axis_source finger, and one axis_stop when the content has
// come to rest.
export class TouchpadScroll {
  // Wayland logical pixels per vp of finger travel.
  scale: number = 1;
  private x: number = 0;
  private y: number = 0;
  private step: number = 0;
  private stopTimer: number = -1;
  // Prototype knobs, read from cache/ohos_wl_scroll_tuning at each lift.
  private gain: number = FINGER_GAIN;
  private platformCoasting: boolean = true;
  private tuningPath: string;

  constructor(cacheDir: string) {
    this.tuningPath = `${cacheDir}/ohos_wl_scroll_tuning`;
    this.readTuning();
  }

  move(horizontal: number, vertical: number, step: number, x: number, y: number): void {
    this.cancelStop();
    this.x = x;
    this.y = y;
    this.step = step;
    this.send(horizontal, vertical);
  }

  lift(): void {
    this.cancelStop();
    if (!this.platformCoasting) {
      this.stop();
      return;
    }
    this.stopTimer = setTimeout(() => this.stop(), COAST_WAIT_MS);
  }

  coast(phase: number, horizontal: number, vertical: number): void {
    if (!this.platformCoasting) return;
    this.cancelStop();
    if (phase === COAST_END) {
      this.stop();
      return;
    }
    this.send(horizontal, vertical);
  }

  private send(horizontal: number, vertical: number): void {
    const factor = this.scale * this.gain;
    if (vertical !== 0) {
      runtime.sendScrollEvent(0, vertical * factor, this.step, this.x, this.y, SOURCE_TOUCHPAD, false);
    }
    if (horizontal !== 0) {
      runtime.sendScrollEvent(1, horizontal * factor, this.step, this.x, this.y, SOURCE_TOUCHPAD, false);
    }
  }

  private stop(): void {
    this.stopTimer = -1;
    runtime.sendScrollEvent(0, 0, this.step, this.x, this.y, SOURCE_TOUCHPAD, true);
    runtime.sendScrollEvent(1, 0, this.step, this.x, this.y, SOURCE_TOUCHPAD, true);
    this.readTuning();
  }

  private cancelStop(): void {
    if (this.stopTimer === -1) return;
    clearTimeout(this.stopTimer);
    this.stopTimer = -1;
  }

  private readTuning(): void {
    try {
      if (!fileIo.accessSync(this.tuningPath)) return;
      const words = fileIo.readTextSync(this.tuningPath).trim().split(' ');
      this.gain = Number(words[0]);
      this.platformCoasting = words[1] !== 'client';
      console.info(`SCROLL_TUNING gain=${this.gain} coasting=${this.platformCoasting ? 'platform' : 'client'}`);
    } catch (error) {
      console.warn(`SCROLL_TUNING unreadable ${JSON.stringify(error)}`);
    }
  }
}
