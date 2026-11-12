import { webview } from '@kit.ArkWeb';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { VulkanShell } from '../vulkan/VulkanShell';
import { LinkedLayoutController } from './LinkedLayoutController';
import {
  LinkedSceneSpecModel,
  NativeSurfaceModel,
  PageBinding,
  PlatformSurfaceModel,
  SurfaceState,
  WebSurfaceModel,
} from './Models';
import { nativeApi } from './NativeBridge';

const TAG = 'SVK-SurfaceProbe';
const DOMAIN = 0x0000;

interface SwipeSample {
  x: number;
  time: number;
}

/** 鸿蒙联动父容器：成员由宿主持有，Swiper 只执行分页，纵向保留自然嵌套滚动。 */
@Component
export struct LinkedLayout {
  @Prop spec: LinkedSceneSpecModel | null = null;
  @Prop controller: LinkedLayoutController = new LinkedLayoutController();
  private outerScroller: Scroller = new Scroller();
  private swiperController: SwiperController = new SwiperController();
  private topBarHeight: number = 56;
  private tabHeight: number = 56;
  private touchGeneration: number = 0;
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
  private pagerAnimating: boolean = false;
  private pagerTarget: number = 0;
  private requestedPage: number = 0;
  private pagerStartedAt: number = 0;
  private pagerGeneration: number = 0;
  private pagerScheduled: boolean = false;
  @State private pages: PageBinding[] = [];
  @State private activePage: number = 0;
  @State private drawerProgress: number = 0;
  @State private sceneWidth: number = 360;
  @State private sceneHeight: number = 640;

  aboutToAppear(): void {
    if (!this.spec) return;
    this.pages = this.spec.pages;
    this.activePage = Math.max(0, this.indexOfPage(this.spec.initialPage));
    this.requestedPage = this.activePage;
    this.pagerTarget = this.activePage;
    this.bindMember(this.spec.header);
    if (this.spec.drawer) this.bindMember(this.spec.drawer);
    this.pages.forEach(page => this.bindMember(page.surface));
    this.controller.pageStateHandle = nativeApi.createPageState(
      this.pageIds(this.pages), this.spec.initialPage);
    this.controller.onHide = () => this.handlePageHide();
    this.controller.onClose = () => this.closeLayout();
    this.controller.onReplace = (pages: PageBinding[]) => this.replacePages(pages);
  }

  private contentHeight(): number {
    return Math.max(1, this.sceneHeight - this.topBarHeight);
  }

  private drawerWidth(): number {
    return Math.max(1, this.sceneWidth * 0.25);
  }

  private webHeight(): number {
    return Math.max(1, this.contentHeight() - this.tabHeight);
  }

  private pageIds(pages: PageBinding[]): number[] {
    return pages.map(page => page.pageId);
  }

  private indexOfPage(pageId: number): number {
    return this.pages.findIndex(page => page.pageId === pageId);
  }

