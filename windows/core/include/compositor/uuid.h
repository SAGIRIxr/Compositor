#pragma once
#include <string>

namespace comp {
// 随机 v4 UUID，大写，与 macOS 的 uuidString 格式一致。
std::string makeUUID();
// 校验并规范化（转大写）。无效时返回空串。
std::string normalizeUUID(const std::string& text);
} // namespace comp
