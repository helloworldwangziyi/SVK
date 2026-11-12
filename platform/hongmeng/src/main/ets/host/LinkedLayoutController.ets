import { nativeApi } from './NativeBridge';
import { PageBinding } from './Models';

/** 宿主持有的布局生命周期句柄；关闭时只解绑，不销毁成员。 */
export class LinkedLayoutController {
  pageStateHandle: number = 0;
  closed: boolean = false;
  onHide?: () => void;
  onClose?: () => void;
  onReplace?: (pages: PageBinding[]) => boolean;

  close(): void {
    if (this.closed) return;
    this.closed = true;
    this.onClose?.();
    if (this.pageStateHandle > 0) {
      nativeApi.destroyPageState(this.pageStateHandle);
      this.pageStateHandle = 0;
    }
  }

  hide(): void {
    if (!this.closed) this.onHide?.();
  }

  replace(pages: PageBinding[]): boolean {
    return !this.closed && this.onReplace !== undefined && this.onReplace(pages);
  }
}