  private bindMember(surface: PlatformSurfaceModel): void {
    if (!this.spec || surface.layoutId !== 0 && surface.layoutId !== this.spec.hostId) {
      throw new Error('surface already bound to another layout');
    }
    surface.layoutId = this.spec.hostId;
    surface.mount();
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

  private probeWebHorizontal(surface: WebSurfaceModel, pageId: number,
    x: number, y: number): void {
    const generation = this.touchGeneration;
    const session = surface.documentSession;
    const script = '(function(){var v=window.visualViewport;var scale=' +
      '(v?v.width:window.innerWidth)/' + this.sceneWidth + ';' +
      'var e=document.elementFromPoint(' + x + '*scale,' + y + '*scale);' +
      'if(!e)return true;while(e){var s=getComputedStyle(e);' +
      'var marker=(e.id+" "+String(e.className)).toLowerCase();' +
      'if(e.tagName==="CANVAS"||e.tagName==="IFRAME"||marker.indexOf("chart")>=0)return true;' +
      'if(e.scrollWidth>e.clientWidth+1&&(s.overflowX==="auto"||' +
      's.overflowX==="scroll"||e===document.documentElement))return true;' +
      'e=e.parentElement;}return false;})()';
    surface.controller.runJavaScript(script).then((value: string) => {
      if (generation !== this.touchGeneration || session !== surface.documentSession ||
        this.indexOfPage(pageId) !== this.touchPage) {
        hilog.debug(DOMAIN, TAG,
          `[event=web_horizontal_probe_stale page=${pageId} session_id=${session} generation=${generation}]`);
        return;
      }
      this.webHorizontalInteractive = value !== 'false';
      hilog.debug(DOMAIN, TAG,
        `[event=web_horizontal_probe page=${pageId} session_id=${session} generation=${generation} interactive=${this.webHorizontalInteractive}]`);
      this.resolveTouchOwner();
    }).catch(() => {
      if (generation !== this.touchGeneration || session !== surface.documentSession) return;
      this.webHorizontalInteractive = true;
      hilog.warn(DOMAIN, TAG,
        `[event=web_horizontal_probe_failed page=${pageId} session_id=${session} reason=javascript_failed action=keep_web_gesture]`);
      this.resolveTouchOwner();
    });
  }

  private resolveTouchOwner(): void {
    if (this.touchOwner !== 'pending' || this.touchDirection !== 'horizontal' ||
      this.webHorizontalInteractive === null) return;
    this.touchOwner = this.webHorizontalInteractive ? 'web_horizontal' : 'tab';
    hilog.debug(DOMAIN, TAG,
      `[event=gesture_lock page=${this.touchPage} generation=${this.touchGeneration} gesture_owner=${this.touchOwner}]`);
    if (this.touchEnded) this.finishPageTouch();
  }

  private finishPageTouch(): void {
    if (this.touchOwner === 'pending' && this.touchDirection === 'horizontal') return;
    const offset = this.touchOffsetX;
    const fling = Math.abs(offset) > 12 && Math.abs(this.touchVelocityX) >= 800;
    const shouldSwitch = this.touchOwner === 'tab' && (Math.abs(offset) >= 40 || fling);
    const direction = fling ? this.touchVelocityX : offset;
    const target = Math.max(0, Math.min(this.pages.length - 1,
      this.requestedPage + (direction < 0 ? 1 : -1)));
    this.invalidateTouch('up');
    if (shouldSwitch) this.selectPage(target);
  }

  private sampleTouch(x: number): void {
    const now = Date.now();
    this.touchSamples.push({ x: x, time: now });
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
    if (!point || page < 0 || page >= this.pages.length) return;
    if (event.type === TouchType.Down) {
      this.invalidateTouch('down');
      if ((page !== this.activePage && page !== this.pagerTarget) || this.drawerProgress > 0) {
        hilog.debug(DOMAIN, TAG,
          `[event=gesture_ignored page=${page} active_page=${this.activePage} reason=inactive_page_or_drawer]`);
        return;
      }
      this.touchPage = page;
      this.touchId = point.id;
      this.touchDownX = point.windowX;
      this.touchDownY = point.windowY;
      this.sampleTouch(point.windowX);
      this.touchOffsetX = 0;
      this.touchOwner = 'pending';
      if (controller) {
        this.probeWebHorizontal(this.pages[page].web(), this.pages[page].pageId, point.x, point.y);
      } else {
        this.webHorizontalInteractive = false;
      }
      return;
    }
    if (this.touchPage !== page || this.touchId !== point.id) return;
    this.touchOffsetX = point.windowX - this.touchDownX;
    const dy = point.windowY - this.touchDownY;
    this.sampleTouch(point.windowX);
    if (this.touchDirection === 'none' && Math.max(Math.abs(this.touchOffsetX), Math.abs(dy)) > 8) {
      this.touchDirection = Math.abs(this.touchOffsetX) > Math.abs(dy) ? 'horizontal' : 'vertical';
      if (this.touchDirection === 'vertical') {
        this.touchOwner = 'vertical';
        hilog.debug(DOMAIN, TAG,
          `[event=gesture_lock page=${this.touchPage} generation=${this.touchGeneration} gesture_owner=vertical]`);
      }
    }
    this.resolveTouchOwner();
    if (event.type === TouchType.Up) {
      this.touchEnded = true;
      this.finishPageTouch();
    }
  }

  private selectPage(index: number): void {
    if (index < 0 || index >= this.pages.length || this.drawerProgress > 0) return;
    this.handlePageTransition(nativeApi.pageSelect(
      this.controller.pageStateHandle, this.pages[index].pageId));
  }

  private handlePageTransition(transition: number[]): void {
    if (!transition || transition.length < 11 || transition[0] === 0) return;
    this.pagerTarget = transition[2];
    this.activePage = transition[3];
    this.requestedPage = transition[4];
    switch (transition[0]) {
      case 1:
        hilog.debug(DOMAIN, TAG,
          `[event=page_change_queued page=${this.requestedPage} animation_target=${this.pagerTarget}]`);
        break;
      case 2:
        this.executeImmediateTransition(transition);
        break;
      case 3:
        this.startPageTransition(transition);
        break;
      case 4:
        this.pagerAnimating = false;
        hilog.info(DOMAIN, TAG, `[event=page_changed page=${this.activePage}]`);
        break;
      default:
        break;
    }
  }

  private executeImmediateTransition(transition: number[]): void {
    const target = transition[2];
    const generation = transition[5];
    this.pagerGeneration = generation;
    this.pagerAnimating = false;
    this.pagerScheduled = true;
    setTimeout(() => {
      if (generation !== this.pagerGeneration) return;
      this.pagerScheduled = false;
      this.swiperController.changeIndex(target, false);
      hilog.info(DOMAIN, TAG, `[event=page_changed page=${target} animate=false]`);
    }, 0);
  }

  private startPageTransition(transition: number[]): void {
    const target = transition[2];
    const generation = transition[5];
    this.pagerTarget = target;
    this.pagerGeneration = generation;
    this.pagerAnimating = true;
    this.pagerScheduled = true;
    // changeIndex 必须晚于当前 UP 派发，避免同轮系统 UP 打断新动画。
    setTimeout(() => {
      if (generation !== this.pagerGeneration) return;
      this.pagerScheduled = false;
      this.pagerStartedAt = Date.now();
      this.swiperController.changeIndex(target, true);
      hilog.info(DOMAIN, TAG, `[event=page_change_requested page=${target} animate=true]`);
    }, 0);
  }

  private finishPageTransition(index: number): void {
    if (!this.pagerAnimating || this.pagerScheduled || index !== this.pagerTarget) {
      hilog.debug(DOMAIN, TAG,
        `[event=page_animation_end_ignored page=${index} animation_target=${this.pagerTarget} reason=stale_or_duplicate]`);
      return;
    }
    this.pagerAnimating = false;
    hilog.info(DOMAIN, TAG,
      `[event=page_animation_completed page=${index} elapsed_ms=${Date.now() - this.pagerStartedAt}]`);
    this.handlePageTransition(nativeApi.pageComplete(
      this.controller.pageStateHandle, this.pagerGeneration));
  }

  private settlePage(index: number, reason: string): void {
    if (index < 0 || index >= this.pages.length) return;
    const state = nativeApi.pageSettle(this.controller.pageStateHandle, this.pages[index].pageId);
    this.pagerScheduled = false;
    this.pagerAnimating = false;
    this.pagerGeneration = state[3];
    this.activePage = this.indexOfPage(state[0]);
    this.pagerTarget = this.indexOfPage(state[1]);
    this.requestedPage = this.indexOfPage(state[2]);
    this.swiperController.changeIndex(this.activePage, false);
    hilog.debug(DOMAIN, TAG, `[event=page_animation_settled page=${this.activePage} reason=${reason}]`);
  }

  private replacePages(replacements: PageBinding[]): boolean {
    if (!this.spec || replacements.length === 0) return false;
    const pageIds = new Set<number>();
    const surfaceIds = new Set<number>();
    for (const page of replacements) {
      if (page.pageId === 0 || page.surface.ref.id === 0 ||
        page.surface.state === SurfaceState.Closed || pageIds.has(page.pageId) ||
        surfaceIds.has(page.surface.ref.id) || page.surface.ref.id === this.spec.header.ref.id ||
        (this.spec.drawer && page.surface.ref.id === this.spec.drawer.ref.id) ||
        (page.surface.layoutId !== 0 && page.surface.layoutId !== this.spec.hostId)) {
        return false;
      }
      pageIds.add(page.pageId);
      surfaceIds.add(page.surface.ref.id);
    }
    const state = nativeApi.pageReplace(this.controller.pageStateHandle, this.pageIds(replacements));
    if (!state || state.length < 6 || state[5] === 0) return false;
    this.invalidateTouch('replace_pages');
    this.pagerScheduled = false;
    this.pagerAnimating = false;
    this.pagerGeneration = state[3];
    this.pages.forEach(page => {
      if (!surfaceIds.has(page.surface.ref.id)) {
        page.surface.layoutId = 0;
        page.surface.suspend();
      }
    });
    replacements.forEach(page => this.bindMember(page.surface));
    this.pages = [...replacements];
    this.activePage = this.indexOfPage(state[0]);
    this.pagerTarget = this.indexOfPage(state[1]);
    this.requestedPage = this.indexOfPage(state[2]);
    setTimeout(() => this.swiperController.changeIndex(this.activePage, false), 0);
    return true;
  }

  private animateDrawer(target: number): void {
    if (!this.spec?.drawer) return;
    this.invalidateTouch('drawer');
    this.settlePage(this.activePage, 'drawer');
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
        if (Math.abs(event.offsetX) < 40 && !fling) return;
        const direction = fling ? event.velocityX : event.offsetX;
        const next = direction < 0
          ? Math.min(this.pages.length - 1, this.requestedPage + 1)
          : Math.max(0, this.requestedPage - 1);
        this.selectPage(next);
      });
  }

  @Builder
  private webPage(page: PageBinding) {
    Column() {
      Web({ src: page.web().url, controller: page.web().controller })
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
        .onTouch((event: TouchEvent) => this.observePageTouch(
          event, this.indexOfPage(page.pageId), page.web().controller))
        .onPageBegin(() => {
          ++page.web().documentSession;
          if (this.indexOfPage(page.pageId) === this.touchPage) {
            this.invalidateTouch('web_navigation');
          }
        })
        .onPageEnd(() => {
          hilog.info(DOMAIN, TAG,
            `[event=web_loaded page=${page.pageId} session_id=${page.web().documentSession} height=${this.webHeight()}]`);
        })
        .onErrorReceive(() => {
          hilog.warn(DOMAIN, TAG, `[event=web_load_failed page=${page.pageId}]`);
        })
    }
    .width('100%')
    .height(this.contentHeight())
  }

  @Builder
  private nativePage(page: PageBinding) {
    Scroll(page.scroller) {
      VulkanShell({
        hostId: this.spec?.hostId ?? 0,
        generation: page.surface.ref.instanceGeneration,
        contentId: page.native().contentId
      })
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
    .onTouch((event: TouchEvent) => this.observePageTouch(
      event, this.indexOfPage(page.pageId), null))
  }

  @Builder
  private tabButton(title: string, page: number) {
    Button(title)
      .layoutWeight(1)
      .fontSize(12)
      .fontColor(this.activePage === page ? Color.Red : Color.Gray)
      .onClick(() => this.selectPage(page))
  }

  private handlePageHide(): void {
    this.invalidateTouch('page_hide');
    this.settlePage(this.requestedPage, 'page_hide');
  }

  private closeLayout(): void {
    this.invalidateTouch('close');
    this.pagerScheduled = false;
    this.pagerAnimating = false;
    ++this.pagerGeneration;
    if (this.spec) {
      this.pages.forEach(page => {
        page.surface.layoutId = 0;
        page.surface.suspend();
      });
      this.spec.header.layoutId = 0;
      if (this.spec.drawer) this.spec.drawer.layoutId = 0;
    }
  }

  aboutToDisappear(): void {
    this.controller.close();
  }

  build() {
    Stack({ alignContent: Alignment.TopStart }) {
      Stack({ alignContent: Alignment.TopStart }) {
        Scroll(this.outerScroller) {
          Column() {
            Stack({ alignContent: Alignment.Bottom }) {
              VulkanShell({
                hostId: this.spec?.hostId ?? 0,
                generation: this.spec?.header.ref.instanceGeneration ?? 0,
                contentId: this.spec?.header.contentId ?? 0
              })
                .width('100%')
                .height(this.contentHeight())
              Row() {
                ForEach(this.pages, (page: PageBinding, index: number) => {
                  this.tabButton(page.title, index)
                }, (page: PageBinding) => page.pageId.toString())
              }
              .width('100%')
              .height(this.tabHeight)
              .backgroundColor(Color.White)
            }
            .width('100%')
            .height(this.contentHeight())
            .onTouch((event: TouchEvent) => {
              if (event.type === TouchType.Down) this.invalidateTouch('header_down');
            })
            .priorityGesture(this.horizontalTabGesture())

            Swiper(this.swiperController) {
              ForEach(this.pages, (page: PageBinding) => {
                if (page.isWeb()) {
                  this.webPage(page)
                } else {
                  this.nativePage(page)
                }
              }, (page: PageBinding) => page.pageId.toString())
            }
            .width('100%')
            .height(this.contentHeight())
            .vertical(false)
            .loop(false)
            .indicator(false)
            .cachedCount(Math.max(1, this.pages.length - 1))
            .duration(160)
            .curve(Curve.EaseOut)
            .disableSwipe(true)
            .onAnimationEnd((index: number) => this.finishPageTransition(index))
          }
        }
        .width('100%')
        .height(this.contentHeight() + this.tabHeight)
        .position({ x: 0, y: this.topBarHeight })
        .scrollable(ScrollDirection.Vertical)
        .scrollBar(BarState.Off)
        .edgeEffect(EdgeEffect.None)

        Row() {
          Text('')
            .layoutWeight(1)
          if (this.spec?.drawer) {
            Button('MENU')
              .width(88)
              .height('100%')
              .onClick(() => this.animateDrawer(1))
          }
        }
        .width('100%')
        .height(this.topBarHeight)
        .backgroundColor(Color.White)
        .position({ x: 0, y: 0 })
        .onTouch((event: TouchEvent) => {
          if (event.type === TouchType.Down) this.invalidateTouch('top_bar_down');
        })
        .priorityGesture(this.horizontalTabGesture())
      }
      .width('100%')
      .height('100%')
      .enabled(this.drawerProgress === 0)
      .translate({ x: -this.drawerWidth() * this.drawerProgress, y: 0 })

      if (this.drawerProgress > 0 && this.spec?.drawer) {
        Column()
          .width('100%')
          .height('100%')
          .backgroundColor(Color.Transparent)
          .translate({ x: -this.drawerWidth() * this.drawerProgress, y: 0 })
          .onClick(() => this.animateDrawer(0))
      }

      if (this.spec?.drawer) {
        Column() {
          VulkanShell({
            hostId: this.spec.hostId,
            generation: this.spec.drawer.ref.instanceGeneration,
            contentId: this.spec.drawer.contentId
          })
            .width('100%')
            .height('100%')
        }
        .width(this.drawerWidth())
        .height(this.sceneHeight)
        .position({ x: this.sceneWidth - this.drawerWidth() * this.drawerProgress, y: 0 })
      }
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
