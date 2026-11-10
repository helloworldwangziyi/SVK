import { webview } from '@kit.ArkWeb';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { VulkanShell } from '../vulkan/VulkanShell';

const TAG = 'SVK-SurfaceProbe';
const DOMAIN = 0x0000;

function referenceWebUrl(path: string): string {
  // URL 与参数格式取自参考项目 K 线页；合约取 market_catalog.h 的 CFFEX|F|IF。
  return 'https://news.epolestar.xyz/' + path +
    '?isEstarX=1&contractNo=CFFEX%7CF%7CIF&theme=0&contractName=IF';
}

/**
 * 鸿蒙专用联动探针：Header 和底部页面均为 TopBar 下的完整视口，Tab 初始位于
 * 屏幕外。网页页禁用外层 Swiper，横向手势归 ArkWeb；其他区域才切换页面。
 */
@Entry
@Component
struct Index {
  private outerScroller: Scroller = new Scroller();
  private nativeScroller: Scroller = new Scroller();
  private swiperController: SwiperController = new SwiperController();
  private noticeController: webview.WebviewController = new webview.WebviewController();
  private relevanceController: webview.WebviewController = new webview.WebviewController();
  private rankController: webview.WebviewController = new webview.WebviewController();
  private f10Controller: webview.WebviewController = new webview.WebviewController();
  private topBarHeight: number = 56;
  private tabHeight: number = 56;
  @State private activePage: number = 0;
  @State private drawerProgress: number = 0;
  @State private sceneWidth: number = 360;
  @State private sceneHeight: number = 640;

  private contentHeight(): number {
    return Math.max(1, this.sceneHeight - this.topBarHeight);
  }

  private drawerWidth(): number {
    return Math.max(1, this.sceneWidth * 0.25);
  }

  private selectPage(index: number): void {
    this.swiperController.changeIndex(index, true);
    hilog.info(DOMAIN, TAG, `[event=page_change_requested page=${index}]`);
  }

  private animateDrawer(target: number): void {
    // 先无动画落定当前页，避免菜单和 Swiper 两套动画同时驱动内容。
    this.swiperController.changeIndex(this.activePage, false);
    hilog.info(DOMAIN, TAG,
      `[event=drawer_start target=${target} drawer_width=${this.drawerWidth()}]`);
    animateTo({ duration: 260, curve: Curve.EaseOut }, () => {
      this.drawerProgress = target;
    });
  }

  private horizontalTabGesture(): PanGestureInterface {
    return PanGesture({ direction: PanDirection.Horizontal })
      .onActionEnd((event: GestureEvent) => {
        if (Math.abs(event.offsetX) < 40) {
          return;
        }
        const next = event.offsetX < 0
          ? Math.min(4, this.activePage + 1)
          : Math.max(0, this.activePage - 1);
        this.selectPage(next);
      });
  }

  @Builder
  private webPage(url: string, controller: webview.WebviewController, page: number) {
    Web({ src: url, controller: controller })
      .width('100%')
      .height('100%')
      .javaScriptAccess(true)
      .domStorageAccess(true)
      .fileAccess(false)
      .mixedMode(MixedMode.None)
      .overScrollMode(OverScrollMode.NEVER)
      .nestedScroll({
        scrollForward: NestedScrollMode.PARENT_FIRST,
        scrollBackward: NestedScrollMode.SELF_FIRST
      })
      .onPageEnd(() => {
        hilog.info(DOMAIN, TAG, `[event=web_loaded page=${page}]`);
      })
      .onErrorReceive(() => {
        hilog.warn(DOMAIN, TAG, `[event=web_load_failed page=${page}]`);
      })
  }

  @Builder
  private tabButton(title: string, page: number) {
    Button(title)
      .layoutWeight(1)
      .fontSize(12)
      .fontColor(this.activePage === page ? Color.Red : Color.Gray)
      .onClick(() => this.selectPage(page))
  }

