import { VulkanShell } from '../vulkan/VulkanShell';

@Entry
@Component
struct Index {
  build() {
    Stack({ alignContent: Alignment.TopStart }) {
      VulkanShell()
    }
    .width('100%')
    .height('100%')
    .expandSafeArea([SafeAreaType.SYSTEM, SafeAreaType.CUTOUT],
      [SafeAreaEdge.TOP, SafeAreaEdge.BOTTOM, SafeAreaEdge.START, SafeAreaEdge.END])
  }
}
