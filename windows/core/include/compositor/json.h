#pragma once
#include "nlohmann/json.hpp"

namespace comp {
// 有序 JSON：保持键的顺序，便于对照原文件。
using Json = nlohmann::ordered_json;
}
