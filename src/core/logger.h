#pragma once

#include <QString>

namespace cj {

// 全链路诊断日志：只记阶段、返回码、长度、选择器命中情况，
// 绝不记录 cookie、密钥、令牌、文件内容。
namespace logger {

void event(const QString &stage, const QString &detail = QString());
QString path();
// 清空日志（每次开始一次发放时调用）
void reset();

} // namespace logger

} // namespace cj
