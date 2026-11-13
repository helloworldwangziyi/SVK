import { webview } from '@kit.ArkWeb';

export enum SurfaceState {
  Created,
  Mounted,
  Suspended,
  Closed,
}

export class SurfaceRef {
  id: number;
  instanceGeneration: number;

  constructor(id: number, instanceGeneration: number) {
    this.id = id;
    this.instanceGeneration = instanceGeneration;
  }
}

export class PlatformSurfaceModel {
  readonly ref: SurfaceRef;
  state: SurfaceState = SurfaceState.Created;
  layoutId: number = 0;

  constructor(ref: SurfaceRef) {
    this.ref = ref;
  }

  mount(): boolean {
    if (this.state === SurfaceState.Closed) return false;
    this.state = SurfaceState.Mounted;
    return true;
  }

  suspend(): boolean {
    if (this.state === SurfaceState.Closed) return false;
    this.state = SurfaceState.Suspended;
    return true;
  }

  resume(): boolean {
    if (this.state === SurfaceState.Closed) return false;
    this.state = SurfaceState.Mounted;
    return true;
  }

  close(): void {
    this.state = SurfaceState.Closed;
    this.layoutId = 0;
  }
}

export class WebSurfaceModel extends PlatformSurfaceModel {
  readonly controller: webview.WebviewController = new webview.WebviewController();
  // 只有 Web 组件触发 onControllerAttached 后才能执行 JS 探测。
  controllerAttached: boolean = false;
  documentSession: number = 0;
  url: string;

  constructor(ref: SurfaceRef, url: string) {
    super(ref);
    this.url = url;
  }
}

export class NativeSurfaceModel extends PlatformSurfaceModel {
  hostId: number;
  contentId: number;

  constructor(ref: SurfaceRef, hostId: number, contentId: number) {
    super(ref);
    this.hostId = hostId;
    this.contentId = contentId;
  }
}

export class PageBinding {
  contentOffset: number = 0;
  readonly scroller: Scroller = new Scroller();
  pageId: number;
  title: string;
  surface: PlatformSurfaceModel;

  constructor(pageId: number, title: string, surface: PlatformSurfaceModel) {
    this.pageId = pageId;
    this.title = title;
    this.surface = surface;
  }

  isWeb(): boolean {
    return this.surface instanceof WebSurfaceModel;
  }

  web(): WebSurfaceModel {
    return this.surface as WebSurfaceModel;
  }

  native(): NativeSurfaceModel {
    return this.surface as NativeSurfaceModel;
  }
}

export class LinkedSceneSpecModel {
  hostId: number;
  header: NativeSurfaceModel;
  drawer: NativeSurfaceModel | null;
  pages: PageBinding[];
  initialPage: number;

  constructor(hostId: number, header: NativeSurfaceModel, drawer: NativeSurfaceModel | null,
    pages: PageBinding[], initialPage: number) {
    this.hostId = hostId;
    this.header = header;
    this.drawer = drawer;
    this.pages = pages;
    this.initialPage = initialPage;
  }
}

export class SurfaceFactory {
  private nextGeneration: number = 1;
  private hostId: number;

  constructor(hostId: number) {
    this.hostId = hostId;
  }

  createWeb(surfaceId: number, url: string): WebSurfaceModel {
    return new WebSurfaceModel(new SurfaceRef(surfaceId, this.nextGeneration++), url);
  }

  createNative(surfaceId: number, contentId: number): NativeSurfaceModel {
    return new NativeSurfaceModel(new SurfaceRef(surfaceId, this.nextGeneration++),
      this.hostId, contentId);
  }
}
