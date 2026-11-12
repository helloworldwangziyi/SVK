import { LinkedLayout } from './LinkedLayout';
import { LinkedLayoutController } from './LinkedLayoutController';
import {
  LinkedSceneSpecModel,
  NativeSurfaceModel,
  PageBinding,
  PlatformSurfaceModel,
  SurfaceFactory,
  WebSurfaceModel,
} from './Models';
import { nativeApi } from './NativeBridge';

/** 通用鸿蒙宿主：App C++ 决定成员和组合，Ability 只转发页面生命周期。 */
@Component
export struct SurfaceHost {
  @State private spec: LinkedSceneSpecModel | null = null;
  @Prop controller: LinkedLayoutController = new LinkedLayoutController();
  private hostId: number = 0;
  private factory?: SurfaceFactory;
  private members: PlatformSurfaceModel[] = [];

  aboutToAppear(): void {
    this.hostId = nativeApi.createHost();
    this.factory = new SurfaceFactory(this.hostId);
    try {
      const root = JSON.parse(nativeApi.getSceneSpec()) as Record<string, Object>;
      const headerJson = root['header'] as Record<string, Object>;
      const header = this.createNative(headerJson['id'] as number,
        headerJson['contentId'] as number);
      let drawer: NativeSurfaceModel | null = null;
      if (root['drawer']) {
        const drawerJson = root['drawer'] as Record<string, Object>;
        drawer = this.createNative(drawerJson['id'] as number,
          drawerJson['contentId'] as number);
      }
      const pages: PageBinding[] = [];
      const pageJson = root['pages'] as Object[];
      pageJson.forEach((value: Object) => {
        const page = value as Record<string, Object>;
        const surfaceId = page['surfaceId'] as number;
        const surface = page['url'] !== undefined
          ? this.createWeb(surfaceId, page['url'] as string)
          : this.createNative(surfaceId, page['contentId'] as number);
        pages.push(new PageBinding(page['pageId'] as number, page['title'] as string, surface));
      });
      this.spec = new LinkedSceneSpecModel(this.hostId, header, drawer, pages,
        root['initialPage'] as number);
    } catch (error) {
      this.close();
      throw error;
    }
  }

  aboutToDisappear(): void {
    this.close();
  }

  onPageHide(): void {
    this.controller.hide();
  }

  createWeb(surfaceId: number, url: string): WebSurfaceModel {
    if (!this.factory) throw new Error('host is not ready');
    const surface = this.factory.createWeb(surfaceId, url);
    this.members.push(surface);
    return surface;
  }

  createNative(surfaceId: number, contentId: number): NativeSurfaceModel {
    if (!this.factory) throw new Error('host is not ready');
    const surface = this.factory.createNative(surfaceId, contentId);
    this.members.push(surface);
    return surface;
  }

  createWebPage(pageId: number, title: string, surfaceId: number, url: string): PageBinding {
    return new PageBinding(pageId, title, this.createWeb(surfaceId, url));
  }

  createNativePage(pageId: number, title: string, surfaceId: number,
    contentId: number): PageBinding {
    return new PageBinding(pageId, title, this.createNative(surfaceId, contentId));
  }

  replacePages(pages: PageBinding[]): boolean {
    return this.controller.replace(pages);
  }

  closeSurface(surface: PlatformSurfaceModel): void {
    surface.close();
    this.members = this.members.filter(member => member !== surface);
  }

  close(): void {
    this.controller.close();
    this.members.forEach(member => member.close());
    this.members = [];
    if (this.hostId > 0) {
      nativeApi.destroyHost(this.hostId);
      this.hostId = 0;
    }
  }

  build() {
    if (this.spec) {
      LinkedLayout({ spec: this.spec, controller: this.controller })
    }
  }
}
