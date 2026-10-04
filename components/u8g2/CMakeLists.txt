# u8g2 组件（olikraus/u8g2 v2.37.1，仅 csrc 纯 C 实现）
# 说明：
#   - 使用 file(GLOB)：vendor 库冻结在固定版本，新增文件需 pio run -t fullclean 才被扫描，
#     IDF 官方不推荐 GLOB 是出于增量构建考虑，此处可接受。
#   - 未用到的显示驱动/字体经链接器 gc-sections 裁剪，不影响最终固件体积。
#   - 字体全部编入（u8g2_fonts.c 较大），首次编译较慢，后续增量缓存。
file(GLOB U8G2_SRCS "${CMAKE_CURRENT_LIST_DIR}/csrc/*.c")

idf_component_register(
    SRCS ${U8G2_SRCS}
    INCLUDE_DIRS "csrc"
)
