import { webview } from '@kit.ArkWeb';

export enum SurfaceState {
  Created,
  Mounted,
  Suspended,
  Closed,
}

export class SurfaceRef {
  constructor(public id: number, public instanceGeneration: number) {}
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
  documentSession: number = 0;

  constructor(ref: SurfaceRef, public url: string) {
    super(ref);
  }
}

export class NativeSurfaceModel extends PlatformSurfaceModel {
  constructor(ref: SurfaceRef, public hostId: number, public contentId: number) {
    super(ref);
  }
}

export class PageBinding {
  contentOffset: number = 0;
  readonly scroller: Scroller = new Scroller();

  constructor(public pageId: number, public title: string, public surface: PlatformSurfaceModel) {}

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
  constructor(
    public hostId: number,
    public header: NativeSurfaceModel,
    public drawer: NativeSurfaceModel | null,
    public pages: PageBinding[],
    public initialPage: number) {}
}

export class SurfaceFactory {
  private nextGeneration: number = 1;

  constructor(private hostId: number) {}

  createWeb(surfaceId: number, url: string): WebSurfaceModel {
    return new WebSurfaceModel(new SurfaceRef(surfaceId, this.nextGeneration++), url);
  }

  createNative(surfaceId: number, contentId: number): NativeSurfaceModel {
    return new NativeSurfaceModel(new SurfaceRef(surfaceId, this.nextGeneration++),
      this.hostId, contentId);
  }
}
