import native from 'libestarx_vulkan.so';

export interface NativeApi {
  createHost(): number;
  destroyHost(hostId: number): number;
  getSceneSpec(): string;
  createPageState(pageIds: number[], initialPage: number): number;
  destroyPageState(handle: number): number;
  pageSelect(handle: number, pageId: number): number[];
  pageComplete(handle: number, generation: number): number[];
  pageSettle(handle: number, pageId: number): number[];
  pageReplace(handle: number, pageIds: number[]): number[];
  pageState(handle: number): number[];
}

export const nativeApi: NativeApi = native as NativeApi;
