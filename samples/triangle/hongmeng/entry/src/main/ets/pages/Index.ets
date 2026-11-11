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
 * 屏幕外。Swiper 只执行切页动画；内容区观察原始触摸序列，在抬手后按已锁定
 * 的归属切页，不在异步 DOM 检测期间改变命中链或重新启用内置手势。
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
  // 触摸与 DOM 回调均在 UI 线程顺序处理。每个 DOWN、切页、菜单和文档重载
  // 都使旧检测失效；page/session 同时校验，防止旧网页结果触发新页面切换。
  private touchGeneration: number = 0;
  private webSessions: number[] = [0, 0, 0, 0];
  private touchPage: number = -1;
  private touchId: number = -1;
  private touchDownX: number = 0;
  private touchDownY: number = 0;
  private touchDownTime: number = 0;
  private touchElapsed: number = 1;
  private touchOffsetX: number = 0;
  private touchEnded: boolean = false;
  private touchDirection: string = 'none';
  private touchOwner: string = 'none';
  private webHorizontalInteractive: boolean | null = null;
  private pagerAnimating: boolean = false;
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

  private webHeight(): number {
    return Math.max(1, this.contentHeight() - this.tabHeight);
  }

  private invalidateTouch(reason: string): void {
    if (this.touchPage >= 0) {
      hilog.debug(DOMAIN, TAG,
        `[event=gesture_end reason=${reason} page=${this.touchPage} gesture_owner=${this.touchOwner}]`);
    }
    ++this.touchGeneration;
    this.touchPage = -1;
    this.touchId = -1;
    this.touchEnded = false;
    this.touchDirection = 'none';
    this.touchOwner = 'none';
    this.webHorizontalInteractive = null;
  }

  private probeWebHorizontal(controller: webview.WebviewController, page: number,
    x: number, y: number): void {
    const generation = this.touchGeneration;
    const session = this.webSessions[page];
    // TouchObject 为 Web 局部 vp；DOM 命中使用 CSS 视口坐标，按实际视口宽度换算，
    // 不把设备像素比当成页面缩放。命中链沿用 Android 的列表/Canvas/Chart 规则。
    const script = '(function(){var v=window.visualViewport;var scale=' +
      '(v?v.width:window.innerWidth)/' + this.sceneWidth + ';' +
      'var e=document.elementFromPoint(' + x + '*scale,' + y + '*scale);' +
      'if(!e)return true;while(e){var s=getComputedStyle(e);' +
      'var marker=(e.id+" "+String(e.className)).toLowerCase();' +
      'if(e.tagName==="CANVAS"||e.tagName==="IFRAME"||marker.indexOf("chart")>=0)return true;' +
      'if(e.scrollWidth>e.clientWidth+1&&(s.overflowX==="auto"||' +
      's.overflowX==="scroll"||e===document.documentElement))return true;' +
      'e=e.parentElement;}return false;})()';
    controller.runJavaScript(script).then((value: string) => {
      if (generation !== this.touchGeneration || session !== this.webSessions[page] ||
        page !== this.activePage) {
        hilog.debug(DOMAIN, TAG,
          `[event=web_horizontal_probe_stale page=${page} session_id=${session} generation=${generation}]`);
        return;
      }
      // 检测失败或未知结果整段交还网页，不猜测并抢走交互。
      this.webHorizontalInteractive = value !== 'false';
      hilog.debug(DOMAIN, TAG,
        `[event=web_horizontal_probe page=${page} session_id=${session} generation=${generation} interactive=${this.webHorizontalInteractive}]`);
      this.resolveTouchOwner();
    }).catch(() => {
      if (generation !== this.touchGeneration || session !== this.webSessions[page]) {
        return;
      }
      this.webHorizontalInteractive = true;
      hilog.warn(DOMAIN, TAG,
        `[event=web_horizontal_probe_failed page=${page} session_id=${session} reason=javascript_failed action=keep_web_gesture]`);
      this.resolveTouchOwner();
    });
  }

  private resolveTouchOwner(): void {
    if (this.touchOwner !== 'pending' || this.touchDirection !== 'horizontal' ||
      this.webHorizontalInteractive === null) {
      return;
    }
    this.touchOwner = this.webHorizontalInteractive ? 'web_horizontal' : 'tab';
    hilog.debug(DOMAIN, TAG,
      `[event=gesture_lock page=${this.touchPage} generation=${this.touchGeneration} gesture_owner=${this.touchOwner}]`);
    if (this.touchEnded) {
      this.finishPageTouch();
    }
  }

  private finishPageTouch(): void {
    if (this.touchOwner === 'pending' && this.touchDirection === 'horizontal') {
      // 快滑可能先 UP 后收到 DOM 结果。保留本次位移，回调仍可完成同一次切页；
      // 下一次 DOWN/Cancel/页面变化会失效它，不要求用户再滑一次。
      return;
    }
    const offset = this.touchOffsetX;
    const shouldSwitch = this.touchOwner === 'tab' &&
      (Math.abs(offset) >= 40 || Math.abs(offset) / this.touchElapsed * 1000 >= 800);
    const target = Math.max(0, Math.min(4, this.touchPage + (offset < 0 ? 1 : -1)));
    this.invalidateTouch('up');
    if (shouldSwitch) {
      this.selectPage(target);
    }
  }

  private observePageTouch(event: TouchEvent, page: number,
    controller: webview.WebviewController | null): void {
    if (event.type === TouchType.Cancel || event.touches.length > 1) {
      this.invalidateTouch(event.type === TouchType.Cancel ? 'cancel' : 'multi_touch');
      return;
    }
    const point = event.changedTouches[0];
    if (!point) {
      return;
    }
    if (event.type === TouchType.Down) {
      this.invalidateTouch('down');
      if (page !== this.activePage || this.pagerAnimating || this.drawerProgress > 0) {
        hilog.debug(DOMAIN, TAG,
          `[event=gesture_ignored page=${page} active_page=${this.activePage} reason=${this.pagerAnimating ? 'page_animation' : 'inactive_page_or_drawer'}]`);
        return;
      }
      this.touchPage = page;
      this.touchId = point.id;
      this.touchDownX = point.x;
      this.touchDownY = point.y;
      this.touchDownTime = Date.now();
      this.touchOffsetX = 0;
      this.touchOwner = 'pending';
      if (controller) {
        this.probeWebHorizontal(controller, page, point.x, point.y);
      } else {
        this.webHorizontalInteractive = false;
      }
      return;
    }
    if (this.touchPage !== page || this.touchId !== point.id) {
      return;
    }
    this.touchOffsetX = point.x - this.touchDownX;
    const dy = point.y - this.touchDownY;
    if (this.touchDirection === 'none' && Math.max(Math.abs(this.touchOffsetX), Math.abs(dy)) > 8) {
      this.touchDirection = Math.abs(this.touchOffsetX) > Math.abs(dy) ? 'horizontal' : 'vertical';
      if (this.touchDirection === 'vertical') {
        this.touchOwner = 'vertical';
        hilog.debug(DOMAIN, TAG,
          `[event=gesture_lock page=${page} generation=${this.touchGeneration} gesture_owner=vertical]`);
      }
    }
    this.resolveTouchOwner();
    if (event.type === TouchType.Up) {
      this.touchElapsed = Math.max(1, Date.now() - this.touchDownTime);
      this.touchEnded = true;
      this.finishPageTouch();
    }
  }

  private selectPage(index: number): void {
    if (this.pagerAnimating) {
      hilog.debug(DOMAIN, TAG,
        `[event=page_change_ignored page=${index} reason=page_animation]`);
      return;
    }
    if (index === this.activePage || this.drawerProgress > 0) {
      return;
    }
    const animate = Math.abs(index - this.activePage) === 1;
    this.invalidateTouch('page_change');
    this.pagerAnimating = animate;
    // activePage 只表示已落定页面。动画期间屏蔽新内容手势，Web 与绿色页共用
    // 同一规则；跨多页直接到位，不让中间 WebView/Surface 逐个穿过。
    if (!animate) {
      this.activePage = index;
    }
    this.swiperController.changeIndex(index, animate);
    hilog.info(DOMAIN, TAG,
      `[event=page_change_requested page=${index} animate=${animate}]`);
  }

  private animateDrawer(target: number): void {
    this.invalidateTouch('drawer');
    this.pagerAnimating = false;
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
    // Swiper 只拉伸包装页，ArkWeb 的实际视口须扣除停靠后的 Tab；包装页仍占满
    // contentHeight，保持外层最大偏移 = Header 高 - Tab 高的既有联动关系。
    Column() {
      Web({ src: url, controller: controller })
        .width('100%')
        .height(this.webHeight())
        .javaScriptAccess(true)
        .domStorageAccess(true)
        .fileAccess(false)
        .mixedMode(MixedMode.None)
        .overScrollMode(OverScrollMode.NEVER)
        .nestedScroll({
          scrollForward: NestedScrollMode.PARENT_FIRST,
          scrollBackward: NestedScrollMode.SELF_FIRST
        })
        .onTouch((event: TouchEvent) => this.observePageTouch(event, page, controller))
        .onPageBegin(() => {
          ++this.webSessions[page];
          if (page === this.touchPage) {
            this.invalidateTouch('web_navigation');
          }
        })
        .onPageEnd(() => {
          hilog.info(DOMAIN, TAG,
            `[event=web_loaded page=${page} session_id=${this.webSessions[page]} height=${this.webHeight()}]`);
        })
        .onErrorReceive(() => {
          hilog.warn(DOMAIN, TAG, `[event=web_load_failed page=${page}]`);
        })
    }
    .width('100%')
    .height(this.contentHeight())
  }

  onPageHide(): void {
    this.invalidateTouch('page_hide');
  }

  aboutToDisappear(): void {
    this.invalidateTouch('destroy');
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
            .onTouch((event: TouchEvent) => {
              if (event.type === TouchType.Down) {
                this.invalidateTouch('header_down');
              }
            })
            .priorityGesture(this.horizontalTabGesture())

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
              .onTouch((event: TouchEvent) => this.observePageTouch(event, 4, null))
            }
            .width('100%')
            .height(this.contentHeight())
            .vertical(false)
            .loop(false)
            .indicator(false)
            .cachedCount(4)
            .duration(220)
            // 从 DOWN 到 UP 不修改命中方式。网页继续处理其原生滚动/Canvas，
            // 观察者仅在普通区域的横滑结束后 changeIndex，不另起滚动或惯性。
            .disableSwipe(true)
            .onAnimationEnd((index: number) => {
              this.activePage = index;
              this.pagerAnimating = false;
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
        .onTouch((event: TouchEvent) => {
          if (event.type === TouchType.Down) {
            this.invalidateTouch('top_bar_down');
          }
        })
        .priorityGesture(this.horizontalTabGesture())
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
      this.invalidateTouch('resize');
      this.sceneWidth = Number(area.width);
      this.sceneHeight = Number(area.height);
      hilog.info(DOMAIN, TAG,
        `[event=scene_resize width=${this.sceneWidth} height=${this.sceneHeight}]`);
    })
  }
}
