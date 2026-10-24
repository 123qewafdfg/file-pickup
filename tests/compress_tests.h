#pragma once

class QTextStream;

// 压缩 / 校验自测：返回失败用例数，失败详情写入 out
int runCompressionTests(QTextStream &out, int *passed);
