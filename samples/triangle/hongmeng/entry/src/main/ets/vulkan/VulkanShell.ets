import { hilog } from '@kit.PerformanceAnalysisKit';

interface NativeBridge {
  onShutdown(): void;
}

let g_bridge: NativeBridge | null = null;

/** Ability 销毁时仍可经任一模块句柄释放全部固定 Surface 槽位。 */
export function nativeBridge(): NativeBridge | null {
  return g_bridge;
}

/**
 * 单个 XComponent 的薄壳。componentId 与 native 固定槽位一一对应；静态红色
 * Surface 在 native 创建和尺寸变化时绘制，ArkUI 负责后续平移、滚动和菜单动画。
 */
@Component
export struct VulkanShell {
  @Prop componentId: string = '';
  @Prop surfaceId: number = 0;

  build() {
    XComponent({ id: this.componentId, type: XComponentType.SURFACE, libraryname: 'estarx_vulkan' })
      .width('100%')
      .height('100%')
      .onLoad((context?: object) => {
        if (!context) {
          return;
        }
        g_bridge = context as NativeBridge;
        hilog.info(0x0000, 'estarx-vulkan',
          `[svk][surface_component_loaded id=${this.surfaceId}]`);
      })
  }
}
