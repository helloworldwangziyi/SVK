import { webview } from '@kit.ArkWeb';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { VulkanShell } from '../vulkan/VulkanShell';

const TAG = 'SVK-SurfaceProbe';
const DOMAIN = 0x0000;

interface SwipeSample {
  x: number;
  time: number;
}

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
  private touchSamples: SwipeSample[] = [];
  private touchVelocityX: number = 0;
  private touchOffsetX: number = 0;
  private touchEnded: boolean = false;
  private touchDirection: string = 'none';
  private touchOwner: string = 'none';
  private webHorizontalInteractive: boolean | null = null;
  // activePage 是已落定页，pagerTarget 是正在执行的动画目标，requestedPage 是
  // 最新有效意图。连续手势基于 requestedPage 逐页推进；动画期间只合并目标，
  // 不丢弃整段手势，也不并行启动第二套分页动画。
  private pagerAnimating: boolean = false;
  private pagerTarget: number = 0;
  private requestedPage: number = 0;
  private pagerStartedAt: number = 0;
  private pagerGeneration: number = 0;
  private pagerScheduled: boolean = false;
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
    this.touchSamples = [];
    this.touchVelocityX = 0;
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
        page !== this.touchPage) {
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
    const fling = Math.abs(offset) > 12 && Math.abs(this.touchVelocityX) >= 800;
    const shouldSwitch = this.touchOwner === 'tab' &&
      (Math.abs(offset) >= 40 || fling);
    const direction = fling ? this.touchVelocityX : offset;
    const target = Math.max(0, Math.min(4, this.requestedPage + (direction < 0 ? 1 : -1)));
    this.invalidateTouch('up');
    if (shouldSwitch) {
      this.selectPage(target);
    }
  }

  private sampleTouch(x: number): void {
    const now = Date.now();
    this.touchSamples.push({ x: x, time: now });
    // 用末段速度识别甩动；整段平均速度会把按住后再甩动误判为慢拖。
    // 至少保留两点，抬手前停顿会自然把速度降为零；固定上限约束批量 MOVE。
    while (this.touchSamples.length > 2 &&
      (this.touchSamples[1].time < now - 80 || this.touchSamples.length > 16)) {
      this.touchSamples.shift();
    }
    const first = this.touchSamples[0];
    this.touchVelocityX = (x - first.x) / Math.max(1, now - first.time) * 1000;
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
      if ((page !== this.activePage && page !== this.pagerTarget) || this.drawerProgress > 0) {
        hilog.debug(DOMAIN, TAG,
          `[event=gesture_ignored page=${page} active_page=${this.activePage} reason=inactive_page_or_drawer]`);
        return;
      }
      this.touchPage = page;
      this.touchId = point.id;
      // 页面可能仍在横移动画中，局部 x 会随 Surface 位移改变；方向、距离和
      // 速度均用窗口 vp，只有 DOWN 的 DOM 命中使用 Web 局部坐标。
      this.touchDownX = point.windowX;
      this.touchDownY = point.windowY;
      this.sampleTouch(point.windowX);
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
    this.touchOffsetX = point.windowX - this.touchDownX;
    const dy = point.windowY - this.touchDownY;
    this.sampleTouch(point.windowX);
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
      this.touchEnded = true;
      this.finishPageTouch();
    }
  }

  private selectPage(index: number): void {
    if (index === this.requestedPage || this.drawerProgress > 0) {
      return;
    }
    this.invalidateTouch('page_request');
    this.requestedPage = index;
    if (this.pagerAnimating) {
      hilog.debug(DOMAIN, TAG,
        `[event=page_change_queued page=${index} animation_target=${this.pagerTarget}]`);
      return;
    }
    this.startPageTransition(index);
  }

  private startPageTransition(index: number): void {
    if (index === this.activePage) {
      return;
    }
    const animate = Math.abs(index - this.activePage) === 1;
    const generation = ++this.pagerGeneration;
    this.pagerTarget = index;
    this.pagerAnimating = animate;
    this.pagerScheduled = true;
    // 相邻页执行一套动画，跨多页直接到位。落定回调不清空正在进行的触摸，
    // 因为第二次滑动可以从当前动画尚未结束时开始。
    if (!animate) {
      this.activePage = index;
    }
    // 原始 onTouch(UP) 先于 Scroll/Swiper 的内置收尾；同轮 changeIndex 会被
    // 随后的 UP 打断，绿色页快速返回时尤其明显。只延后到当前派发结束，不设
    // 冷却时间；generation 让菜单、后台和后续直接切页能撤销尚未执行的任务。
    setTimeout(() => {
      if (generation !== this.pagerGeneration) {
        return;
      }
      this.pagerScheduled = false;
      this.pagerStartedAt = Date.now();
      this.swiperController.changeIndex(index, animate);
      hilog.info(DOMAIN, TAG,
        `[event=page_change_requested page=${index} animate=${animate}]`);
    }, 0);
  }

  private finishPageTransition(index: number): void {
    if (!this.pagerAnimating || this.pagerScheduled || index !== this.pagerTarget) {
      hilog.debug(DOMAIN, TAG,
        `[event=page_animation_end_ignored page=${index} animation_target=${this.pagerTarget} reason=stale_or_duplicate]`);
      return;
    }
    this.activePage = index;
    this.pagerAnimating = false;
    hilog.info(DOMAIN, TAG,
      `[event=page_changed page=${index} elapsed_ms=${Date.now() - this.pagerStartedAt}]`);
    this.startPageTransition(this.requestedPage);
  }

  private animateDrawer(target: number): void {
    this.invalidateTouch('drawer');
    ++this.pagerGeneration;
    this.pagerScheduled = false;
    this.pagerAnimating = false;
    this.pagerTarget = this.activePage;
    this.requestedPage = this.activePage;
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
        const fling = Math.abs(event.offsetX) > 12 && Math.abs(event.velocityX) >= 800;
        if (Math.abs(event.offsetX) < 40 && !fling) {
          return;
        }
        const direction = fling ? event.velocityX : event.offsetX;
        const next = direction < 0
          ? Math.min(4, this.requestedPage + 1)
          : Math.max(0, this.requestedPage - 1);
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
    ++this.pagerGeneration;
    this.pagerScheduled = false;
    // 退后台时落定最新已确认的目标，避免不可见期间继续消费排队请求，或恢复后
    // 等待已经失效的动画回调。尚未抬手的触摸已在上面取消，不产生新切页意图。
    this.pagerAnimating = false;
    this.pagerTarget = this.requestedPage;
    this.activePage = this.requestedPage;
    this.swiperController.changeIndex(this.activePage, false);
  }

  aboutToDisappear(): void {
    this.invalidateTouch('destroy');
    ++this.pagerGeneration;
    this.pagerScheduled = false;
    this.pagerAnimating = false;
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
            // 显式使用有界时长曲线，避免默认弹簧收尾延长连续切页的等待时间。
            .duration(160)
            .curve(Curve.EaseOut)
            // 从 DOWN 到 UP 不修改命中方式。网页继续处理其原生滚动/Canvas，
            // 观察者仅在普通区域的横滑结束后 changeIndex，不另起滚动或惯性。
            .disableSwipe(true)
            .onAnimationEnd((index: number) => this.finishPageTransition(index))
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
