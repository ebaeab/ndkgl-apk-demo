/*
 * ui_strings.h —— 日志查看器 UI 用到的所有字符串常量
 *
 * 作用:
 *   1. logview_ui.c 用这些常量渲染界面文字;
 *   2. tools/genfont.c 扫描这些常量, 自动提取所有非 ASCII 字符,
 *      用 FreeType 光栅化成位图字形写入 jni/font_bitmap.h。
 *
 * 约定: 只要在这里新增了中文文案, 必须重新运行 tools/genfont.c 重新生成
 *       font_bitmap.h, 否则新字符会缺字形(显示为方块)。
 */
#ifndef UI_STRINGS_H
#define UI_STRINGS_H

/* ---- 工具栏按钮 ---- */
#define S_BTN_OPEN      "打开"
#define S_BTN_PARSE     "运行解析"
#define S_BTN_STOP      "停止"
#define S_BTN_SEARCH    "搜索"
#define S_BTN_HELP      "帮助"

/* ---- 子窗口标题 ---- */
#define S_TITLE_LOG     "日志显示"
#define S_TITLE_SEARCH  "搜索关键字"

/* ---- 状态栏 ---- */
#define S_STATUS_PREFIX "状态:"
#define S_STATUS_READY  "就绪"
#define S_STATUS_LOADED "已载入日志"
#define S_STATUS_RUN    "解析中"
#define S_STATUS_STOP   "已停止"

/* ---- 日志窗口占位 / 空匹配 ---- */
#define S_EMPTY_LOG     "请点击 打开 载入日志"
#define S_HINT_PARSE    "已载入, 点击 运行解析 显示"
#define S_NO_MATCH      "无匹配行"

/* ---- 搜索窗口提示 ---- */
#define S_SEARCH_HINT   "输入关键字过滤"
#define S_MATCH_FMT     "匹配 %d 行"

/* ---- 帮助面板 ---- */
#define S_HELP_TITLE    "帮助"
#define S_HELP_L1       "打开 : 载入日志文件"
#define S_HELP_L2       "运行解析 : 开始解析显示"
#define S_HELP_L3       "停止 : 停止解析"
#define S_HELP_L4       "搜索 : 关键字过滤显示"
#define S_HELP_L5       "帮助 : 显示本说明"
#define S_HELP_L6       "点击空白处关闭"

/* ---- 演示日志内容(模拟一个日志文件) ---- */
#define S_LOG_0 "--------- beginning of main"
#define S_LOG_1 "01-01 00:00:00.001  1000  1000 I NDKGL  : GL renderer = Mali-G77"
#define S_LOG_2 "01-01 00:00:00.012  1000  1000 I NDKGL  : 着色器编译成功"
#define S_LOG_3 "01-01 00:00:00.020  1000  1000 I NDKGL  : 载入字体纹理 16x16"
#define S_LOG_4 "01-01 00:00:00.105  1000  1000 W NDKGL  : 帧率下降"
#define S_LOG_5 "01-01 00:00:00.210  1000  1000 E NDKGL  : 分配缓冲失败"
#define S_LOG_6 "01-01 00:00:00.211  1000  1000 E NDKGL  : 加载模块失败"
#define S_LOG_7 "01-01 00:01:00.000  1000  1000 I NDKGL  : 解析开始"
#define S_LOG_8 "01-01 00:01:00.002  1000  1000 I NDKGL  : 关键字过滤 = error"
#define S_LOG_9 "01-01 00:01:00.500  1000  1000 I NDKGL  : 停止解析"

#endif /* UI_STRINGS_H */
