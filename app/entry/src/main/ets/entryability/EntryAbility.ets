/*
 * Copyright (c) 2026 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import { AbilityConstant, ConfigurationConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { BusinessError } from '@kit.BasicServicesKit';

const DOMAIN = 0x0000;

// The HAP runs the app in rawfile/app.json. A testing HAP also takes
// `aa start ... --ps test <name>` to run one of its named tests.
function publishLaunchRequest(want: Want): void {
  const test = want.parameters?.['test'];
  AppStorage.setOrCreate<string>('launchTest', typeof test === 'string' ? test : '');
  // `--ps capture 1` writes the next composed frame to cache/ohos_wl_capture.ppm
  // without restarting the app (ohos_wl.py screenshot).
  const capture = want.parameters?.['capture'];
  AppStorage.setOrCreate<boolean>('launchCapture', typeof capture === 'string' && capture.length > 0);
  AppStorage.setOrCreate<number>('launchSerial', (AppStorage.get<number>('launchSerial') ?? 0) + 1);
}

export default class EntryAbility extends UIAbility {
  onNewWant(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    publishLaunchRequest(want);
  }

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    publishLaunchRequest(want);
    hilog.info(DOMAIN, 'testTag', 'APP_START reason=%{public}d lastExit=%{public}d %{public}s',
      launchParam.launchReason, launchParam.lastExitReason, launchParam.lastExitMessage ?? '');
    try {
      this.context.getApplicationContext().setColorMode(ConfigurationConstant.ColorMode.COLOR_MODE_NOT_SET);
    } catch (err) {
      hilog.error(DOMAIN, 'testTag', 'Failed to set colorMode. Cause: %{public}s', JSON.stringify(err));
    }
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onCreate');
  }

  onDestroy(): void {
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onDestroy');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    // Main window is created, set main page for this ability
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onWindowStageCreate');
    try {
      windowStage.getMainWindowSync().setWindowKeepScreenOn(true);
      AppStorage.setOrCreate<window.Window>('mainWindow', windowStage.getMainWindowSync());
    } catch (err) {
      hilog.error(DOMAIN, 'testTag', 'Failed to keep screen on: %{public}s', JSON.stringify(err));
    }

    // HarmonyOS reopens the main window at the size, position, and maximized
    // state it had when it last closed. Off by default; free-window devices only.
    windowStage.setWindowRectAutoSave(true).then(() => {
      hilog.info(DOMAIN, 'testTag', 'WINDOW_RECT_AUTOSAVE on');
    }).catch((err: BusinessError) => {
      hilog.warn(DOMAIN, 'testTag', 'WINDOW_RECT_AUTOSAVE %{public}s', JSON.stringify(err));
    });

    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, 'testTag', 'Failed to load the content. Cause: %{public}s', JSON.stringify(err));
        return;
      }
      hilog.info(DOMAIN, 'testTag', 'Succeeded in loading the content.');
    });
  }

  onWindowStageDestroy(): void {
    // Main window is destroyed, release UI related resources
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onWindowStageDestroy');
  }

  onForeground(): void {
    // Ability has brought to foreground
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onForeground');
  }

  onBackground(): void {
    // Ability has back to background
    hilog.info(DOMAIN, 'testTag', '%{public}s', 'Ability onBackground');
  }
}