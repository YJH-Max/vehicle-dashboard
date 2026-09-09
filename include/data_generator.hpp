#pragma once
#include <atomic>
#include "data_pool.hpp"

// rate_hz: 每秒生成多少帧数据
void dataGenerator(DataPool& pool, int rate_hz, const std::atomic<bool>& running);