  build() {
    Stack({ alignContent: Alignment.TopStart }) {
      Stack({ alignContent: Alignment.TopStart }) {
        Scroll(this.outerScroller) {
          Column() {
            Stack({ alignContent: Alignment.Bottom }) {
              VulkanShell({ componentId: 'estarx-header', surfaceId: 0 })
                .width('100%')
                .height(this.contentHeight())

              Row() {
                this.tabButton('资讯', 0)
                this.tabButton('关联', 1)
                this.tabButton('排行', 2)
                this.tabButton('F10', 3)
                this.tabButton('绿色', 4)
              }
              .width('100%')
              .height(this.tabHeight)
              .backgroundColor(Color.White)
            }
            .width('100%')
            .height(this.contentHeight())
            .gesture(this.horizontalTabGesture())

            Swiper(this.swiperController) {
              this.webPage(referenceWebUrl('notice'), this.noticeController, 0)
              this.webPage(referenceWebUrl('relevance'), this.relevanceController, 1)
              this.webPage(referenceWebUrl('rank'), this.rankController, 2)
              this.webPage(referenceWebUrl('f10'), this.f10Controller, 3)

              Scroll(this.nativeScroller) {
                VulkanShell({ componentId: 'estarx-native', surfaceId: 1 })
                  .width('100%')
                  .height(this.contentHeight() * 2)
              }
              .width('100%')
              .height('100%')
              .scrollable(ScrollDirection.Vertical)
              .scrollBar(BarState.Off)
              .edgeEffect(EdgeEffect.None)
              .nestedScroll({
                scrollForward: NestedScrollMode.PARENT_FIRST,
                scrollBackward: NestedScrollMode.SELF_FIRST
              })
            }
            .width('100%')
            .height(this.contentHeight())
            .vertical(false)
            .loop(false)
            .indicator(false)
            .cachedCount(4)
            .duration(220)
            // 网页内部可能有自己的横向场景，只有绿色 Vulkan 页允许 Swiper 接管。
            .disableSwipe(this.activePage !== 4 || this.drawerProgress > 0)
            .onAnimationEnd((index: number) => {
              this.activePage = index;
              hilog.info(DOMAIN, TAG, `[event=page_changed page=${index}]`);
            })
          }
        }
        .width('100%')
        // Tab 覆盖在 Header 内不占额外布局高度；Scroll 视口比可见区多一个
        // Tab 高，使自然最大外层偏移 = Header 高 - Tab 高，恰好停在 TopBar 下。
        .height(this.contentHeight() + this.tabHeight)
        .position({ x: 0, y: this.topBarHeight })
        .scrollable(ScrollDirection.Vertical)
        .scrollBar(BarState.Off)
        .edgeEffect(EdgeEffect.None)

        Row() {
          Text('Surface Probe')
            .fontSize(16)
            .fontColor(Color.Gray)
            .layoutWeight(1)
          Button('MENU')
            .width(88)
            .height('100%')
            .onClick(() => this.animateDrawer(1))
        }
        .width('100%')
        .height(this.topBarHeight)
        .backgroundColor(Color.White)
        .position({ x: 0, y: 0 })
        .gesture(this.horizontalTabGesture())
      }
      .width('100%')
      .height('100%')
      .enabled(this.drawerProgress === 0)
      .translate({ x: -this.drawerWidth() * this.drawerProgress, y: 0 })

      if (this.drawerProgress > 0) {
        Column()
          .width('100%')
          .height('100%')
          .backgroundColor(Color.Transparent)
          .translate({ x: -this.drawerWidth() * this.drawerProgress, y: 0 })
          .onClick(() => this.animateDrawer(0))
      }

      // 菜单始终挂载并提前创建 Surface，避免第一次点击时才初始化 Vulkan。
      Column() {
        VulkanShell({ componentId: 'estarx-menu', surfaceId: 2 })
          .width('100%')
          .height('100%')
      }
      .width(this.drawerWidth())
      .height(this.sceneHeight)
      .position({ x: this.sceneWidth - this.drawerWidth() * this.drawerProgress, y: 0 })
    }
    .width('100%')
    .height('100%')
    .clip(true)
    .expandSafeArea([SafeAreaType.SYSTEM, SafeAreaType.CUTOUT],
      [SafeAreaEdge.TOP, SafeAreaEdge.BOTTOM, SafeAreaEdge.START, SafeAreaEdge.END])
    .onAreaChange((_old: Area, area: Area) => {
      this.sceneWidth = Number(area.width);
      this.sceneHeight = Number(area.height);
      hilog.info(DOMAIN, TAG,
        `[event=scene_resize width=${this.sceneWidth} height=${this.sceneHeight}]`);
    })
  }
}
