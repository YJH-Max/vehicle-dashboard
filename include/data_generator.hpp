#pragma once
#include <atomic>
#include "data_pool.hpp"
#include "topic_bus.hpp"

void dataGenerator(DataPool& pool,
                   TopicBus<DataPoint>& bus,
                   int rate_hz,
                   const std::atomic<bool>& running);
