/* include_demo_epilogue.cpp —— `%epilogue include` 进来的函数定义
 *
 * 这段贴在 parser.c / parser.cpp 末尾（与手写的 %epilogue 段一模一样），
 * 可以放非 inline 的函数定义。
 */

double IncludeDemoScale(double value)
{
    return value * 2.0;
}
