/** 单个动态 XComponent；ID 同时携带宿主、实例代次和 App 内容身份。 */
@Component
export struct VulkanShell {
  @Prop hostId: number = 0;
  @Prop generation: number = 0;
  @Prop contentId: number = 0;

  build() {
    XComponent({
      id: `svk:${this.hostId}:${this.generation}:${this.contentId}`,
      type: XComponentType.SURFACE,
      libraryname: 'estarx_vulkan'
    })
      .width('100%')
      .height('100%')
  }
}
