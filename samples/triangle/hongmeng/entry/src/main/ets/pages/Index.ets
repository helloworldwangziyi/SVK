import { LinkedLayoutController, SurfaceHost } from 'svkplatform';

/** C++ App 提供场景配置；页面只负责转发宿主生命周期。 */
@Entry
@Component
struct Index {
  private linkedController: LinkedLayoutController = new LinkedLayoutController();

  onPageHide(): void {
    this.linkedController.hide();
  }

  aboutToDisappear(): void {
    this.linkedController.close();
  }

  build() {
    Stack({ alignContent: Alignment.TopStart }) {
      SurfaceHost({ controller: this.linkedController })
    }
    .width('100%')
    .height('100%')
  }
}
