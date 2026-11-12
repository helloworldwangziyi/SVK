import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';

const TAG = 'estarx-vulkan';
const DOMAIN = 0x0000;

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(DOMAIN, TAG, '[svk][lifecycle][ability_created]');
  }

  onDestroy(): void {
    hilog.info(DOMAIN, TAG, '[svk][lifecycle][ability_destroyed]');
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    // 边到边布局：内容延伸到状态栏/手势条之下（对应 Android 的
    // WindowCompat.setDecorFitsSystemWindows(false)）。
    windowStage.getMainWindowSync().setWindowLayoutFullScreen(true);

    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, TAG,
          '[svk][lifecycle][page_load_failed reason=%{public}s]',
          JSON.stringify(err) ?? '');
        return;
      }
      hilog.info(DOMAIN, TAG, '[svk][lifecycle][page_loaded]');
    });
  }

  onWindowStageDestroy(): void {
    hilog.info(DOMAIN, TAG, '[svk][lifecycle][window_stage_destroyed]');
  }

  onForeground(): void {
    hilog.info(DOMAIN, TAG, '[svk][lifecycle][entered_foreground]');
  }

  onBackground(): void {
    // 退后台：引擎侧 surface 级挂起由 XComponent OnSurfaceDestroyed 驱动
    // （保活，引擎级资源保留）。
    hilog.info(DOMAIN, TAG, '[svk][lifecycle][entered_background]');
  }
}
